// SPDX-License-Identifier: GPL-3.0-only
#![cfg(test)]

use ext4plus::{
    JOURNAL_BLOCK_BYTES, JournalCommitOperation, JournalFlush, JournalRecordKind, JournalRing,
    JournalTransaction, JournalTransactionError, replay_committed_transaction,
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
    let mut ring = JournalRing::new_clean(40, UUID, MAXIMUM_BLOCK, &slots).unwrap();
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
}
