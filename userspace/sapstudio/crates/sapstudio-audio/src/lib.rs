// SPDX-License-Identifier: GPL-3.0-only
#![cfg_attr(not(test), no_std)]
#![allow(
    clippy::doc_markdown,
    reason = "SapStudio and Sapote are product names, not identifiers"
)]
//! Deterministic integer audio processing.
//!
//! This crate provides rate-aware buffers, decibel gain, constant-power pan,
//! loudness measurement, and bounded mixing. Logarithms and square roots use
//! [`sapstudio_core::Fixed`] for bit-for-bit results without libm (R-4.1).
//! Resampling, dither, and filtering are outside this crate's API (R-1.3).

extern crate alloc;

pub mod buffer;
pub mod gain;
pub mod loudness;
pub mod mix;
pub mod overview;
pub mod pan;
pub mod status;

pub use buffer::{
    AudioBuffer, FULL_SCALE, MAX_CHANNELS, MAX_SAMPLES, NEGATIVE_FULL_SCALE, SampleRate,
};
pub use gain::{Gain, MAXIMUM_DECIBELS, MINIMUM_DECIBELS};
pub use loudness::{integrated, momentary};
pub use mix::{MixReport, Source, mix, pan_to_stereo};
pub use overview::{Bucket, Overview};
pub use pan::Pan;
pub use status::{AudioStatus, Result};
