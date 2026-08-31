// SPDX-License-Identifier: GPL-3.0-only
#![cfg(test)]

use ext4plus::{
    Ext4, JOURNAL_BLOCK_BYTES, JournalCommitOperation, JournalFlush, JournalPreparedTransaction,
    JournalRecordKind, JournalRing, JournalSuperblockImage, JournalTransaction,
    JournalTransactionError, load_journal_inode_map, recover_committed_ring,
    replay_committed_transaction,
};
use std::collections::BTreeMap;

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
    let mut ring = JournalRing::new_clean(40, UUID, MAXIMUM_BLOCK, true, &slots).unwrap();
    let mut first = ring.begin_transaction().unwrap();
    first.stage_metadata(100, &filled(1)).unwrap();
    let first = ring.prepare(&first).unwrap();
    assert_eq!(first.journal_blocks(), &slots[..3]);
    assert_eq!(
        ring.checkpoint_durable(first.ticket()),
        Err(JournalTransactionError::ReservationState)
    );
    ring.mark_commit_durable(first.ticket()).unwrap();
    ring.checkpoint_durable(first.ticket()).unwrap();

    let mut second = ring.begin_transaction().unwrap();
    second.stage_metadata(101, &filled(2)).unwrap();
    second.stage_metadata(102, &filled(3)).unwrap();
    let second = ring.prepare(&second).unwrap();
    ring.mark_commit_durable(second.ticket()).unwrap();
    ring.checkpoint_durable(second.ticket()).unwrap();

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
    let Ok(bytes) = std::fs::read(path) else {
        eprintln!("journal-inode fixture was not produced because e2fsprogs is unavailable");
        return;
    };
    let filesystem = Ext4::load(Box::new(bytes)).unwrap();
    let journal = load_journal_inode_map(&filesystem).unwrap();
    assert_eq!(journal.superblock().start_block(), 0);
    assert!(!journal.filesystem_needs_recovery());
    assert_eq!(
        usize::try_from(journal.superblock().maximum_length()).unwrap(),
        journal.physical_blocks().len()
    );
    assert!(journal.physical_blocks().len() >= 4);
    assert!(journal.physical_blocks().iter().all(|block| *block != 0));

    let physical = Vec::from(journal.physical_blocks());
    let mut ring = journal.into_clean_ring().unwrap();
    let home_block = (1..MAXIMUM_BLOCK)
        .find(|block| !physical.contains(block))
        .unwrap();
    let mut transaction = ring.begin_transaction().unwrap();
    transaction
        .stage_metadata(home_block, &filled(0x44))
        .unwrap();
    let prepared = ring.prepare(&transaction).unwrap();
    assert_eq!(prepared.journal_blocks(), &physical[1..4]);
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
    ring.mark_commit_durable(advance_three.ticket()).unwrap();
    ring.checkpoint_durable(advance_three.ticket()).unwrap();

    let mut advance_four = ring.begin_transaction().unwrap();
    advance_four.stage_metadata(11, &filled(2)).unwrap();
    advance_four.stage_metadata(12, &filled(3)).unwrap();
    let advance_four = ring.prepare(&advance_four).unwrap();
    ring.mark_commit_durable(advance_four.ticket()).unwrap();
    ring.checkpoint_durable(advance_four.ticket()).unwrap();

    let mut first_live = ring.begin_transaction().unwrap();
    first_live.stage_metadata(100, &filled(0x44)).unwrap();
    let first_live = ring.prepare(&first_live).unwrap();
    assert_eq!(first_live.journal_blocks(), &[3008, 3001, 3002]);
    ring.mark_commit_durable(first_live.ticket()).unwrap();

    let mut second_live = ring.begin_transaction().unwrap();
    second_live.stage_metadata(101, &filled(0x55)).unwrap();
    second_live.stage_revocation(100).unwrap();
    let second_live = ring.prepare(&second_live).unwrap();
    assert_eq!(second_live.journal_blocks(), &[3003, 3004, 3005, 3006]);
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
    assert_eq!(
        recover_committed_ring(&clean, true, MAXIMUM_BLOCK, &references),
        Err(JournalTransactionError::RecoveryStateMismatch)
    );
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

    let mut exhausted =
        JournalRing::new_clean(u32::MAX - 1, UUID, MAXIMUM_BLOCK, true, &[100, 101, 102]).unwrap();
    let mut final_sequence = exhausted.begin_transaction().unwrap();
    final_sequence.stage_metadata(2, &filled(2)).unwrap();
    assert_eq!(
        exhausted.prepare(&final_sequence),
        Err(JournalTransactionError::SequenceOverflow)
    );
}
