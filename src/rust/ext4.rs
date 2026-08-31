// SPDX-License-Identifier: GPL-3.0-only
//! Checked ext4 admission over Sapote's native block boundary.

extern crate alloc;

use alloc::boxed::Box;
use core::error::Error;
use core::fmt::{self, Display, Formatter};
use ext4plus::{Ext4, Ext4Read};

/// A pointer-free identity copied from a validated ext4 superblock.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
#[repr(C)]
pub(crate) struct Identity {
    pub(crate) label: [u8; 16],
    pub(crate) uuid: [u8; 16],
}

#[derive(Debug)]
struct BlockReadError;

impl Display for BlockReadError {
    fn fmt(&self, formatter: &mut Formatter<'_>) -> fmt::Result {
        formatter.write_str("Sapote block read failed")
    }
}

impl Error for BlockReadError {}

struct SapoteReader {
    context: usize,
}

impl Ext4Read for SapoteReader {
    fn read(
        &self,
        start_byte: u64,
        destination: &mut [u8],
    ) -> Result<(), Box<dyn Error + Send + Sync + 'static>> {
        if crate::abi::ext4_block_read(
            self.context,
            start_byte,
            destination,
        ) {
            Ok(())
        } else {
            Err(Box::new(BlockReadError))
        }
    }
}

/// Validate ext4 metadata and return its media identity.
pub(crate) fn probe(context: usize) -> Result<Identity, ProbeError> {
    let filesystem = Ext4::load(Box::new(SapoteReader { context }))
        .map_err(|error| {
            if error.as_io().is_some() {
                ProbeError::Io
            } else {
                ProbeError::Invalid
            }
        })?;
    Ok(Identity {
        label: *filesystem.label().as_bytes(),
        uuid: *filesystem.uuid().as_bytes(),
    })
}

/// Stable errors returned across the C boundary.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum ProbeError {
    /// The native block reader failed.
    Io,
    /// The media is not a supported, internally consistent ext4 filesystem.
    Invalid,
}
