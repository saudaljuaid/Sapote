// Copyright 2026 Sapote contributors
//
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// https://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

//! Bounded JBD2 transaction images and an ordered-data commit plan.
//!
//! This module deliberately stops below ext4 namespace mutation. It provides
//! checked descriptor/data/revoke/commit serialization, explicit durability
//! barriers, replay validation for one transaction, and allocation within a
//! caller-supplied clean-journal ring. It does not discover the journal inode
//! or persist live ring state. A caller must execute the returned operations in
//! order and reclaim a reservation only after its checkpoint flush.

use super::block_header::{JournalBlockHeader, JournalBlockType};
use super::commit_block::validate_commit_block_checksum;
use super::descriptor_block::{DescriptorBlockTagIter, validate_descriptor_block_checksum};
use super::revocation_block::{read_revocation_block_table, validate_revocation_block_checksum};
use super::superblock::JournalSuperblock;
use crate::checksum::Checksum;
use crate::uuid::Uuid;
use alloc::collections::VecDeque;
use alloc::vec;
use alloc::vec::Vec;
use core::error::Error;
use core::fmt::{self, Display, Formatter};

/// The only journal block size admitted by Sapote's ext4 profile.
pub const JOURNAL_BLOCK_BYTES: usize = 4096;

/// A deliberately small upper bound for one transaction's metadata images.
pub const JOURNAL_TRANSACTION_MAX_METADATA_BLOCKS: usize = 64;

/// The maximum number of block revocations represented by one transaction.
pub const JOURNAL_TRANSACTION_MAX_REVOKED_BLOCKS: usize = 64;

/// The maximum clean-journal data slots admitted by the bounded ring planner.
pub const JOURNAL_RING_MAX_SLOTS: usize = 8192;

/// A checked block image owned by a transaction or replay result.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct JournalBlockImage {
    block_index: u64,
    bytes: Vec<u8>,
}

impl JournalBlockImage {
    /// Return the absolute filesystem block targeted by this image.
    #[must_use]
    pub fn block_index(&self) -> u64 {
        self.block_index
    }

    /// Return the complete 4 KiB block image.
    #[must_use]
    pub fn bytes(&self) -> &[u8] {
        &self.bytes
    }
}

/// The durability boundary represented by a commit-plan flush operation.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum JournalFlush {
    /// Ordered file data must be durable before journal metadata is committed.
    OrderedData,
    /// Descriptor and journaled metadata must be durable before the commit.
    JournalPayload,
    /// The checksummed commit record must be durable before home metadata.
    Commit,
    /// Checkpointed home metadata must be durable before reclaiming the log.
    Checkpoint,
}

/// The kind of a block written into the journal.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum JournalRecordKind {
    /// A JBD2 descriptor containing checksummed home-block tags.
    Descriptor,
    /// A metadata image named by the corresponding descriptor tag.
    Metadata,
    /// A checksummed list of home blocks whose older images must not replay.
    Revocation,
    /// The checksummed record that makes the transaction replayable.
    Commit,
}

/// One operation in a strictly ordered JBD2 commit plan.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum JournalCommitOperation {
    /// Write file data to its final block before committing related metadata.
    WriteOrderedData(JournalBlockImage),
    /// Persist every write preceding this operation.
    Flush(JournalFlush),
    /// Write one descriptor, metadata, or commit image into the journal.
    WriteJournal {
        /// Absolute filesystem block belonging to the journal inode.
        journal_block: u64,
        /// Semantic type of this journal record.
        kind: JournalRecordKind,
        /// Complete 4 KiB record image.
        bytes: Vec<u8>,
    },
    /// Checkpoint committed metadata to its final home block.
    WriteHomeMetadata(JournalBlockImage),
}

/// A refusal produced while constructing or validating a transaction.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum JournalTransactionError {
    /// Sequence `u32::MAX` cannot be followed by another checked transaction.
    SequenceOverflow,
    /// The transaction has no metadata image to journal.
    EmptyTransaction,
    /// A transaction exceeded the fixed metadata or ordered-data bound.
    TooManyBlocks,
    /// A supplied image was not exactly one Sapote filesystem block.
    IncorrectBlockLength,
    /// A home or journal block lies beyond the admitted filesystem geometry.
    BlockOutOfRange,
    /// Two images or journal slots name the same physical block.
    DuplicateBlock,
    /// The journal slot list does not exactly fit descriptor, data, and commit.
    JournalSlotCount,
    /// A journal slot overlaps a staged home block.
    JournalSlotOverlap,
    /// A metadata image needs JBD2 magic escaping, which is not implemented.
    EscapedBlockUnsupported,
    /// A descriptor is missing, malformed, or fails its checksum.
    CorruptDescriptor,
    /// A journaled metadata image fails its descriptor-tag checksum.
    CorruptData,
    /// A complete, checksummed commit record is not present.
    MissingCommit,
    /// The commit header, sequence, or checksum is invalid.
    CorruptCommit,
    /// A revocation header, table, sequence, or checksum is invalid.
    CorruptRevocation,
    /// The clean-journal ring geometry is too small, too large, or malformed.
    RingGeometry,
    /// The ring has insufficient uncheckpointed space for a transaction.
    RingFull,
    /// A transaction was prepared with a stale sequence or different geometry.
    RingTransactionMismatch,
    /// A reservation ticket is unknown or no longer active.
    ReservationUnknown,
    /// A reservation transition was requested from the wrong durability state.
    ReservationState,
    /// Commit, abort, or checkpoint order would reclaim live journal records.
    ReservationOrder,
    /// The monotonic reservation ticket would overflow.
    ReservationOverflow,
}

impl Display for JournalTransactionError {
    fn fmt(&self, formatter: &mut Formatter<'_>) -> fmt::Result {
        let message = match self {
            Self::SequenceOverflow => "journal sequence would overflow",
            Self::EmptyTransaction => "journal transaction has no metadata",
            Self::TooManyBlocks => "journal transaction block bound exceeded",
            Self::IncorrectBlockLength => "journal image is not 4096 bytes",
            Self::BlockOutOfRange => "journal block lies outside the filesystem",
            Self::DuplicateBlock => "journal transaction repeats a block",
            Self::JournalSlotCount => "journal slot count does not fit the transaction",
            Self::JournalSlotOverlap => "journal and home blocks overlap",
            Self::EscapedBlockUnsupported => "journal magic escaping is unsupported",
            Self::CorruptDescriptor => "journal descriptor is corrupt",
            Self::CorruptData => "journal metadata image is corrupt",
            Self::MissingCommit => "durable journal commit is missing",
            Self::CorruptCommit => "journal commit is corrupt",
            Self::CorruptRevocation => "journal revocation record is corrupt",
            Self::RingGeometry => "journal ring geometry is invalid",
            Self::RingFull => "journal ring has insufficient free slots",
            Self::RingTransactionMismatch => "journal transaction does not match the ring",
            Self::ReservationUnknown => "journal reservation is not active",
            Self::ReservationState => "journal reservation state transition is invalid",
            Self::ReservationOrder => "journal reservation order is invalid",
            Self::ReservationOverflow => "journal reservation ticket would overflow",
        };
        formatter.write_str(message)
    }
}

impl Error for JournalTransactionError {}

/// A bounded, single-descriptor JBD2 transaction.
#[derive(Clone, Debug)]
pub struct JournalTransaction {
    sequence: u32,
    uuid: Uuid,
    maximum_block: u64,
    ordered_data: Vec<JournalBlockImage>,
    metadata: Vec<JournalBlockImage>,
    revoked_blocks: Vec<u64>,
}

impl JournalTransaction {
    /// Start a transaction for one admitted filesystem and journal sequence.
    ///
    /// `maximum_block` is the highest valid absolute filesystem block index,
    /// inclusive.
    pub fn new(
        sequence: u32,
        journal_uuid: [u8; 16],
        maximum_block: u64,
    ) -> Result<Self, JournalTransactionError> {
        if sequence == u32::MAX {
            return Err(JournalTransactionError::SequenceOverflow);
        }
        Ok(Self {
            sequence,
            uuid: Uuid::new(journal_uuid),
            maximum_block,
            ordered_data: Vec::new(),
            metadata: Vec::new(),
            revoked_blocks: Vec::new(),
        })
    }

    /// Stage a block revocation for an older journaled image.
    pub fn stage_revocation(&mut self, block_index: u64) -> Result<(), JournalTransactionError> {
        if self.revoked_blocks.len() >= JOURNAL_TRANSACTION_MAX_REVOKED_BLOCKS {
            return Err(JournalTransactionError::TooManyBlocks);
        }
        if block_index > self.maximum_block {
            return Err(JournalTransactionError::BlockOutOfRange);
        }
        if self.revoked_blocks.contains(&block_index) {
            return Err(JournalTransactionError::DuplicateBlock);
        }
        self.revoked_blocks.push(block_index);
        Ok(())
    }

    /// Return the exact descriptor/data/revoke/commit slot requirement.
    pub fn required_journal_slots(&self) -> Result<usize, JournalTransactionError> {
        if self.metadata.is_empty() {
            return Err(JournalTransactionError::EmptyTransaction);
        }
        self.metadata
            .len()
            .checked_add(2)
            .and_then(|count| count.checked_add(usize::from(!self.revoked_blocks.is_empty())))
            .ok_or(JournalTransactionError::TooManyBlocks)
    }

    /// Stage a complete file-data block that must precede the journal commit.
    pub fn stage_ordered_data(
        &mut self,
        block_index: u64,
        bytes: &[u8],
    ) -> Result<(), JournalTransactionError> {
        if self.ordered_data.len() >= JOURNAL_TRANSACTION_MAX_METADATA_BLOCKS {
            return Err(JournalTransactionError::TooManyBlocks);
        }
        self.stage(block_index, bytes, false)
    }

    /// Stage a complete metadata block for descriptor/data journaling.
    pub fn stage_metadata(
        &mut self,
        block_index: u64,
        bytes: &[u8],
    ) -> Result<(), JournalTransactionError> {
        if self.metadata.len() >= JOURNAL_TRANSACTION_MAX_METADATA_BLOCKS {
            return Err(JournalTransactionError::TooManyBlocks);
        }
        self.stage(block_index, bytes, true)
    }

    fn stage(
        &mut self,
        block_index: u64,
        bytes: &[u8],
        metadata: bool,
    ) -> Result<(), JournalTransactionError> {
        if bytes.len() != JOURNAL_BLOCK_BYTES {
            return Err(JournalTransactionError::IncorrectBlockLength);
        }
        if block_index > self.maximum_block {
            return Err(JournalTransactionError::BlockOutOfRange);
        }
        if self
            .ordered_data
            .iter()
            .chain(self.metadata.iter())
            .any(|image| image.block_index == block_index)
        {
            return Err(JournalTransactionError::DuplicateBlock);
        }
        if metadata && bytes[..4] == JournalBlockHeader::MAGIC.to_be_bytes() {
            return Err(JournalTransactionError::EscapedBlockUnsupported);
        }
        let image = JournalBlockImage {
            block_index,
            bytes: Vec::from(bytes),
        };
        if metadata {
            self.metadata.push(image);
        } else {
            self.ordered_data.push(image);
        }
        Ok(())
    }

    /// Build the only legal ordered-data execution sequence for this image.
    ///
    /// `journal_blocks` must contain exactly one descriptor slot, one slot for
    /// each metadata image, an optional revoke slot, and one final commit slot.
    /// The returned operations intentionally place the commit flush before
    /// every home-metadata write.
    pub fn commit_plan(
        &self,
        journal_blocks: &[u64],
    ) -> Result<Vec<JournalCommitOperation>, JournalTransactionError> {
        let required_slots = self.required_journal_slots()?;
        if journal_blocks.len() != required_slots {
            return Err(JournalTransactionError::JournalSlotCount);
        }
        for (index, block) in journal_blocks.iter().enumerate() {
            if *block > self.maximum_block {
                return Err(JournalTransactionError::BlockOutOfRange);
            }
            if journal_blocks[..index].contains(block) {
                return Err(JournalTransactionError::DuplicateBlock);
            }
            if self
                .ordered_data
                .iter()
                .chain(self.metadata.iter())
                .any(|image| image.block_index == *block)
                || self.revoked_blocks.contains(block)
            {
                return Err(JournalTransactionError::JournalSlotOverlap);
            }
        }

        let mut operations = Vec::new();
        operations
            .try_reserve_exact(
                self.ordered_data.len()
                    + self.metadata.len() * 2
                    + usize::from(!self.revoked_blocks.is_empty())
                    + 7,
            )
            .map_err(|_| JournalTransactionError::TooManyBlocks)?;
        operations.extend(
            self.ordered_data
                .iter()
                .cloned()
                .map(JournalCommitOperation::WriteOrderedData),
        );
        if !self.ordered_data.is_empty() {
            operations.push(JournalCommitOperation::Flush(JournalFlush::OrderedData));
        }
        operations.push(JournalCommitOperation::WriteJournal {
            journal_block: journal_blocks[0],
            kind: JournalRecordKind::Descriptor,
            bytes: self.descriptor_block(),
        });
        for (index, image) in self.metadata.iter().enumerate() {
            operations.push(JournalCommitOperation::WriteJournal {
                journal_block: journal_blocks[index + 1],
                kind: JournalRecordKind::Metadata,
                bytes: image.bytes.clone(),
            });
        }
        let mut commit_slot = self.metadata.len() + 1;
        if !self.revoked_blocks.is_empty() {
            operations.push(JournalCommitOperation::WriteJournal {
                journal_block: journal_blocks[commit_slot],
                kind: JournalRecordKind::Revocation,
                bytes: self.revocation_block(),
            });
            commit_slot += 1;
        }
        operations.push(JournalCommitOperation::Flush(JournalFlush::JournalPayload));
        operations.push(JournalCommitOperation::WriteJournal {
            journal_block: journal_blocks[commit_slot],
            kind: JournalRecordKind::Commit,
            bytes: self.commit_block(),
        });
        operations.push(JournalCommitOperation::Flush(JournalFlush::Commit));
        operations.extend(
            self.metadata
                .iter()
                .cloned()
                .map(JournalCommitOperation::WriteHomeMetadata),
        );
        operations.push(JournalCommitOperation::Flush(JournalFlush::Checkpoint));
        Ok(operations)
    }

    fn descriptor_block(&self) -> Vec<u8> {
        const UUID_OMITTED: u32 = 0x2;
        const LAST_TAG: u32 = 0x8;
        const TAG_BYTES: usize = 16;

        let mut block = vec![0; JOURNAL_BLOCK_BYTES];
        write_u32be(&mut block, 0, JournalBlockHeader::MAGIC);
        write_u32be(&mut block, 4, JournalBlockType::DESCRIPTOR.0);
        write_u32be(&mut block, 8, self.sequence);
        let mut offset = JournalBlockHeader::SIZE;
        for (index, image) in self.metadata.iter().enumerate() {
            let low = u32::try_from(image.block_index & 0xffff_ffff)
                .expect("masked block index fits u32");
            let high =
                u32::try_from(image.block_index >> 32).expect("shifted block index fits u32");
            let flags = if index == 0 { 0 } else { UUID_OMITTED }
                | if index + 1 == self.metadata.len() {
                    LAST_TAG
                } else {
                    0
                };
            let mut checksum = Checksum::new();
            checksum.update(self.uuid.as_bytes());
            checksum.update_u32_be(self.sequence);
            checksum.update(&image.bytes);
            write_u32be(&mut block, offset, low);
            write_u32be(&mut block, offset + 4, flags);
            write_u32be(&mut block, offset + 8, high);
            write_u32be(&mut block, offset + 12, checksum.finalize());
            offset += TAG_BYTES;
            if index == 0 {
                block[offset..offset + 16].copy_from_slice(self.uuid.as_bytes());
                offset += 16;
            }
        }
        let checksum_offset = JOURNAL_BLOCK_BYTES - 4;
        let mut checksum = Checksum::new();
        checksum.update(self.uuid.as_bytes());
        checksum.update(&block[..checksum_offset]);
        checksum.update_u32_be(0);
        write_u32be(&mut block, checksum_offset, checksum.finalize());
        block
    }

    fn commit_block(&self) -> Vec<u8> {
        const CHECKSUM_OFFSET: usize = 16;

        let mut block = vec![0; JOURNAL_BLOCK_BYTES];
        write_u32be(&mut block, 0, JournalBlockHeader::MAGIC);
        write_u32be(&mut block, 4, JournalBlockType::COMMIT.0);
        write_u32be(&mut block, 8, self.sequence);
        let mut checksum = Checksum::new();
        checksum.update(self.uuid.as_bytes());
        checksum.update(&block[..CHECKSUM_OFFSET]);
        checksum.update_u32_be(0);
        checksum.update(&block[CHECKSUM_OFFSET + 4..]);
        write_u32be(&mut block, CHECKSUM_OFFSET, checksum.finalize());
        block
    }

    fn revocation_block(&self) -> Vec<u8> {
        const TABLE_BYTES_OFFSET: usize = JournalBlockHeader::SIZE;
        const TABLE_OFFSET: usize = TABLE_BYTES_OFFSET + 4;

        let mut block = vec![0; JOURNAL_BLOCK_BYTES];
        write_u32be(&mut block, 0, JournalBlockHeader::MAGIC);
        write_u32be(&mut block, 4, JournalBlockType::REVOCATION.0);
        write_u32be(&mut block, 8, self.sequence);
        let table_bytes = self
            .revoked_blocks
            .len()
            .checked_mul(8)
            .and_then(|bytes| u32::try_from(bytes).ok())
            .expect("bounded revocation table fits u32");
        write_u32be(&mut block, TABLE_BYTES_OFFSET, table_bytes);
        for (index, revoked) in self.revoked_blocks.iter().enumerate() {
            let offset = TABLE_OFFSET + index * 8;
            block[offset..offset + 8].copy_from_slice(&revoked.to_be_bytes());
        }
        let checksum_offset = JOURNAL_BLOCK_BYTES - 4;
        let mut checksum = Checksum::new();
        checksum.update(self.uuid.as_bytes());
        checksum.update(&block[..checksum_offset]);
        checksum.update_u32_be(0);
        write_u32be(&mut block, checksum_offset, checksum.finalize());
        block
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum ReservationState {
    Prepared,
    CommitDurable,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct ActiveReservation {
    ticket: u64,
    start: usize,
    slot_count: usize,
    state: ReservationState,
}

/// One ring reservation and its complete ordered commit operation list.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct JournalPreparedTransaction {
    ticket: u64,
    sequence: u32,
    journal_blocks: Vec<u64>,
    operations: Vec<JournalCommitOperation>,
}

impl JournalPreparedTransaction {
    /// Return the monotonic ticket used for durability transitions.
    #[must_use]
    pub fn ticket(&self) -> u64 {
        self.ticket
    }

    /// Return the JBD2 transaction sequence encoded into every record.
    #[must_use]
    pub fn sequence(&self) -> u32 {
        self.sequence
    }

    /// Return the physical journal blocks reserved in logical ring order.
    #[must_use]
    pub fn journal_blocks(&self) -> &[u64] {
        &self.journal_blocks
    }

    /// Return the only legal ordered-data commit operation list.
    #[must_use]
    pub fn operations(&self) -> &[JournalCommitOperation] {
        &self.operations
    }
}

/// Bounded allocation and reclamation for an admitted clean JBD2 ring.
///
/// The caller supplies the physical blocks of the journal inode after its
/// superblock in logical ring order. This coordinator does not read or update
/// the on-disk journal superblock. It prevents reuse until the oldest durable
/// commit has also completed its checkpoint flush.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct JournalRing {
    uuid: Uuid,
    maximum_block: u64,
    slots: Vec<u64>,
    head: usize,
    tail: usize,
    used: usize,
    next_sequence: u32,
    next_ticket: u64,
    active: VecDeque<ActiveReservation>,
}

impl JournalRing {
    /// Admit a clean journal ring with no live or replayable transactions.
    pub fn new_clean(
        next_sequence: u32,
        journal_uuid: [u8; 16],
        maximum_block: u64,
        journal_slots: &[u64],
    ) -> Result<Self, JournalTransactionError> {
        if next_sequence == u32::MAX {
            return Err(JournalTransactionError::SequenceOverflow);
        }
        if journal_slots.len() < 3
            || journal_slots.len() > JOURNAL_RING_MAX_SLOTS
            || journal_slots.iter().any(|slot| *slot > maximum_block)
        {
            return Err(JournalTransactionError::RingGeometry);
        }
        let mut sorted = Vec::from(journal_slots);
        sorted.sort_unstable();
        if sorted.windows(2).any(|pair| pair[0] == pair[1]) {
            return Err(JournalTransactionError::RingGeometry);
        }
        Ok(Self {
            uuid: Uuid::new(journal_uuid),
            maximum_block,
            slots: Vec::from(journal_slots),
            head: 0,
            tail: 0,
            used: 0,
            next_sequence,
            next_ticket: 1,
            active: VecDeque::new(),
        })
    }

    /// Start a transaction using the ring's next checked sequence.
    pub fn begin_transaction(&self) -> Result<JournalTransaction, JournalTransactionError> {
        JournalTransaction::new(
            self.next_sequence,
            *self.uuid.as_bytes(),
            self.maximum_block,
        )
    }

    /// Reserve ring slots and build a complete commit plan without issuing I/O.
    pub fn prepare(
        &mut self,
        transaction: &JournalTransaction,
    ) -> Result<JournalPreparedTransaction, JournalTransactionError> {
        if transaction.sequence != self.next_sequence
            || transaction.uuid != self.uuid
            || transaction.maximum_block != self.maximum_block
        {
            return Err(JournalTransactionError::RingTransactionMismatch);
        }
        let required = transaction.required_journal_slots()?;
        let free = self
            .slots
            .len()
            .checked_sub(self.used)
            .ok_or(JournalTransactionError::ReservationState)?;
        if required > free {
            return Err(JournalTransactionError::RingFull);
        }
        let next_ticket = self
            .next_ticket
            .checked_add(1)
            .ok_or(JournalTransactionError::ReservationOverflow)?;
        let next_sequence = self
            .next_sequence
            .checked_add(1)
            .ok_or(JournalTransactionError::SequenceOverflow)?;
        let mut journal_blocks = Vec::new();
        journal_blocks
            .try_reserve_exact(required)
            .map_err(|_| JournalTransactionError::TooManyBlocks)?;
        for offset in 0..required {
            let index = self
                .head
                .checked_add(offset)
                .ok_or(JournalTransactionError::RingGeometry)?
                % self.slots.len();
            journal_blocks.push(self.slots[index]);
        }
        let operations = transaction.commit_plan(&journal_blocks)?;
        self.active
            .try_reserve(1)
            .map_err(|_| JournalTransactionError::TooManyBlocks)?;
        let start = self.head;
        self.head = self
            .head
            .checked_add(required)
            .ok_or(JournalTransactionError::RingGeometry)?
            % self.slots.len();
        self.used = self
            .used
            .checked_add(required)
            .ok_or(JournalTransactionError::RingGeometry)?;
        let ticket = self.next_ticket;
        self.next_ticket = next_ticket;
        self.next_sequence = next_sequence;
        self.active.push_back(ActiveReservation {
            ticket,
            start,
            slot_count: required,
            state: ReservationState::Prepared,
        });
        Ok(JournalPreparedTransaction {
            ticket,
            sequence: transaction.sequence,
            journal_blocks,
            operations,
        })
    }

    /// Mark a commit flush durable, preserving transaction sequence order.
    pub fn mark_commit_durable(&mut self, ticket: u64) -> Result<(), JournalTransactionError> {
        let index = self
            .active
            .iter()
            .position(|reservation| reservation.ticket == ticket)
            .ok_or(JournalTransactionError::ReservationUnknown)?;
        if self
            .active
            .iter()
            .take(index)
            .any(|reservation| reservation.state == ReservationState::Prepared)
        {
            return Err(JournalTransactionError::ReservationOrder);
        }
        let reservation = self
            .active
            .get_mut(index)
            .ok_or(JournalTransactionError::ReservationUnknown)?;
        if reservation.state != ReservationState::Prepared {
            return Err(JournalTransactionError::ReservationState);
        }
        reservation.state = ReservationState::CommitDurable;
        Ok(())
    }

    /// Abort only the newest reservation before its commit becomes durable.
    pub fn abort_precommit(&mut self, ticket: u64) -> Result<(), JournalTransactionError> {
        let reservation = self
            .active
            .back()
            .copied()
            .ok_or(JournalTransactionError::ReservationUnknown)?;
        if reservation.ticket != ticket {
            return Err(JournalTransactionError::ReservationOrder);
        }
        if reservation.state != ReservationState::Prepared {
            return Err(JournalTransactionError::ReservationState);
        }
        self.head = reservation.start;
        self.used = self
            .used
            .checked_sub(reservation.slot_count)
            .ok_or(JournalTransactionError::ReservationState)?;
        self.next_sequence = self
            .next_sequence
            .checked_sub(1)
            .ok_or(JournalTransactionError::ReservationState)?;
        let _ = self.active.pop_back();
        Ok(())
    }

    /// Reclaim the oldest committed reservation after its checkpoint flush.
    pub fn checkpoint_durable(&mut self, ticket: u64) -> Result<(), JournalTransactionError> {
        let reservation = self
            .active
            .front()
            .copied()
            .ok_or(JournalTransactionError::ReservationUnknown)?;
        if reservation.ticket != ticket {
            return Err(JournalTransactionError::ReservationOrder);
        }
        if reservation.state != ReservationState::CommitDurable {
            return Err(JournalTransactionError::ReservationState);
        }
        self.tail = self
            .tail
            .checked_add(reservation.slot_count)
            .ok_or(JournalTransactionError::RingGeometry)?
            % self.slots.len();
        self.used = self
            .used
            .checked_sub(reservation.slot_count)
            .ok_or(JournalTransactionError::ReservationState)?;
        let _ = self.active.pop_front();
        Ok(())
    }

    /// Return the number of journal data slots currently reserved.
    #[must_use]
    pub fn used_slots(&self) -> usize {
        self.used
    }

    /// Return the next transaction sequence that will be assigned.
    #[must_use]
    pub fn next_sequence(&self) -> u32 {
        self.next_sequence
    }
}

/// Validate and replay one complete descriptor/data/commit transaction.
///
/// No home-block images are returned unless the final commit header, sequence,
/// and checksum are valid. Callers can therefore discard every power-cut
/// prefix before the durable commit without exposing partial metadata.
/// `maximum_block` is the highest valid replay target, inclusive.
pub fn replay_committed_transaction(
    journal_uuid: [u8; 16],
    sequence: u32,
    maximum_block: u64,
    journal_blocks: &[&[u8]],
) -> Result<Vec<JournalBlockImage>, JournalTransactionError> {
    if journal_blocks.len() < 2 {
        return Err(JournalTransactionError::MissingCommit);
    }
    if journal_blocks
        .iter()
        .any(|block| block.len() != JOURNAL_BLOCK_BYTES)
    {
        return Err(JournalTransactionError::IncorrectBlockLength);
    }
    let superblock = JournalSuperblock {
        block_size: u32::try_from(JOURNAL_BLOCK_BYTES).expect("4096 fits u32"),
        sequence,
        start_block: 0,
        uuid: Uuid::new(journal_uuid),
    };
    let descriptor = journal_blocks[0];
    let descriptor_header = JournalBlockHeader::read_bytes(descriptor)
        .ok_or(JournalTransactionError::CorruptDescriptor)?;
    if descriptor_header.block_type != JournalBlockType::DESCRIPTOR
        || descriptor_header.sequence != sequence
        || validate_descriptor_block_checksum(&superblock, descriptor).is_err()
    {
        return Err(JournalTransactionError::CorruptDescriptor);
    }
    let mut tags = Vec::new();
    for tag in DescriptorBlockTagIter::new(&descriptor[JournalBlockHeader::SIZE..]) {
        tags.push(tag.map_err(|_| JournalTransactionError::CorruptDescriptor)?);
    }
    let without_revocation = tags
        .len()
        .checked_add(2)
        .ok_or(JournalTransactionError::TooManyBlocks)?;
    let with_revocation = without_revocation
        .checked_add(1)
        .ok_or(JournalTransactionError::TooManyBlocks)?;
    if tags.is_empty()
        || (journal_blocks.len() != without_revocation && journal_blocks.len() != with_revocation)
    {
        return Err(JournalTransactionError::JournalSlotCount);
    }
    if tags.len() > JOURNAL_TRANSACTION_MAX_METADATA_BLOCKS {
        return Err(JournalTransactionError::TooManyBlocks);
    }

    let mut replay = Vec::new();
    replay
        .try_reserve_exact(tags.len())
        .map_err(|_| JournalTransactionError::TooManyBlocks)?;
    for (index, tag) in tags.iter().enumerate() {
        if tag.block_index > maximum_block {
            return Err(JournalTransactionError::BlockOutOfRange);
        }
        if replay
            .iter()
            .any(|image: &JournalBlockImage| image.block_index == tag.block_index)
        {
            return Err(JournalTransactionError::DuplicateBlock);
        }
        let data = journal_blocks[index + 1];
        let mut checksum = Checksum::new();
        checksum.update(superblock.uuid.as_bytes());
        checksum.update_u32_be(sequence);
        checksum.update(data);
        if checksum.finalize() != tag.checksum {
            return Err(JournalTransactionError::CorruptData);
        }
        replay.push(JournalBlockImage {
            block_index: tag.block_index,
            bytes: Vec::from(data),
        });
    }

    if journal_blocks.len() == with_revocation {
        let revocation = journal_blocks[tags.len() + 1];
        let header = JournalBlockHeader::read_bytes(revocation)
            .ok_or(JournalTransactionError::CorruptRevocation)?;
        if header.block_type != JournalBlockType::REVOCATION
            || header.sequence != sequence
            || validate_revocation_block_checksum(&superblock, revocation).is_err()
        {
            return Err(JournalTransactionError::CorruptRevocation);
        }
        let mut revoked_blocks = Vec::new();
        read_revocation_block_table(revocation, &mut revoked_blocks)
            .map_err(|_| JournalTransactionError::CorruptRevocation)?;
        if revoked_blocks.len() > JOURNAL_TRANSACTION_MAX_REVOKED_BLOCKS {
            return Err(JournalTransactionError::TooManyBlocks);
        }
        for (index, revoked) in revoked_blocks.iter().enumerate() {
            if *revoked > maximum_block || revoked_blocks[..index].contains(revoked) {
                return Err(JournalTransactionError::CorruptRevocation);
            }
        }
        replay.retain(|image| !revoked_blocks.contains(&image.block_index));
    }

    let commit = journal_blocks[journal_blocks.len() - 1];
    let commit_header =
        JournalBlockHeader::read_bytes(commit).ok_or(JournalTransactionError::MissingCommit)?;
    if commit_header.block_type != JournalBlockType::COMMIT
        || commit_header.sequence != sequence
        || validate_commit_block_checksum(&superblock, commit).is_err()
    {
        return Err(JournalTransactionError::CorruptCommit);
    }
    Ok(replay)
}

fn write_u32be(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_be_bytes());
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::collections::BTreeMap;

    const UUID: [u8; 16] = [0x5a; 16];
    const MAXIMUM_BLOCK: u64 = 4095;
    const JOURNAL_SLOTS: [u64; 4] = [3000, 3001, 3002, 3003];

    fn filled(value: u8) -> Vec<u8> {
        vec![value; JOURNAL_BLOCK_BYTES]
    }

    fn transaction() -> JournalTransaction {
        let mut transaction = JournalTransaction::new(17, UUID, MAXIMUM_BLOCK).unwrap();
        transaction.stage_ordered_data(100, &filled(0x11)).unwrap();
        transaction.stage_metadata(200, &filled(0x22)).unwrap();
        transaction.stage_metadata(201, &filled(0x33)).unwrap();
        transaction
    }

    fn journal_images(operations: &[JournalCommitOperation]) -> Vec<Vec<u8>> {
        operations
            .iter()
            .filter_map(|operation| match operation {
                JournalCommitOperation::WriteJournal { bytes, .. } => Some(bytes.clone()),
                _ => None,
            })
            .collect()
    }

    #[test]
    fn transaction_round_trips_through_reader_checks() {
        let operations = transaction().commit_plan(&JOURNAL_SLOTS).unwrap();
        let journal = journal_images(&operations);
        let references: Vec<&[u8]> = journal.iter().map(Vec::as_slice).collect();
        let replay = replay_committed_transaction(UUID, 17, MAXIMUM_BLOCK, &references).unwrap();

        assert_eq!(replay.len(), 2);
        assert_eq!(replay[0].block_index(), 200);
        assert_eq!(replay[0].bytes(), filled(0x22));
        assert_eq!(replay[1].block_index(), 201);
        assert_eq!(replay[1].bytes(), filled(0x33));
    }

    #[test]
    fn descriptor_has_linux_compatible_first_uuid_layout() {
        const SAME_UUID: u32 = 0x2;
        const LAST_TAG: u32 = 0x8;

        let operations = transaction().commit_plan(&JOURNAL_SLOTS).unwrap();
        let descriptor = journal_images(&operations).remove(0);
        let first_tag = JournalBlockHeader::SIZE;
        assert_eq!(
            u32::from_be_bytes(descriptor[first_tag + 4..first_tag + 8].try_into().unwrap()),
            0
        );
        assert_eq!(&descriptor[first_tag + 16..first_tag + 32], &UUID);

        let second_tag = first_tag + 32;
        assert_eq!(
            u32::from_be_bytes(
                descriptor[second_tag + 4..second_tag + 8]
                    .try_into()
                    .unwrap()
            ),
            SAME_UUID | LAST_TAG
        );
    }

    #[test]
    fn commit_flush_precedes_every_home_metadata_write() {
        let operations = transaction().commit_plan(&JOURNAL_SLOTS).unwrap();
        let ordered_data_flush = operations
            .iter()
            .position(|operation| {
                *operation == JournalCommitOperation::Flush(JournalFlush::OrderedData)
            })
            .unwrap();
        let commit_record = operations
            .iter()
            .position(|operation| {
                matches!(
                    operation,
                    JournalCommitOperation::WriteJournal {
                        kind: JournalRecordKind::Commit,
                        ..
                    }
                )
            })
            .unwrap();
        let commit_flush = operations
            .iter()
            .position(|operation| *operation == JournalCommitOperation::Flush(JournalFlush::Commit))
            .unwrap();
        let first_home = operations
            .iter()
            .position(|operation| matches!(operation, JournalCommitOperation::WriteHomeMetadata(_)))
            .unwrap();

        assert!(ordered_data_flush < commit_record);
        assert!(commit_flush < first_home);
        assert!(operations[..commit_flush].iter().all(|operation| {
            !matches!(operation, JournalCommitOperation::WriteHomeMetadata(_))
        }));
    }

    #[test]
    fn every_power_cut_before_commit_flush_discards_metadata() {
        let operations = transaction().commit_plan(&JOURNAL_SLOTS).unwrap();
        let commit_flush = operations
            .iter()
            .position(|operation| *operation == JournalCommitOperation::Flush(JournalFlush::Commit))
            .unwrap();

        for cut in 0..=operations.len() {
            let mut pending_journal = BTreeMap::new();
            let mut durable_journal = BTreeMap::new();
            let mut pending_ordered_data = BTreeMap::new();
            let mut durable_ordered_data = BTreeMap::new();
            let mut pending_home = BTreeMap::new();
            let mut durable_home = BTreeMap::new();
            for operation in &operations[..cut] {
                match operation {
                    JournalCommitOperation::WriteJournal {
                        journal_block,
                        bytes,
                        ..
                    } => {
                        pending_journal.insert(*journal_block, bytes.clone());
                    }
                    JournalCommitOperation::WriteHomeMetadata(image) => {
                        pending_home.insert(image.block_index, image.bytes.clone());
                    }
                    JournalCommitOperation::WriteOrderedData(image) => {
                        pending_ordered_data.insert(image.block_index, image.bytes.clone());
                    }
                    JournalCommitOperation::Flush(_) => {
                        durable_journal.extend(pending_journal.clone());
                        durable_ordered_data.extend(pending_ordered_data.clone());
                        durable_home.extend(pending_home.clone());
                    }
                }
            }

            if cut <= commit_flush {
                assert!(durable_home.is_empty());
                let durable: Option<Vec<&[u8]>> = JOURNAL_SLOTS
                    .iter()
                    .map(|slot| durable_journal.get(slot).map(Vec::as_slice))
                    .collect();
                assert!(durable.is_none());
            } else {
                assert_eq!(durable_ordered_data.get(&100), Some(&filled(0x11)));
                let durable: Vec<&[u8]> = JOURNAL_SLOTS
                    .iter()
                    .map(|slot| durable_journal.get(slot).unwrap().as_slice())
                    .collect();
                let replay =
                    replay_committed_transaction(UUID, 17, MAXIMUM_BLOCK, &durable).unwrap();
                assert_eq!(replay.len(), 2);
            }
        }
    }

    #[test]
    fn corruption_and_truncation_are_not_replayable() {
        let operations = transaction().commit_plan(&JOURNAL_SLOTS).unwrap();
        let journal = journal_images(&operations);

        for (index, expected) in [
            (0usize, JournalTransactionError::CorruptDescriptor),
            (1usize, JournalTransactionError::CorruptData),
            (3usize, JournalTransactionError::CorruptCommit),
        ] {
            let mut corrupt = journal.clone();
            corrupt[index][128] ^= 0x80;
            let references: Vec<&[u8]> = corrupt.iter().map(Vec::as_slice).collect();
            assert_eq!(
                replay_committed_transaction(UUID, 17, MAXIMUM_BLOCK, &references),
                Err(expected)
            );
        }
        let references: Vec<&[u8]> = journal[..3].iter().map(Vec::as_slice).collect();
        assert!(replay_committed_transaction(UUID, 17, MAXIMUM_BLOCK, &references).is_err());

        let references: Vec<&[u8]> = journal.iter().map(Vec::as_slice).collect();
        assert_eq!(
            replay_committed_transaction(UUID, 17, 199, &references),
            Err(JournalTransactionError::BlockOutOfRange)
        );
    }

    #[test]
    fn revocations_are_checksummed_and_remove_stale_images() {
        let mut transaction = transaction();
        transaction.stage_revocation(200).unwrap();
        let slots = [3000, 3001, 3002, 3003, 3004];
        let operations = transaction.commit_plan(&slots).unwrap();
        assert!(operations.iter().any(|operation| {
            matches!(
                operation,
                JournalCommitOperation::WriteJournal {
                    kind: JournalRecordKind::Revocation,
                    ..
                }
            )
        }));
        let journal = journal_images(&operations);
        let references: Vec<&[u8]> = journal.iter().map(Vec::as_slice).collect();
        let replay = replay_committed_transaction(UUID, 17, MAXIMUM_BLOCK, &references).unwrap();
        assert_eq!(replay.len(), 1);
        assert_eq!(replay[0].block_index(), 201);

        let mut corrupt = journal;
        corrupt[3][128] ^= 0x80;
        let references: Vec<&[u8]> = corrupt.iter().map(Vec::as_slice).collect();
        assert_eq!(
            replay_committed_transaction(UUID, 17, MAXIMUM_BLOCK, &references),
            Err(JournalTransactionError::CorruptRevocation)
        );
    }

    #[test]
    fn clean_ring_wraps_and_reclaims_only_after_checkpoint() {
        let slots = [3000, 3001, 3002, 3003, 3004, 3005, 3006, 3007];
        let mut ring = JournalRing::new_clean(40, UUID, MAXIMUM_BLOCK, &slots).unwrap();

        let mut first = ring.begin_transaction().unwrap();
        first.stage_metadata(100, &filled(1)).unwrap();
        let first = ring.prepare(&first).unwrap();
        assert_eq!(first.journal_blocks(), &slots[..3]);
        ring.mark_commit_durable(first.ticket()).unwrap();
        ring.checkpoint_durable(first.ticket()).unwrap();

        let mut second = ring.begin_transaction().unwrap();
        second.stage_metadata(101, &filled(2)).unwrap();
        second.stage_metadata(102, &filled(3)).unwrap();
        let second = ring.prepare(&second).unwrap();
        assert_eq!(second.journal_blocks(), &slots[3..7]);
        ring.mark_commit_durable(second.ticket()).unwrap();
        ring.checkpoint_durable(second.ticket()).unwrap();

        let mut wrapped = ring.begin_transaction().unwrap();
        wrapped.stage_metadata(103, &filled(4)).unwrap();
        let wrapped = ring.prepare(&wrapped).unwrap();
        assert_eq!(wrapped.journal_blocks(), &[3007, 3000, 3001]);
        assert_eq!(wrapped.sequence(), 42);
        assert_eq!(ring.next_sequence(), 43);
        assert_eq!(ring.used_slots(), 3);
    }

    #[test]
    fn ring_refuses_reuse_and_out_of_order_transitions() {
        let slots = [3000, 3001, 3002, 3003, 3004, 3005];
        let mut ring = JournalRing::new_clean(50, UUID, MAXIMUM_BLOCK, &slots).unwrap();
        let mut first = ring.begin_transaction().unwrap();
        first.stage_metadata(100, &filled(1)).unwrap();
        let first = ring.prepare(&first).unwrap();
        let mut second = ring.begin_transaction().unwrap();
        second.stage_metadata(101, &filled(2)).unwrap();
        let second = ring.prepare(&second).unwrap();
        let mut full = ring.begin_transaction().unwrap();
        full.stage_metadata(102, &filled(3)).unwrap();
        assert_eq!(ring.prepare(&full), Err(JournalTransactionError::RingFull));
        assert_eq!(
            ring.mark_commit_durable(second.ticket()),
            Err(JournalTransactionError::ReservationOrder)
        );
        assert_eq!(
            ring.checkpoint_durable(first.ticket()),
            Err(JournalTransactionError::ReservationState)
        );
        assert_eq!(
            ring.abort_precommit(first.ticket()),
            Err(JournalTransactionError::ReservationOrder)
        );
        ring.mark_commit_durable(first.ticket()).unwrap();
        ring.mark_commit_durable(second.ticket()).unwrap();
        assert_eq!(
            ring.checkpoint_durable(second.ticket()),
            Err(JournalTransactionError::ReservationOrder)
        );
        ring.checkpoint_durable(first.ticket()).unwrap();
        ring.checkpoint_durable(second.ticket()).unwrap();
        assert_eq!(ring.used_slots(), 0);

        let mut aborted = ring.begin_transaction().unwrap();
        aborted.stage_metadata(103, &filled(4)).unwrap();
        let aborted = ring.prepare(&aborted).unwrap();
        let aborted_sequence = aborted.sequence();
        ring.abort_precommit(aborted.ticket()).unwrap();
        assert_eq!(ring.used_slots(), 0);
        assert_eq!(ring.next_sequence(), aborted_sequence);
        assert_eq!(
            ring.mark_commit_durable(aborted.ticket()),
            Err(JournalTransactionError::ReservationUnknown)
        );
    }

    #[test]
    fn hostile_bounds_duplicates_and_escape_are_refused() {
        assert_eq!(
            JournalTransaction::new(u32::MAX, UUID, MAXIMUM_BLOCK).unwrap_err(),
            JournalTransactionError::SequenceOverflow
        );
        assert_eq!(
            JournalTransaction::new(1, UUID, MAXIMUM_BLOCK)
                .unwrap()
                .commit_plan(&[100, 101]),
            Err(JournalTransactionError::EmptyTransaction)
        );

        let mut transaction = JournalTransaction::new(9, UUID, MAXIMUM_BLOCK).unwrap();
        assert_eq!(
            transaction.stage_metadata(1, &[0; 8]),
            Err(JournalTransactionError::IncorrectBlockLength)
        );
        assert_eq!(
            transaction.stage_metadata(MAXIMUM_BLOCK + 1, &filled(1)),
            Err(JournalTransactionError::BlockOutOfRange)
        );
        transaction.stage_metadata(2, &filled(2)).unwrap();
        assert_eq!(
            transaction.stage_ordered_data(2, &filled(3)),
            Err(JournalTransactionError::DuplicateBlock)
        );
        let mut escaped = filled(0);
        escaped[..4].copy_from_slice(&JournalBlockHeader::MAGIC.to_be_bytes());
        assert_eq!(
            transaction.stage_metadata(3, &escaped),
            Err(JournalTransactionError::EscapedBlockUnsupported)
        );
        assert_eq!(
            transaction.commit_plan(&[100, 101]),
            Err(JournalTransactionError::JournalSlotCount)
        );
        assert_eq!(
            transaction.commit_plan(&[100, 100, 102]),
            Err(JournalTransactionError::DuplicateBlock)
        );
        assert_eq!(
            transaction.commit_plan(&[2, 101, 102]),
            Err(JournalTransactionError::JournalSlotOverlap)
        );
        assert_eq!(
            transaction.commit_plan(&[100, 101, MAXIMUM_BLOCK + 1]),
            Err(JournalTransactionError::BlockOutOfRange)
        );
        transaction.stage_revocation(4).unwrap();
        assert_eq!(
            transaction.stage_revocation(4),
            Err(JournalTransactionError::DuplicateBlock)
        );
        assert_eq!(
            transaction.stage_revocation(MAXIMUM_BLOCK + 1),
            Err(JournalTransactionError::BlockOutOfRange)
        );

        assert_eq!(
            JournalRing::new_clean(1, UUID, MAXIMUM_BLOCK, &[1, 2]),
            Err(JournalTransactionError::RingGeometry)
        );
        assert_eq!(
            JournalRing::new_clean(1, UUID, MAXIMUM_BLOCK, &[1, 2, 2]),
            Err(JournalTransactionError::RingGeometry)
        );
        assert_eq!(
            JournalRing::new_clean(u32::MAX, UUID, MAXIMUM_BLOCK, &[3000, 3001, 3002]),
            Err(JournalTransactionError::SequenceOverflow)
        );

        let mut bounded = JournalTransaction::new(10, UUID, MAXIMUM_BLOCK).unwrap();
        for block in 0..JOURNAL_TRANSACTION_MAX_METADATA_BLOCKS {
            bounded
                .stage_metadata(u64::try_from(block).unwrap(), &filled(4))
                .unwrap();
        }
        assert_eq!(
            bounded.stage_metadata(
                u64::try_from(JOURNAL_TRANSACTION_MAX_METADATA_BLOCKS).unwrap(),
                &filled(5)
            ),
            Err(JournalTransactionError::TooManyBlocks)
        );
    }
}
