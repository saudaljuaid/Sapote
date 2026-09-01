// Copyright 2026 Sapote contributors
//
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// https://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

//! Bounded copy-on-write storage for one synchronous ext4 mutation attempt.

use super::transaction::{
    FILESYSTEM_SUPERBLOCK_START_BYTE, JOURNAL_BLOCK_BYTES,
    JOURNAL_TRANSACTION_MAX_METADATA_BLOCKS, JournalBlockImage,
};
use crate::error::BoxedError;
use crate::sync::RwLock;
use crate::{Ext4Read, Ext4Write};
use alloc::boxed::Box;
use alloc::collections::BTreeMap;
use alloc::vec;
use alloc::vec::Vec;
use core::error::Error;
use core::fmt::{self, Display, Formatter};

/// A refusal while creating or using a bounded mutation stage.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum JournalMutationStageError {
    /// The declared filesystem byte geometry is zero or not 4 KiB aligned.
    Geometry,
    /// A read or write lies outside the declared filesystem byte geometry.
    Range,
    /// One mutation attempt exceeded the stage's transaction safety bound.
    TooManyBlocks,
}

impl Display for JournalMutationStageError {
    fn fmt(&self, formatter: &mut Formatter<'_>) -> fmt::Result {
        let message = match self {
            Self::Geometry => "mutation stage geometry is invalid",
            Self::Range => "mutation stage byte range is invalid",
            Self::TooManyBlocks => "mutation stage block bound exceeded",
        };
        formatter.write_str(message)
    }
}

impl Error for JournalMutationStageError {}

/// A bounded overlay that never writes through to its backing reader.
///
/// The first write to a block reads its complete 4 KiB home image, then every
/// partial write coalesces into that owned copy. Reads observe staged copies.
/// The caller must discard both this stage and the `Ext4` instance that used it
/// after rollback because ext4plus also updates in-memory allocation counters.
#[cfg(feature = "sync")]
pub struct JournalMutationStage {
    reader: Box<dyn Ext4Read>,
    filesystem_bytes: u64,
    blocks: RwLock<BTreeMap<u64, Vec<u8>>>,
}

#[cfg(feature = "sync")]
impl JournalMutationStage {
    /// Create an empty stage over an immutable admitted-filesystem reader.
    pub fn new(
        reader: Box<dyn Ext4Read>,
        filesystem_bytes: u64,
    ) -> Result<Self, JournalMutationStageError> {
        if filesystem_bytes == 0 || filesystem_bytes % JOURNAL_BLOCK_BYTES as u64 != 0 {
            return Err(JournalMutationStageError::Geometry);
        }
        Ok(Self {
            reader,
            filesystem_bytes,
            blocks: RwLock::new(BTreeMap::new()),
        })
    }

    fn range_end(&self, start_byte: u64, length: usize) -> Result<u64, JournalMutationStageError> {
        let length = u64::try_from(length).map_err(|_| JournalMutationStageError::Range)?;
        let end = start_byte
            .checked_add(length)
            .ok_or(JournalMutationStageError::Range)?;
        if start_byte > self.filesystem_bytes || end > self.filesystem_bytes {
            return Err(JournalMutationStageError::Range);
        }
        Ok(end)
    }

    /// Return the number of complete block images currently staged.
    #[must_use]
    pub fn staged_block_count(&self) -> usize {
        self.blocks.read().len()
    }

    /// Snapshot every complete staged block image in ascending block order.
    #[must_use]
    pub fn staged_images(&self) -> Vec<JournalBlockImage> {
        self.blocks
            .read()
            .iter()
            .map(|(block_index, bytes)| {
                JournalBlockImage::from_staged(*block_index, bytes.clone())
            })
            .collect()
    }

    /// Discard every staged byte.
    ///
    /// The associated ext4plus filesystem object must be discarded too.
    pub fn rollback(&self) {
        self.blocks.write().clear();
    }
}

#[cfg(feature = "sync")]
impl Ext4Read for JournalMutationStage {
    fn read(&self, start_byte: u64, destination: &mut [u8]) -> Result<(), BoxedError> {
        self.range_end(start_byte, destination.len())
            .map_err(|error| Box::new(error) as BoxedError)?;
        let mut position = start_byte;
        let mut remaining = destination;
        while !remaining.is_empty() {
            let block_index = position / JOURNAL_BLOCK_BYTES as u64;
            let within = usize::try_from(position % JOURNAL_BLOCK_BYTES as u64)
                .map_err(|_| Box::new(JournalMutationStageError::Range) as BoxedError)?;
            let length = remaining.len().min(JOURNAL_BLOCK_BYTES - within);
            if let Some(image) = self.blocks.read().get(&block_index) {
                remaining[..length].copy_from_slice(&image[within..within + length]);
            } else {
                self.reader.read(position, &mut remaining[..length])?;
            }
            position += length as u64;
            remaining = &mut remaining[length..];
        }
        Ok(())
    }
}

#[cfg(feature = "sync")]
impl Ext4Write for JournalMutationStage {
    fn write(&self, start_byte: u64, source: &[u8]) -> Result<(), BoxedError> {
        self.range_end(start_byte, source.len())
            .map_err(|error| Box::new(error) as BoxedError)?;
        if !source.is_empty() && start_byte < FILESYSTEM_SUPERBLOCK_START_BYTE {
            return Err(Box::new(JournalMutationStageError::Range));
        }
        let mut position = start_byte;
        let mut remaining = source;
        let mut blocks = self.blocks.write();
        while !remaining.is_empty() {
            let block_index = position / JOURNAL_BLOCK_BYTES as u64;
            let block_start = block_index
                .checked_mul(JOURNAL_BLOCK_BYTES as u64)
                .ok_or_else(|| Box::new(JournalMutationStageError::Range) as BoxedError)?;
            let within = usize::try_from(position - block_start)
                .map_err(|_| Box::new(JournalMutationStageError::Range) as BoxedError)?;
            let length = remaining.len().min(JOURNAL_BLOCK_BYTES - within);
            if !blocks.contains_key(&block_index) {
                if blocks.len() >= JOURNAL_TRANSACTION_MAX_METADATA_BLOCKS {
                    return Err(Box::new(JournalMutationStageError::TooManyBlocks));
                }
                let mut image = vec![0; JOURNAL_BLOCK_BYTES];
                self.reader.read(block_start, &mut image)?;
                blocks.insert(block_index, image);
            }
            let image = blocks
                .get_mut(&block_index)
                .ok_or_else(|| Box::new(JournalMutationStageError::Range) as BoxedError)?;
            image[within..within + length].copy_from_slice(&remaining[..length]);
            position += length as u64;
            remaining = &remaining[length..];
        }
        Ok(())
    }
}
