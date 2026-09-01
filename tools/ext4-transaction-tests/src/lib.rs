// SPDX-License-Identifier: GPL-3.0-only
#![cfg(test)]

use ext4plus::{
    Ext4, Ext4Read, Ext4Write, FILESYSTEM_SUPERBLOCK_START_BYTE, JOURNAL_BLOCK_BYTES,
    JournalCommitOperation, JournalExecutionError, JournalFlush, JournalMutationStage,
    JournalMutationStageError, JournalPreparedTransaction, JournalRecordKind, JournalRing,
    JournalStorage, JournalSuperblockImage, JournalTransaction, JournalTransactionError,
    execute_commit_operations, load_journal_inode_map, recover_committed_ring,
    replay_committed_transaction,
};
use std::collections::BTreeMap;
use std::rc::Rc;

const UUID: [u8; 16] = [0x5a; 16];
const MAXIMUM_BLOCK: u64 = 4095;
const JOURNAL_SLOTS: [u64; 4] = [3000, 3001, 3002, 3003];

fn filled(value: u8) -> Vec<u8> {
    vec![value; JOURNAL_BLOCK_BYTES]
}

fn ext4_crc32c(bytes: &[u8]) -> u32 {
    const REFLECTED_CRC32C_POLYNOMIAL: u32 = 0x82f6_3b78;

    let mut checksum = u32::MAX;
    for byte in bytes {
        checksum ^= u32::from(*byte);
        for _ in 0..8 {
            let mask = 0u32.wrapping_sub(checksum & 1);
            checksum = (checksum >> 1) ^ (REFLECTED_CRC32C_POLYNOMIAL & mask);
        }
    }
    checksum
}

fn transaction() -> JournalTransaction {
    let mut transaction = JournalTransaction::new(17, UUID, MAXIMUM_BLOCK).unwrap();
    transaction.stage_ordered_data(100, &filled(0x11)).unwrap();
    transaction.stage_metadata(200, &filled(0x22)).unwrap();
    transaction.stage_metadata(201, &filled(0x33)).unwrap();
    transaction
}

fn mapped_ring(next_sequence: u32, slots: &[u64]) -> JournalRing {
    let superblock =
        JournalSuperblockImage::new_clean(next_sequence, UUID, (slots.len() + 1) as u32).unwrap();
    let mut physical = Vec::with_capacity(slots.len() + 1);
    physical.push(MAXIMUM_BLOCK);
    physical.extend_from_slice(slots);
    superblock.map_clean_ring(MAXIMUM_BLOCK, &physical).unwrap()
}

fn finish_transaction(ring: &mut JournalRing, prepared: &JournalPreparedTransaction) {
    let _commit = ring.prepare_commit_plan(prepared).unwrap();
    ring.mark_commit_durable(prepared.ticket()).unwrap();
    let _checkpoint = ring.prepare_checkpoint_plan(prepared.ticket()).unwrap();
    ring.checkpoint_durable(prepared.ticket()).unwrap();
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

#[derive(Debug, Eq, PartialEq)]
enum StorageEvent {
    Write(u64, usize),
    Flush(JournalFlush),
}

#[derive(Default)]
struct RecordingStorage {
    events: Vec<StorageEvent>,
}

impl JournalStorage for RecordingStorage {
    type Error = core::convert::Infallible;

    fn write(&mut self, start_byte: u64, bytes: &[u8]) -> Result<(), Self::Error> {
        self.events.push(StorageEvent::Write(start_byte, bytes.len()));
        Ok(())
    }

    fn flush(&mut self, boundary: JournalFlush) -> Result<(), Self::Error> {
        self.events.push(StorageEvent::Flush(boundary));
        Ok(())
    }
}

#[test]
fn public_executor_maps_every_block_write_and_preserves_flushes() {
    let slots = [3000, 3001, 3002, 3003, 3004, 3005, 3006, 3007];
    let mut ring = mapped_ring(17, &slots);
    let prepared = ring.prepare(&transaction()).unwrap();
    let operations = ring.prepare_commit_plan(&prepared).unwrap();
    let mut storage = RecordingStorage::default();
    execute_commit_operations(&mut storage, &operations).unwrap();

    assert_eq!(
        storage.events,
        vec![
            StorageEvent::Write(MAXIMUM_BLOCK * JOURNAL_BLOCK_BYTES as u64, 1024),
            StorageEvent::Write(100 * JOURNAL_BLOCK_BYTES as u64, JOURNAL_BLOCK_BYTES),
            StorageEvent::Flush(JournalFlush::OrderedData),
            StorageEvent::Write(3000 * JOURNAL_BLOCK_BYTES as u64, JOURNAL_BLOCK_BYTES),
            StorageEvent::Write(3001 * JOURNAL_BLOCK_BYTES as u64, JOURNAL_BLOCK_BYTES),
            StorageEvent::Write(3002 * JOURNAL_BLOCK_BYTES as u64, JOURNAL_BLOCK_BYTES),
            StorageEvent::Flush(JournalFlush::JournalPayload),
            StorageEvent::Write(3003 * JOURNAL_BLOCK_BYTES as u64, JOURNAL_BLOCK_BYTES),
            StorageEvent::Flush(JournalFlush::Commit),
            StorageEvent::Write(200 * JOURNAL_BLOCK_BYTES as u64, JOURNAL_BLOCK_BYTES),
            StorageEvent::Write(201 * JOURNAL_BLOCK_BYTES as u64, JOURNAL_BLOCK_BYTES),
            StorageEvent::Flush(JournalFlush::Checkpoint),
        ]
    );

    let overflow = [JournalCommitOperation::WriteJournal {
        journal_block: u64::MAX,
        kind: JournalRecordKind::Commit,
        bytes: filled(0),
    }];
    let mut storage = RecordingStorage::default();
    assert_eq!(
        execute_commit_operations(&mut storage, &overflow),
        Err(JournalExecutionError::AddressOverflow)
    );
    assert!(storage.events.is_empty());
}

#[test]
fn mutation_stage_coalesces_partial_blocks_without_writing_through() {
    let backing = Rc::new(
        (0..JOURNAL_BLOCK_BYTES * 66)
            .map(|index| (index % 251) as u8)
            .collect::<Vec<_>>(),
    );
    let stage = JournalMutationStage::new(
        Box::new(backing.clone()),
        u64::try_from(backing.len()).unwrap(),
    )
    .unwrap();
    assert_eq!(
        Ext4Write::write(&stage, 1023, &[0xff]).unwrap_err().to_string(),
        JournalMutationStageError::Range.to_string()
    );
    let original = backing[8186..8202].to_vec();
    Ext4Write::write(&stage, 8190, &[0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6]).unwrap();
    assert_eq!(stage.staged_block_count(), 2);
    let images = stage.staged_images();
    assert_eq!(images.len(), 2);
    assert_eq!(images[0].block_index(), 1);
    assert_eq!(images[1].block_index(), 2);
    assert_eq!(&images[0].bytes()[4094..], &[0xa1, 0xa2]);
    assert_eq!(&images[1].bytes()[..4], &[0xa3, 0xa4, 0xa5, 0xa6]);

    let mut overlaid = [0u8; 16];
    Ext4Read::read(&stage, 8186, &mut overlaid).unwrap();
    assert_eq!(&overlaid[..4], &original[..4]);
    assert_eq!(&overlaid[4..10], &[0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6]);
    assert_eq!(&overlaid[10..], &original[10..]);
    assert_eq!(&backing[8186..8202], original.as_slice());

    stage.rollback();
    assert_eq!(stage.staged_block_count(), 0);
    Ext4Read::read(&stage, 8186, &mut overlaid).unwrap();
    assert_eq!(overlaid.as_slice(), original.as_slice());

    for block in 1..=64u64 {
        Ext4Write::write(&stage, block * JOURNAL_BLOCK_BYTES as u64, &[0xff]).unwrap();
    }
    let error = Ext4Write::write(&stage, 65 * JOURNAL_BLOCK_BYTES as u64, &[0xff])
        .unwrap_err();
    assert_eq!(error.to_string(), JournalMutationStageError::TooManyBlocks.to_string());
    assert_eq!(stage.staged_block_count(), 64);
    assert_eq!(backing[65 * JOURNAL_BLOCK_BYTES], (65 * JOURNAL_BLOCK_BYTES % 251) as u8);
}

fn install_journal_writes(
    logical_slots: &[u64],
    storage: &mut [Vec<u8>],
    prepared: &JournalPreparedTransaction,
) {
    for operation in prepared.operations() {
        if let JournalCommitOperation::WriteJournal {
            journal_block,
            bytes,
            ..
        } = operation
        {
            let index = logical_slots
                .iter()
                .position(|slot| slot == journal_block)
                .unwrap();
            storage[index] = bytes.clone();
        }
    }
}

#[test]
fn public_transaction_round_trips_and_preserves_order() {
    let operations = transaction().commit_plan(&JOURNAL_SLOTS).unwrap();
    let ordered_flush = operations
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

    assert!(ordered_flush < commit_record);
    assert!(commit_record < commit_flush);
    assert!(commit_flush < first_home);
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
fn every_precommit_power_cut_has_no_durable_home_or_replay() {
    let operations = transaction().commit_plan(&JOURNAL_SLOTS).unwrap();
    let commit_flush = operations
        .iter()
        .position(|operation| *operation == JournalCommitOperation::Flush(JournalFlush::Commit))
        .unwrap();

    for cut in 0..=commit_flush {
        let mut pending_journal = BTreeMap::new();
        let mut durable_journal = BTreeMap::new();
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
                    pending_home.insert(image.block_index(), image.bytes().to_vec());
                }
                JournalCommitOperation::Flush(_) => {
                    durable_journal.extend(pending_journal.clone());
                    durable_home.extend(pending_home.clone());
                }
                JournalCommitOperation::WriteOrderedData(_) => {}
                JournalCommitOperation::WriteFilesystemSuperblock { .. } => {}
                JournalCommitOperation::WriteJournalSuperblock { .. } => {}
            }
        }
        assert!(durable_home.is_empty());
        let complete: Option<Vec<&[u8]>> = JOURNAL_SLOTS
            .iter()
            .map(|slot| durable_journal.get(slot).map(Vec::as_slice))
            .collect();
        assert!(complete.is_none());
    }
}

#[test]
fn mapped_ring_power_cuts_preserve_a_recoverable_or_clean_state() {
    let slots = [3000, 3001, 3002, 3003, 3004, 3005, 3006, 3007];
    let initial_superblock =
        JournalSuperblockImage::new_clean(60, UUID, (slots.len() + 1) as u32).unwrap();
    let mut physical = Vec::from([MAXIMUM_BLOCK]);
    physical.extend_from_slice(&slots);
    let mut ring = initial_superblock
        .map_clean_ring(MAXIMUM_BLOCK, &physical)
        .unwrap();
    let mut transaction = ring.begin_transaction().unwrap();
    transaction.stage_metadata(200, &filled(0x77)).unwrap();
    let prepared = ring.prepare(&transaction).unwrap();
    let mut operations = ring.prepare_commit_plan(&prepared).unwrap();

    let live = match &operations[0] {
        JournalCommitOperation::WriteJournalSuperblock {
            journal_block,
            image,
        } => {
            assert_eq!(*journal_block, MAXIMUM_BLOCK);
            image.clone()
        }
        _ => panic!("mapped commit must begin with a live superblock"),
    };
    assert_eq!(live.sequence(), 60);
    assert_eq!(live.start_block(), 1);
    ring.mark_commit_durable(prepared.ticket()).unwrap();
    let checkpoint = ring.prepare_checkpoint_plan(prepared.ticket()).unwrap();
    let clean = match &checkpoint[0] {
        JournalCommitOperation::WriteJournalSuperblock { image, .. } => image.clone(),
        _ => panic!("checkpoint must finish with a journal-state update"),
    };
    assert_eq!(clean.sequence(), 61);
    assert_eq!(clean.start_block(), 0);
    operations.extend(checkpoint);

    for cut in 0..=operations.len() {
        let mut durable_superblock = initial_superblock.clone();
        let mut pending_superblock = None;
        let mut durable_journal = vec![filled(0); slots.len()];
        let mut pending_journal = BTreeMap::new();
        let mut durable_home = BTreeMap::from([(200, filled(0))]);
        let mut pending_home = BTreeMap::new();

        for operation in &operations[..cut] {
            match operation {
                JournalCommitOperation::WriteJournalSuperblock { image, .. } => {
                    pending_superblock = Some(image.clone());
                }
                JournalCommitOperation::WriteJournal {
                    journal_block,
                    bytes,
                    ..
                } => {
                    pending_journal.insert(*journal_block, bytes.clone());
                }
                JournalCommitOperation::WriteHomeMetadata(image) => {
                    pending_home.insert(image.block_index(), image.bytes().to_vec());
                }
                JournalCommitOperation::Flush(_) => {
                    if let Some(superblock) = pending_superblock.take() {
                        durable_superblock = superblock;
                    }
                    for (block, bytes) in &pending_journal {
                        let index = slots.iter().position(|slot| slot == block).unwrap();
                        durable_journal[index] = bytes.clone();
                    }
                    durable_home.extend(pending_home.clone());
                }
                JournalCommitOperation::WriteOrderedData(_) => {}
                JournalCommitOperation::WriteFilesystemSuperblock { .. } => {}
            }
        }

        if durable_superblock.start_block() == 0 {
            if durable_superblock.sequence() == 60 {
                assert_eq!(durable_home.get(&200), Some(&filled(0)));
            } else {
                assert_eq!(durable_superblock.sequence(), 61);
                assert_eq!(durable_home.get(&200), Some(&filled(0x77)));
            }
            continue;
        }

        let references: Vec<&[u8]> = durable_journal.iter().map(Vec::as_slice).collect();
        let recovery =
            recover_committed_ring(&durable_superblock, true, MAXIMUM_BLOCK, &references).unwrap();
        if recovery.committed_transactions() == 0 {
            assert_eq!(durable_home.get(&200), Some(&filled(0)));
        } else {
            assert_eq!(recovery.committed_transactions(), 1);
            assert_eq!(recovery.replay_images().len(), 1);
            assert_eq!(recovery.replay_images()[0].block_index(), 200);
            assert_eq!(recovery.replay_images()[0].bytes(), filled(0x77));
        }
    }

    ring.checkpoint_durable(prepared.ticket()).unwrap();
    assert_eq!(ring.used_slots(), 0);
    assert_eq!(ring.next_sequence(), 61);
}

#[test]
fn corrupted_descriptor_data_and_commit_are_refused() {
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
}

#[test]
fn revocation_records_are_checksummed_and_suppress_stale_images() {
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
fn clean_ring_wraps_and_refuses_early_reclamation() {
    let slots = [3000, 3001, 3002, 3003, 3004, 3005, 3006, 3007];
    let mut ring = mapped_ring(40, &slots);
    let mut first = ring.begin_transaction().unwrap();
    first.stage_metadata(100, &filled(1)).unwrap();
    let first = ring.prepare(&first).unwrap();
    assert_eq!(first.journal_blocks(), &slots[..3]);
    assert_eq!(
        ring.checkpoint_durable(first.ticket()),
        Err(JournalTransactionError::ReservationState)
    );
    let _commit = ring.prepare_commit_plan(&first).unwrap();
    ring.mark_commit_durable(first.ticket()).unwrap();
    assert_eq!(
        ring.checkpoint_durable(first.ticket()),
        Err(JournalTransactionError::ReservationState)
    );
    let _checkpoint = ring.prepare_checkpoint_plan(first.ticket()).unwrap();
    ring.checkpoint_durable(first.ticket()).unwrap();

    let mut second = ring.begin_transaction().unwrap();
    second.stage_metadata(101, &filled(2)).unwrap();
    second.stage_metadata(102, &filled(3)).unwrap();
    let second = ring.prepare(&second).unwrap();
    finish_transaction(&mut ring, &second);

    let mut wrapped = ring.begin_transaction().unwrap();
    wrapped.stage_metadata(103, &filled(4)).unwrap();
    let wrapped = ring.prepare(&wrapped).unwrap();
    assert_eq!(wrapped.journal_blocks(), &[3007, 3000, 3001]);
    assert_eq!(ring.used_slots(), 3);
    let aborted_sequence = wrapped.sequence();
    ring.abort_precommit(wrapped.ticket()).unwrap();
    assert_eq!(ring.used_slots(), 0);
    assert_eq!(ring.next_sequence(), aborted_sequence);
}

#[test]
fn checkpoint_tail_advances_only_to_the_next_durable_transaction() {
    let slots = [3000, 3001, 3002, 3003, 3004, 3005, 3006, 3007];
    let mut ring = mapped_ring(70, &slots);
    let mut first = ring.begin_transaction().unwrap();
    first.stage_metadata(100, &filled(1)).unwrap();
    let first = ring.prepare(&first).unwrap();
    let _first_commit = ring.prepare_commit_plan(&first).unwrap();
    ring.mark_commit_durable(first.ticket()).unwrap();

    let mut second = ring.begin_transaction().unwrap();
    second.stage_metadata(101, &filled(2)).unwrap();
    let second = ring.prepare(&second).unwrap();
    assert_eq!(
        ring.prepare_checkpoint_plan(first.ticket()),
        Err(JournalTransactionError::ReservationOrder)
    );
    let _second_commit = ring.prepare_commit_plan(&second).unwrap();
    ring.mark_commit_durable(second.ticket()).unwrap();

    let first_checkpoint = ring.prepare_checkpoint_plan(first.ticket()).unwrap();
    let advanced = match &first_checkpoint[0] {
        JournalCommitOperation::WriteJournalSuperblock { image, .. } => image,
        _ => panic!("checkpoint must advance the journal tail"),
    };
    assert_eq!(advanced.sequence(), 71);
    assert_eq!(advanced.start_block(), 4);
    ring.checkpoint_durable(first.ticket()).unwrap();

    let second_checkpoint = ring.prepare_checkpoint_plan(second.ticket()).unwrap();
    let clean = match &second_checkpoint[0] {
        JournalCommitOperation::WriteJournalSuperblock { image, .. } => image,
        _ => panic!("final checkpoint must clean the journal"),
    };
    assert_eq!(clean.sequence(), 72);
    assert_eq!(clean.start_block(), 0);
    ring.checkpoint_durable(second.ticket()).unwrap();
}

#[test]
fn superblock_images_admit_only_a_complete_clean_inode_map() {
    let superblock = JournalSuperblockImage::new_clean(70, UUID, 9).unwrap();
    assert_eq!(superblock.maximum_length(), 9);
    assert_eq!(superblock.sequence(), 70);
    assert_eq!(superblock.start_block(), 0);
    assert!(superblock.block_revocations());
    assert_eq!(
        JournalSuperblockImage::from_bytes(superblock.bytes()).unwrap(),
        superblock
    );

    let physical = [3000, 3001, 3002, 3003, 3004, 3005, 3006, 3007, 3008];
    let mut ring = superblock.map_clean_ring(MAXIMUM_BLOCK, &physical).unwrap();
    let mut transaction = ring.begin_transaction().unwrap();
    transaction.stage_metadata(100, &filled(1)).unwrap();
    let prepared = ring.prepare(&transaction).unwrap();
    assert_eq!(prepared.journal_blocks(), &[3001, 3002, 3003]);
    ring.abort_precommit(prepared.ticket()).unwrap();

    let mut overlaps_superblock = ring.begin_transaction().unwrap();
    overlaps_superblock
        .stage_metadata(physical[0], &filled(2))
        .unwrap();
    assert_eq!(
        ring.prepare(&overlaps_superblock),
        Err(JournalTransactionError::JournalSlotOverlap)
    );

    let dirty = superblock.with_state(70, 1).unwrap();
    assert_eq!(dirty.start_block(), 1);
    assert_eq!(
        dirty.map_clean_ring(MAXIMUM_BLOCK, &physical),
        Err(JournalTransactionError::JournalNotClean)
    );
    let clean = dirty.with_state(71, 0).unwrap();
    assert_eq!(clean.sequence(), 71);
    assert_eq!(clean.start_block(), 0);

    let mut malformed = Vec::from(clean.bytes());
    malformed[0] = 0;
    assert_eq!(
        JournalSuperblockImage::from_bytes(&malformed),
        Err(JournalTransactionError::CorruptSuperblockMagic)
    );
    malformed = Vec::from(clean.bytes());
    malformed[7] = 3;
    assert_eq!(
        JournalSuperblockImage::from_bytes(&malformed),
        Err(JournalTransactionError::UnsupportedSuperblockType(3))
    );
    malformed = Vec::from(clean.bytes());
    malformed[0x2b] &= !0x10;
    assert_eq!(
        JournalSuperblockImage::from_bytes(&malformed),
        Err(JournalTransactionError::MissingSuperblockFeatures(0x10))
    );
    malformed = Vec::from(clean.bytes());
    malformed[0x2b] |= 0x20;
    assert_eq!(
        JournalSuperblockImage::from_bytes(&malformed),
        Err(JournalTransactionError::UnsupportedSuperblockFeatures(0x20))
    );
    malformed = Vec::from(clean.bytes());
    malformed[0x50] = 1;
    assert_eq!(
        JournalSuperblockImage::from_bytes(&malformed),
        Err(JournalTransactionError::UnsupportedSuperblockChecksumType(
            1
        ))
    );
    malformed = Vec::from(clean.bytes());
    malformed[128] ^= 0x80;
    assert!(matches!(
        JournalSuperblockImage::from_bytes(&malformed),
        Err(JournalTransactionError::CorruptSuperblockChecksum { .. })
    ));
    assert_eq!(
        clean.with_state(71, 9),
        Err(JournalTransactionError::RingGeometry)
    );
    assert_eq!(
        clean.map_clean_ring(MAXIMUM_BLOCK, &physical[..8]),
        Err(JournalTransactionError::RingGeometry)
    );
    let mut hole = physical;
    hole[4] = 0;
    assert_eq!(
        clean.map_clean_ring(MAXIMUM_BLOCK, &hole),
        Err(JournalTransactionError::RingGeometry)
    );
}

#[test]
fn deterministic_ext4_fixture_discovers_its_real_journal_inode_map() {
    let Ok(path) = std::env::var("SAPOTE_EXT4_RUST_FIXTURE") else {
        eprintln!("SAPOTE_EXT4_RUST_FIXTURE is unset; journal-inode integration is CI-only");
        return;
    };
    let Ok(bytes) = std::fs::read(&path) else {
        eprintln!("journal-inode fixture was not produced because e2fsprogs is unavailable");
        return;
    };
    let filesystem = Ext4::load(Box::new(bytes)).unwrap();
    let journal = load_journal_inode_map(&filesystem).unwrap();
    assert_eq!(journal.superblock().start_block(), 0);
    assert!(!journal.filesystem_needs_recovery());
    let clean_filesystem_superblock = journal.filesystem_superblock().clone();
    assert!(!clean_filesystem_superblock.needs_recovery());
    let recovery_marker = clean_filesystem_superblock.with_recovery_state(true);
    assert!(recovery_marker.needs_recovery());
    assert_eq!(
        u32::from_le_bytes(recovery_marker.bytes()[0x60..0x64].try_into().unwrap()) & 0x4,
        0x4
    );
    assert_eq!(
        u32::from_le_bytes(recovery_marker.bytes()[0x3fc..0x400].try_into().unwrap()),
        ext4_crc32c(&recovery_marker.bytes()[..0x3fc])
    );
    for index in 0..recovery_marker.bytes().len() {
        if !(0x60..0x64).contains(&index) && !(0x3fc..0x400).contains(&index) {
            assert_eq!(
                recovery_marker.bytes()[index],
                clean_filesystem_superblock.bytes()[index]
            );
        }
    }
    assert_eq!(
        recovery_marker.with_recovery_state(false),
        clean_filesystem_superblock
    );
    assert_eq!(
        usize::try_from(journal.superblock().maximum_length()).unwrap(),
        journal.physical_blocks().len()
    );
    assert!(journal.physical_blocks().len() >= 4);
    assert!(journal.physical_blocks().iter().all(|block| *block != 0));

    let empty_ring = vec![filled(0); journal.physical_blocks().len() - 1];
    let empty_references: Vec<&[u8]> = empty_ring.iter().map(Vec::as_slice).collect();
    let marker_only = recover_committed_ring(
        journal.superblock(),
        true,
        journal.maximum_block(),
        &empty_references,
    )
    .unwrap();
    assert_eq!(marker_only.committed_transactions(), 0);
    assert_eq!(marker_only.consumed_slots(), 0);
    assert!(marker_only.replay_images().is_empty());
    assert_eq!(
        marker_only.checkpoint_plan(0, &recovery_marker),
        Err(JournalTransactionError::BlockOutOfRange)
    );
    assert_eq!(
        marker_only.checkpoint_plan(journal.physical_blocks()[0], &clean_filesystem_superblock),
        Err(JournalTransactionError::RecoveryStateMismatch)
    );
    let cleanup = marker_only
        .checkpoint_plan(journal.physical_blocks()[0], &recovery_marker)
        .unwrap();
    assert_eq!(cleanup.len(), 5);
    assert_eq!(cleanup[0], JournalCommitOperation::Flush(JournalFlush::Checkpoint));
    assert!(matches!(
        &cleanup[1],
        JournalCommitOperation::WriteJournalSuperblock { journal_block, image }
            if *journal_block == journal.physical_blocks()[0]
                && image == journal.superblock()
    ));
    assert_eq!(cleanup[2], JournalCommitOperation::Flush(JournalFlush::JournalState));
    assert!(matches!(
        &cleanup[3],
        JournalCommitOperation::WriteFilesystemSuperblock { start_byte, image }
            if *start_byte == FILESYSTEM_SUPERBLOCK_START_BYTE
                && image == &clean_filesystem_superblock
    ));
    assert_eq!(cleanup[4], JournalCommitOperation::Flush(JournalFlush::FilesystemState));

    let physical = Vec::from(journal.physical_blocks());
    let mut ring = journal.into_clean_ring().unwrap();
    let marker_plan = ring.prepare_recovery_marker_plan().unwrap();
    assert!(matches!(
        &marker_plan[0],
        JournalCommitOperation::WriteFilesystemSuperblock { start_byte, image }
            if *start_byte == FILESYSTEM_SUPERBLOCK_START_BYTE
                && image == &recovery_marker
    ));
    assert_eq!(
        marker_plan[1],
        JournalCommitOperation::Flush(JournalFlush::FilesystemState)
    );
    let home_block = (1..MAXIMUM_BLOCK)
        .find(|block| !physical.contains(block))
        .unwrap();
    let mut transaction = ring.begin_transaction().unwrap();
    transaction
        .stage_metadata(home_block, &filled(0x44))
        .unwrap();
    let prepared = ring.prepare(&transaction).unwrap();
    assert_eq!(prepared.journal_blocks(), &physical[1..4]);
    assert_eq!(
        ring.prepare_commit_plan(&prepared),
        Err(JournalTransactionError::RecoveryMarkerNotDurable)
    );
    ring.mark_recovery_marker_durable().unwrap();
    assert!(ring.prepare_commit_plan(&prepared).is_ok());

    let backing = Rc::new(std::fs::read(&path).unwrap());
    let stage = Rc::new(
        JournalMutationStage::new(
            Box::new(backing.clone()),
            u64::try_from(backing.len()).unwrap(),
        )
        .unwrap(),
    );
    let staged_filesystem = Ext4::load_with_writer(
        Box::new(stage.clone()),
        Some(Box::new(stage.clone())),
    )
    .unwrap();
    let mut file = staged_filesystem.open(b"/README.TXT").unwrap();
    let mut original = [0u8; 1];
    assert_eq!(file.read_bytes_at(&mut original, 0).unwrap(), 1);
    assert_eq!(file.write_bytes_at(b"X", 0).unwrap(), 1);
    let mut overlaid = [0u8; 1];
    assert_eq!(file.read_bytes_at(&mut overlaid, 0).unwrap(), 1);
    assert_eq!(overlaid, *b"X");
    assert_ne!(original, overlaid);
    assert!(stage.staged_block_count() >= 1);
    assert_eq!(std::fs::read(&path).unwrap(), backing.as_slice());
    drop(file);
    drop(staged_filesystem);
    stage.rollback();
    let restored_filesystem = Ext4::load(Box::new(stage.clone())).unwrap();
    let mut restored = restored_filesystem.open(b"/README.TXT").unwrap();
    assert_eq!(restored.read_bytes_at(&mut overlaid, 0).unwrap(), 1);
    assert_eq!(overlaid, original);
}

#[test]
fn dirty_ring_recovery_wraps_replays_revokes_and_discards_an_uncommitted_tail() {
    let physical = [3000, 3001, 3002, 3003, 3004, 3005, 3006, 3007, 3008];
    let logical_slots = &physical[1..];
    let superblock = JournalSuperblockImage::new_clean(80, UUID, 9).unwrap();
    let mut ring = superblock.map_clean_ring(MAXIMUM_BLOCK, &physical).unwrap();

    let mut advance_three = ring.begin_transaction().unwrap();
    advance_three.stage_metadata(10, &filled(1)).unwrap();
    let advance_three = ring.prepare(&advance_three).unwrap();
    finish_transaction(&mut ring, &advance_three);

    let mut advance_four = ring.begin_transaction().unwrap();
    advance_four.stage_metadata(11, &filled(2)).unwrap();
    advance_four.stage_metadata(12, &filled(3)).unwrap();
    let advance_four = ring.prepare(&advance_four).unwrap();
    finish_transaction(&mut ring, &advance_four);

    let mut first_live = ring.begin_transaction().unwrap();
    first_live.stage_metadata(100, &filled(0x44)).unwrap();
    let first_live = ring.prepare(&first_live).unwrap();
    assert_eq!(first_live.journal_blocks(), &[3008, 3001, 3002]);
    let _first_commit = ring.prepare_commit_plan(&first_live).unwrap();
    ring.mark_commit_durable(first_live.ticket()).unwrap();

    let mut second_live = ring.begin_transaction().unwrap();
    second_live.stage_metadata(101, &filled(0x55)).unwrap();
    second_live.stage_revocation(100).unwrap();
    let second_live = ring.prepare(&second_live).unwrap();
    assert_eq!(second_live.journal_blocks(), &[3003, 3004, 3005, 3006]);
    let _second_commit = ring.prepare_commit_plan(&second_live).unwrap();
    ring.mark_commit_durable(second_live.ticket()).unwrap();

    let mut storage = vec![filled(0); logical_slots.len()];
    install_journal_writes(logical_slots, &mut storage, &first_live);
    install_journal_writes(logical_slots, &mut storage, &second_live);
    let dirty = superblock.with_state(82, 8).unwrap();
    let references: Vec<&[u8]> = storage.iter().map(Vec::as_slice).collect();
    let recovered = recover_committed_ring(&dirty, true, MAXIMUM_BLOCK, &references).unwrap();
    assert_eq!(recovered.committed_transactions(), 2);
    assert_eq!(recovered.consumed_slots(), 7);
    assert_eq!(recovered.clean_superblock().sequence(), 84);
    assert_eq!(recovered.clean_superblock().start_block(), 0);
    assert_eq!(recovered.replay_images().len(), 1);
    assert_eq!(recovered.replay_images()[0].block_index(), 101);
    assert_eq!(recovered.replay_images()[0].bytes(), filled(0x55));

    let mut uncommitted = JournalTransaction::new(84, UUID, MAXIMUM_BLOCK).unwrap();
    uncommitted.stage_metadata(102, &filled(0x66)).unwrap();
    let uncommitted = uncommitted
        .commit_plan(&[logical_slots[6], logical_slots[7], logical_slots[0]])
        .unwrap();
    let descriptor = uncommitted
        .iter()
        .find_map(|operation| match operation {
            JournalCommitOperation::WriteJournal {
                kind: JournalRecordKind::Descriptor,
                bytes,
                ..
            } => Some(bytes.clone()),
            _ => None,
        })
        .unwrap();
    storage[6] = descriptor;
    let references: Vec<&[u8]> = storage.iter().map(Vec::as_slice).collect();
    let recovered = recover_committed_ring(&dirty, true, MAXIMUM_BLOCK, &references).unwrap();
    assert_eq!(recovered.committed_transactions(), 2);
    assert_eq!(recovered.consumed_slots(), 7);

    storage[5][128] ^= 0x80;
    let references: Vec<&[u8]> = storage.iter().map(Vec::as_slice).collect();
    assert_eq!(
        recover_committed_ring(&dirty, true, MAXIMUM_BLOCK, &references),
        Err(JournalTransactionError::CorruptCommit)
    );

    let clean = superblock.with_state(84, 0).unwrap();
    let marker_only =
        recover_committed_ring(&clean, true, MAXIMUM_BLOCK, &references).unwrap();
    assert_eq!(marker_only.committed_transactions(), 0);
    assert_eq!(marker_only.consumed_slots(), 0);
    assert!(marker_only.replay_images().is_empty());
    assert_eq!(marker_only.clean_superblock(), &clean);
    assert_eq!(
        recover_committed_ring(&dirty, false, MAXIMUM_BLOCK, &references),
        Err(JournalTransactionError::RecoveryStateMismatch)
    );
}

#[test]
fn hostile_geometry_duplicates_escape_and_slot_overlap_are_refused() {
    let mut transaction = JournalTransaction::new(9, UUID, MAXIMUM_BLOCK).unwrap();
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
    escaped[..4].copy_from_slice(&0xc03b_3998_u32.to_be_bytes());
    assert_eq!(
        transaction.stage_metadata(3, &escaped),
        Err(JournalTransactionError::EscapedBlockUnsupported)
    );
    assert_eq!(
        transaction.commit_plan(&[2, 101, 102]),
        Err(JournalTransactionError::JournalSlotOverlap)
    );
    transaction.stage_revocation(4).unwrap();
    let mut ring =
        JournalRing::new_clean(9, UUID, MAXIMUM_BLOCK, false, &[100, 101, 102, 103]).unwrap();
    assert_eq!(
        ring.prepare(&transaction),
        Err(JournalTransactionError::RevocationsUnsupported)
    );
    assert_eq!(
        JournalRing::new_clean(9, UUID, MAXIMUM_BLOCK, true, &[0, 101, 102]),
        Err(JournalTransactionError::RingGeometry)
    );

    let mut unmapped =
        JournalRing::new_clean(20, UUID, MAXIMUM_BLOCK, true, &[100, 101, 102]).unwrap();
    let mut transaction = unmapped.begin_transaction().unwrap();
    transaction.stage_metadata(2, &filled(2)).unwrap();
    let prepared = unmapped.prepare(&transaction).unwrap();
    assert_eq!(
        unmapped.prepare_commit_plan(&prepared),
        Err(JournalTransactionError::JournalStateUnavailable)
    );

    let mut exhausted =
        JournalRing::new_clean(u32::MAX - 1, UUID, MAXIMUM_BLOCK, true, &[100, 101, 102]).unwrap();
    let mut final_sequence = exhausted.begin_transaction().unwrap();
    final_sequence.stage_metadata(2, &filled(2)).unwrap();
    assert_eq!(
        exhausted.prepare(&final_sequence),
        Err(JournalTransactionError::SequenceOverflow)
    );
}
