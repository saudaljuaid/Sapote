// SPDX-License-Identifier: GPL-3.0-only
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_op_in_unsafe_fn)]
#![allow(
    clippy::doc_markdown,
    reason = "SapStudio and Sapote are product names, not identifiers"
)]
//! SapStudio's boundary to the Sapote native ABI.
//!
//! This crate contains the raw syscall sequence, native call numbers, and the
//! service traits used by higher layers. It is one of the crates allowed to
//! contain `unsafe` (R-3.1.4). [`seam`] keeps the application logic testable
//! with host implementations of the same services.

pub mod seam;
pub mod syscall;

pub use seam::{Console, Slot, Storage, Time};
pub use syscall::{ABI_VERSION, AbiVersion};
