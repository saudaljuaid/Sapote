// SPDX-License-Identifier: GPL-3.0-only
//! Checked, read-only ext4 operations over Sapote's native block boundary.

extern crate alloc;

use alloc::boxed::Box;
use alloc::vec::Vec;
use core::error::Error;
use core::fmt::{self, Display, Formatter};
use ext4plus::error::Ext4Error;
use ext4plus::path::Path;
use ext4plus::{
    Ext4, Ext4Read, FileType, FollowSymlinks, JOURNAL_BLOCK_BYTES,
    JournalExecutionError, JournalFlush, JournalInodeMap, JournalInodeMapError,
    JournalStorage, execute_commit_operations, load_journal_inode_map,
    recover_committed_ring,
};

const SUPERBLOCK_BYTES: usize = 1024;
const SUPERBLOCK_START: u64 = 1024;
const BLOCK_BYTES: u64 = 4096;
const COMPAT_FEATURES: u32 = 0x002c;
const INCOMPAT_FEATURES: u32 = 0x20c2;
const INCOMPAT_RECOVERY_FEATURE: u32 = 0x0004;
const READ_ONLY_FEATURES: u32 = 0x046b;
const MAX_VALIDATED_ENTRIES: usize = 8_192;
const MAX_PENDING_DIRECTORIES: usize = 512;

/// A pointer-free identity copied from a validated ext4 superblock.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
#[repr(C)]
pub(crate) struct Identity {
    pub(crate) label: [u8; 16],
    pub(crate) uuid: [u8; 16],
    pub(crate) recovered_transactions: u32,
    pub(crate) replayed_blocks: u32,
    pub(crate) consumed_slots: u32,
    pub(crate) recovery_performed: u8,
    pub(crate) reserved: [u8; 3],
}

/// Pointer-free metadata returned to C.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
#[repr(C)]
pub(crate) struct Metadata {
    pub(crate) inode: u64,
    pub(crate) size: u64,
    pub(crate) uid: u32,
    pub(crate) gid: u32,
    pub(crate) mode: u16,
    pub(crate) links: u16,
    pub(crate) file_type: u8,
    pub(crate) reserved: [u8; 7],
}

/// One ext4 directory entry returned without borrowing Rust storage.
#[derive(Clone, Copy)]
#[repr(C)]
pub(crate) struct DirectoryEntry {
    pub(crate) metadata: Metadata,
    pub(crate) name_length: u16,
    pub(crate) name: [u8; 255],
    pub(crate) reserved: u8,
}

impl Default for DirectoryEntry {
    fn default() -> Self {
        Self {
            metadata: Metadata::default(),
            name_length: 0,
            name: [0; 255],
            reserved: 0,
        }
    }
}

/// A loaded read-only filesystem. C installs a short NVMe lease per call.
pub(crate) struct Mounted {
    filesystem: Ext4,
}

#[derive(Debug)]
struct BlockReadError;

impl Display for BlockReadError {
    fn fmt(&self, formatter: &mut Formatter<'_>) -> fmt::Result {
        formatter.write_str("Sapote block read failed")
    }
}

impl Error for BlockReadError {}

#[derive(Debug)]
struct BlockStorageError;

impl Display for BlockStorageError {
    fn fmt(&self, formatter: &mut Formatter<'_>) -> fmt::Result {
        formatter.write_str("Sapote block write or flush failed")
    }
}

impl Error for BlockStorageError {}

struct SapoteReader {
    context: usize,
}

impl Ext4Read for SapoteReader {
    fn read(
        &self,
        start_byte: u64,
        destination: &mut [u8],
    ) -> Result<(), Box<dyn Error + Send + Sync + 'static>> {
        if crate::abi::ext4_block_read(self.context, start_byte, destination) {
            Ok(())
        } else {
            Err(Box::new(BlockReadError))
        }
    }
}

struct SapoteJournalStorage {
    context: usize,
}

impl JournalStorage for SapoteJournalStorage {
    type Error = BlockStorageError;

    fn write(&mut self, start_byte: u64, bytes: &[u8]) -> Result<(), Self::Error> {
        if crate::abi::ext4_block_write(self.context, start_byte, bytes) {
            Ok(())
        } else {
            Err(BlockStorageError)
        }
    }

    fn flush(&mut self, _boundary: JournalFlush) -> Result<(), Self::Error> {
        if crate::abi::ext4_block_flush(self.context) {
            Ok(())
        } else {
            Err(BlockStorageError)
        }
    }
}

fn read_u16(bytes: &[u8], offset: usize) -> Option<u16> {
    Some(u16::from_le_bytes(
        bytes.get(offset..offset.checked_add(2)?)?.try_into().ok()?,
    ))
}

fn read_u32(bytes: &[u8], offset: usize) -> Option<u32> {
    Some(u32::from_le_bytes(
        bytes.get(offset..offset.checked_add(4)?)?.try_into().ok()?,
    ))
}

fn validate_profile(context: usize, media_bytes: u64) -> Result<(), Status> {
    let mut superblock = [0u8; SUPERBLOCK_BYTES];
    if !crate::abi::ext4_block_read(context, SUPERBLOCK_START, &mut superblock) {
        return Err(Status::Io);
    }
    let magic = read_u16(&superblock, 0x38).ok_or(Status::Invalid)?;
    let log_block_size = read_u32(&superblock, 0x18).ok_or(Status::Invalid)?;
    let inode_size = read_u16(&superblock, 0x58).ok_or(Status::Invalid)?;
    let descriptor_size = read_u16(&superblock, 0xfe).ok_or(Status::Invalid)?;
    let compat = read_u32(&superblock, 0x5c).ok_or(Status::Invalid)?;
    let incompat = read_u32(&superblock, 0x60).ok_or(Status::Invalid)?;
    let read_only = read_u32(&superblock, 0x64).ok_or(Status::Invalid)?;
    let blocks = u64::from(read_u32(&superblock, 0x04).ok_or(Status::Invalid)?)
        | (u64::from(read_u32(&superblock, 0x150).ok_or(Status::Invalid)?) << 32);
    let free_blocks = u64::from(read_u32(&superblock, 0x0c).ok_or(Status::Invalid)?)
        | (u64::from(read_u32(&superblock, 0x158).ok_or(Status::Invalid)?) << 32);
    let inodes = read_u32(&superblock, 0x00).ok_or(Status::Invalid)?;
    let free_inodes = read_u32(&superblock, 0x10).ok_or(Status::Invalid)?;
    let image_bytes = blocks.checked_mul(BLOCK_BYTES).ok_or(Status::Invalid)?;

    if magic != 0xef53
        || log_block_size != 2
        || inode_size != 256
        || descriptor_size != 64
        || read_u32(&superblock, 0x14) != Some(0)
        || compat != COMPAT_FEATURES
        || incompat & !INCOMPAT_RECOVERY_FEATURE != INCOMPAT_FEATURES
        || read_only != READ_ONLY_FEATURES
        || blocks == 0
        || image_bytes > media_bytes
        || free_blocks > blocks
        || inodes == 0
        || free_inodes > inodes
        || read_u32(&superblock, 0x20) == Some(0)
        || read_u32(&superblock, 0x28) == Some(0)
    {
        return Err(Status::Invalid);
    }
    Ok(())
}

fn absolute_path(path: &[u8]) -> Result<Vec<u8>, Status> {
    if path == b"." {
        return Ok(Vec::from(&b"/"[..]));
    }
    if path.is_empty() || path.len() >= 4096 {
        return Err(Status::Range);
    }
    let capacity = path.len().checked_add(1).ok_or(Status::Range)?;
    let mut absolute = Vec::new();
    absolute
        .try_reserve_exact(capacity)
        .map_err(|_| Status::Range)?;
    absolute.push(b'/');
    absolute.extend_from_slice(path);
    Path::try_from(absolute.as_slice()).map_err(|_| Status::Invalid)?;
    Ok(absolute)
}

fn classify(file_type: FileType) -> Result<u8, Status> {
    if file_type.is_regular_file() {
        Ok(1)
    } else if file_type.is_dir() {
        Ok(2)
    } else if file_type.is_symlink() {
        Ok(3)
    } else {
        Err(Status::Special)
    }
}

fn inode_metadata(inode: &ext4plus::inode::Inode) -> Result<Metadata, Status> {
    Ok(Metadata {
        inode: u64::from(inode.index.get()),
        size: inode.size_in_bytes(),
        uid: inode.uid(),
        gid: inode.gid(),
        mode: inode.mode().bits(),
        links: inode.links_count(),
        file_type: classify(inode.file_type())?,
        reserved: [0; 7],
    })
}

fn map_error(error: Ext4Error) -> Status {
    match error {
        Ext4Error::Io(_) => Status::Io,
        Ext4Error::NotFound => Status::NotFound,
        Ext4Error::NotADirectory => Status::NotDirectory,
        Ext4Error::IsADirectory => Status::IsDirectory,
        Ext4Error::PathTooLong | Ext4Error::FileTooLarge => Status::Range,
        Ext4Error::IsASpecialFile => Status::Special,
        _ => Status::Invalid,
    }
}

fn map_journal_error(error: JournalInodeMapError) -> Status {
    match error {
        JournalInodeMapError::Filesystem(error) => map_error(error),
        JournalInodeMapError::MissingJournal | JournalInodeMapError::Transaction(_) => {
            Status::Invalid
        }
    }
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
struct RecoveryReport {
    transactions: u32,
    replayed_blocks: u32,
    consumed_slots: u32,
}

fn recover_dirty_journal(
    context: usize,
    journal: &JournalInodeMap,
) -> Result<RecoveryReport, Status> {
    let physical_blocks = journal.physical_blocks();
    let slot_count = physical_blocks.len().checked_sub(1).ok_or(Status::Invalid)?;
    let ring_bytes = slot_count
        .checked_mul(JOURNAL_BLOCK_BYTES)
        .ok_or(Status::Range)?;
    let mut storage_bytes = Vec::new();
    storage_bytes
        .try_reserve_exact(ring_bytes)
        .map_err(|_| Status::Range)?;
    storage_bytes.resize(ring_bytes, 0);
    for (index, block) in physical_blocks[1..].iter().enumerate() {
        let start_byte = block.checked_mul(BLOCK_BYTES).ok_or(Status::Range)?;
        let start = index.checked_mul(JOURNAL_BLOCK_BYTES).ok_or(Status::Range)?;
        let end = start
            .checked_add(JOURNAL_BLOCK_BYTES)
            .ok_or(Status::Range)?;
        if !crate::abi::ext4_block_read(
            context,
            start_byte,
            storage_bytes.get_mut(start..end).ok_or(Status::Range)?,
        ) {
            return Err(Status::Io);
        }
    }
    let mut references = Vec::new();
    references
        .try_reserve_exact(slot_count)
        .map_err(|_| Status::Range)?;
    references.extend(storage_bytes.chunks_exact(JOURNAL_BLOCK_BYTES));
    if references.len() != slot_count {
        return Err(Status::Invalid);
    }
    let recovery = recover_committed_ring(
        journal.superblock(),
        true,
        journal.maximum_block(),
        &references,
    )
    .map_err(|_| Status::Invalid)?;
    let report = RecoveryReport {
        transactions: u32::try_from(recovery.committed_transactions())
            .map_err(|_| Status::Range)?,
        replayed_blocks: u32::try_from(recovery.replay_images().len())
            .map_err(|_| Status::Range)?,
        consumed_slots: u32::try_from(recovery.consumed_slots())
            .map_err(|_| Status::Range)?,
    };
    let journal_superblock_block = *physical_blocks.first().ok_or(Status::Invalid)?;
    let operations = recovery
        .checkpoint_plan(journal_superblock_block, journal.filesystem_superblock())
        .map_err(|_| Status::Invalid)?;
    execute_commit_operations(
        &mut SapoteJournalStorage { context },
        &operations,
    )
    .map_err(|error| match error {
        JournalExecutionError::AddressOverflow => Status::Range,
        JournalExecutionError::Storage(_) => Status::Io,
    })?;
    Ok(report)
}

fn validate_xattrs(filesystem: &Ext4, path: &[u8]) -> Result<(), Status> {
    let xattrs = filesystem.list_xattrs(path).map_err(map_error)?;
    for name in xattrs {
        let _value = filesystem
            .get_xattr(path, name.as_slice())
            .map_err(map_error)?;
    }
    Ok(())
}

fn validate_namespace(filesystem: &Ext4) -> Result<(), Status> {
    let mut pending = Vec::new();
    pending.try_reserve_exact(1).map_err(|_| Status::Range)?;
    let mut root = Vec::new();
    root.try_reserve_exact(1).map_err(|_| Status::Range)?;
    root.push(b'/');
    validate_xattrs(filesystem, root.as_slice())?;
    pending.push(root);
    let mut visited = 0usize;
    while let Some(path) = pending.pop() {
        let mut directory = filesystem.read_dir(path.as_slice()).map_err(map_error)?;
        for result in &mut directory {
            let entry = result.map_err(map_error)?;
            let name = entry.file_name();
            if name == "." || name == ".." {
                continue;
            }
            visited = visited.checked_add(1).ok_or(Status::Range)?;
            if visited > MAX_VALIDATED_ENTRIES {
                return Err(Status::Range);
            }
            let entry_path = entry.path();
            let metadata = entry.metadata().map_err(map_error)?;
            validate_xattrs(filesystem, entry_path.as_ref())?;
            let kind = classify(metadata.file_type())?;
            if kind == 2 {
                if pending.len() >= MAX_PENDING_DIRECTORIES {
                    return Err(Status::Range);
                }
                pending.try_reserve(1).map_err(|_| Status::Range)?;
                let path_bytes: &[u8] = entry_path.as_ref();
                let mut owned_path = Vec::new();
                owned_path
                    .try_reserve_exact(path_bytes.len())
                    .map_err(|_| Status::Range)?;
                owned_path.extend_from_slice(path_bytes);
                pending.push(owned_path);
            } else if kind == 3 {
                let _target = filesystem
                    .read_link(entry_path.as_ref())
                    .map_err(map_error)?;
            } else if metadata.size_in_bytes != 0 {
                let mut file = filesystem.open(entry_path.as_ref()).map_err(map_error)?;
                let mut byte = [0u8; 1];
                let first = file.read_bytes_at(&mut byte, 0).map_err(map_error)?;
                let last = file
                    .read_bytes_at(&mut byte, metadata.size_in_bytes - 1)
                    .map_err(map_error)?;
                if first != 1 || last != 1 {
                    return Err(Status::Invalid);
                }
            }
        }
    }
    Ok(())
}

/// Load and validate the exact Sapote ext4 profile and reachable namespace.
pub(crate) fn mount(context: usize, media_bytes: u64) -> Result<(Box<Mounted>, Identity), Status> {
    validate_profile(context, media_bytes)?;
    let mut filesystem = Ext4::load(Box::new(SapoteReader { context })).map_err(map_error)?;
    let mut journal = load_journal_inode_map(&filesystem).map_err(map_journal_error)?;
    let mut recovery = RecoveryReport::default();
    let mut recovery_performed = 0u8;
    if journal.filesystem_needs_recovery() {
        recovery = recover_dirty_journal(context, &journal)?;
        recovery_performed = 1;
        drop(journal);
        drop(filesystem);
        validate_profile(context, media_bytes)?;
        filesystem = Ext4::load(Box::new(SapoteReader { context })).map_err(map_error)?;
        journal = load_journal_inode_map(&filesystem).map_err(map_journal_error)?;
        if journal.filesystem_needs_recovery() {
            return Err(Status::Invalid);
        }
        let _clean_ring = journal.into_clean_ring().map_err(|_| Status::Invalid)?;
    } else {
        let _clean_ring = journal.into_clean_ring().map_err(|_| Status::Invalid)?;
    }
    validate_namespace(&filesystem)?;
    let identity = Identity {
        label: *filesystem.label().as_bytes(),
        uuid: *filesystem.uuid().as_bytes(),
        recovered_transactions: recovery.transactions,
        replayed_blocks: recovery.replayed_blocks,
        consumed_slots: recovery.consumed_slots,
        recovery_performed,
        reserved: [0; 3],
    };
    Ok((Box::new(Mounted { filesystem }), identity))
}

/// Resolve a path and return inode-stable metadata.
pub(crate) fn stat(mounted: &Mounted, path: &[u8]) -> Result<Metadata, Status> {
    let absolute = absolute_path(path)?;
    let checked = Path::try_from(absolute.as_slice()).map_err(|_| Status::Invalid)?;
    let inode = mounted
        .filesystem
        .path_to_inode(checked, FollowSymlinks::All)
        .map_err(map_error)?;
    inode_metadata(&inode)
}

/// Read bytes at a 64-bit offset without changing any shared cursor.
pub(crate) fn pread(
    mounted: &Mounted,
    path: &[u8],
    offset: u64,
    destination: &mut [u8],
) -> Result<usize, Status> {
    let absolute = absolute_path(path)?;
    let mut file = mounted
        .filesystem
        .open(absolute.as_slice())
        .map_err(map_error)?;
    file.read_bytes_at(destination, offset).map_err(map_error)
}

/// Return the visible directory entry at `wanted_index`.
pub(crate) fn directory_entry(
    mounted: &Mounted,
    path: &[u8],
    wanted_index: u64,
) -> Result<Option<DirectoryEntry>, Status> {
    let absolute = absolute_path(path)?;
    let mut directory = mounted
        .filesystem
        .read_dir(absolute.as_slice())
        .map_err(map_error)?;
    let mut visible = 0u64;
    for result in &mut directory {
        let entry = result.map_err(map_error)?;
        let name = entry.file_name();
        if name == "." || name == ".." {
            continue;
        }
        if visible == wanted_index {
            let bytes = name.as_ref();
            let inode = ext4plus::inode::Inode::read(&mounted.filesystem, entry.inode)
                .map_err(map_error)?;
            let mut output = DirectoryEntry {
                metadata: inode_metadata(&inode)?,
                name_length: u16::try_from(bytes.len()).map_err(|_| Status::Range)?,
                ..DirectoryEntry::default()
            };
            output.name[..bytes.len()].copy_from_slice(bytes);
            return Ok(Some(output));
        }
        visible = visible.checked_add(1).ok_or(Status::Range)?;
    }
    Ok(None)
}

/// Stable status codes returned across the C boundary.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(i32)]
pub(crate) enum Status {
    /// The operation succeeded.
    Ok = 0,
    /// A required pointer was null.
    NullArgument = 1,
    /// The native volume lease could not be acquired.
    Volume = 2,
    /// The native block reader failed.
    Io = 3,
    /// The media or path is invalid for the supported profile.
    Invalid = 4,
    /// No inode exists at the path.
    NotFound = 5,
    /// A directory operation targeted a non-directory.
    NotDirectory = 6,
    /// A file operation targeted a directory.
    IsDirectory = 7,
    /// A checked bound was exceeded.
    Range = 8,
    /// Device nodes, FIFOs, and sockets are outside the profile.
    Special = 9,
}

const _: i32 = Status::Volume as i32;
