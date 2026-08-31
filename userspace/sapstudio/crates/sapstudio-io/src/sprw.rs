// SPDX-License-Identifier: GPL-3.0-only
//! `SPRW`: SapStudio's uncompressed mezzanine.
//!
//! ```text
//! offset  size  field
//! 0       4     magic, "SPRW"
//! 4       2     format version, little-endian
//! 6       2     reserved, must be zero
//! 8       4     width
//! 12      4     height
//! 16      1     pixel format
//! 17      1     colour primaries
//! 18      1     transfer function
//! 19      1     matrix coefficients
//! 20      1     range
//! 21      1     chroma siting, or 0 for none
//! 22      1     alpha association, or 0 for a format with no alpha
//! 23      1     reserved, must be zero
//! 24      8     pixel aspect numerator
//! 32      8     pixel aspect denominator
//! 40      8     timebase numerator
//! 48      8     timebase denominator
//! 56      8     frame count
//! 64      32    SHA-256 of bytes 0..64 followed by the payload
//! 96      N     the frames, tightly packed, in order
//! ```
//!
//! The digest covers both the header and samples, including every colour and
//! alpha tag. The format uses no entropy coding. All frames in a file share one
//! description; a description change starts a new file.

use alloc::vec::Vec;

use sapstudio_core::{Digest, Rational, Timebase};
use sapstudio_media::colour::{
    AlphaState, ChromaSiting, ColourDescription, MatrixCoefficients, Primaries, Range,
    TransferFunction,
};
use sapstudio_media::{Frame, FrameDescription, Geometry, PixelFormat};

use crate::bytes::{Reader, Writer};
use crate::status::{IoStatus, Result};

/// The four bytes every reel begins with.
pub const MAGIC: [u8; 4] = *b"SPRW";

/// The format this build writes.
///
/// Version two adds the alpha-association field. Version one was never released.
pub const FORMAT_VERSION: u16 = 2;

/// How long the fixed header is.
pub const HEADER_BYTES: usize = 96;

/// How much of the header the digest covers: everything before the digest
/// field itself.
pub const DESCRIBED_BYTES: usize = 64;

/// The digest a reel carries: its description and its samples together.
fn digest_of(head: &[u8], payload: &[u8]) -> Digest {
    let mut hasher = sapstudio_core::Sha256::new();
    hasher.update(head);
    hasher.update(payload);
    hasher.finish()
}

/// The most frames one reel may hold.
///
/// Twenty-four thousand: about sixteen minutes at 24 frames a second, which is
/// a generous single take and a small fraction of what a compressed format
/// would hold. A reel is a working file, not an archive.
pub const MAX_FRAMES: usize = 24_000;

/// The most bytes one reel may occupy.
pub const MAX_REEL_BYTES: usize = 512 * 1024 * 1024;

/// A run of frames that share one description and one rate.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Reel {
    description: FrameDescription,
    timebase: Timebase,
    frames: Vec<Frame>,
}

impl Reel {
    /// Gather frames into a reel.
    ///
    /// # Errors
    ///
    /// [`IoStatus::ReelDescriptionMismatch`] if any frame is described
    /// differently from the first, or [`IoStatus::TooMany`].
    pub fn new(timebase: Timebase, frames: Vec<Frame>) -> Result<Self> {
        let first = frames.first().ok_or(IoStatus::EmptyReel)?;
        let description = *first.description();
        for frame in &frames {
            if frame.description() != &description {
                return Err(IoStatus::ReelDescriptionMismatch);
            }
        }
        if frames.len() > MAX_FRAMES {
            return Err(IoStatus::TooMany);
        }
        Ok(Self {
            description,
            timebase,
            frames,
        })
    }

    /// What every frame in this reel is.
    #[must_use]
    pub const fn description(&self) -> &FrameDescription {
        &self.description
    }

    /// The rate the frames are counted at.
    #[must_use]
    pub const fn timebase(&self) -> Timebase {
        self.timebase
    }

    /// The frames, in order.
    #[must_use]
    pub fn frames(&self) -> &[Frame] {
        &self.frames
    }

    /// How many frames.
    #[must_use]
    pub fn len(&self) -> usize {
        self.frames.len()
    }

    /// Whether the reel holds nothing. It never does; a reel with no frames
    /// cannot be built.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.frames.is_empty()
    }
}

/// Encode a reel.
///
/// # Errors
///
/// [`IoStatus::PayloadTooLarge`] or [`IoStatus::OutOfMemory`].
pub fn encode(reel: &Reel) -> Result<Vec<u8>> {
    let mut payload = Writer::new(MAX_REEL_BYTES);
    for frame in reel.frames() {
        payload.bytes(&frame.to_packed()?)?;
    }
    let payload = payload.finish();

    let description = reel.description();
    let geometry = description.geometry();
    let colour = description.colour();

    let mut head = Writer::new(DESCRIBED_BYTES);
    head.bytes(&MAGIC)?;
    head.u16(FORMAT_VERSION)?;
    head.u16(0)?;
    head.u32(geometry.width())?;
    head.u32(geometry.height())?;
    head.u8(format_tag(description.format()))?;
    head.u8(primaries_tag(colour.primaries))?;
    head.u8(transfer_tag(colour.transfer))?;
    head.u8(matrix_tag(colour.matrix))?;
    head.u8(range_tag(colour.range))?;
    head.u8(siting_tag(description.siting()))?;
    head.u8(alpha_tag(description.alpha()))?;
    head.u8(0)?;
    head.i64(description.pixel_aspect().numerator())?;
    head.i64(description.pixel_aspect().denominator())?;
    head.i64(reel.timebase().rate().numerator())?;
    head.i64(reel.timebase().rate().denominator())?;
    head.u64(reel.len() as u64)?;
    let head = head.finish();

    let mut file = Writer::new(HEADER_BYTES + MAX_REEL_BYTES);
    file.bytes(&head)?;
    file.bytes(digest_of(&head, &payload).bytes())?;
    file.bytes(&payload)?;
    Ok(file.finish())
}

/// Decode a reel.
///
/// # Errors
///
/// Any [`IoStatus`]. Nothing is returned on a refusal.
pub fn decode(file: &[u8]) -> Result<Reel> {
    if file.len() < HEADER_BYTES {
        return Err(IoStatus::TruncatedHeader);
    }
    let mut header = Reader::new(&file[..HEADER_BYTES]);
    if header.take(4)? != MAGIC {
        return Err(IoStatus::NotAReel);
    }
    let version = header.u16()?;
    if version != FORMAT_VERSION {
        return Err(IoStatus::UnsupportedVersion(version));
    }
    if header.u16()? != 0 {
        return Err(IoStatus::ReservedFieldSet);
    }
    let width = header.u32()?;
    let height = header.u32()?;
    let format = read_format(header.u8()?)?;
    let colour = ColourDescription::new(
        read_primaries(header.u8()?)?,
        read_transfer(header.u8()?)?,
        read_matrix(header.u8()?)?,
        read_range(header.u8()?)?,
    );
    let siting = read_siting(header.u8()?)?;
    let alpha = read_alpha(header.u8()?)?;
    if header.u8()? != 0 {
        return Err(IoStatus::ReservedFieldSet);
    }
    let pixel_aspect = Rational::new(header.i64()?, header.i64()?)?;
    let timebase = Timebase::new(Rational::new(header.i64()?, header.i64()?)?)?;
    let declared = header.u64()?;
    let expected = Digest::new(header.digest_bytes()?);

    let geometry = Geometry::new(width, height)?;
    let description = FrameDescription::new(geometry, format, colour, siting, alpha, pixel_aspect)?;

    let count = usize::try_from(declared).map_err(|_| IoStatus::TooMany)?;
    if count > MAX_FRAMES {
        return Err(IoStatus::TooMany);
    }
    if count == 0 {
        return Err(IoStatus::EmptyReel);
    }

    let frame_bytes = description.packed_bytes()?;
    let required = frame_bytes
        .checked_mul(count)
        .ok_or(IoStatus::PayloadTooLarge)?;
    let payload = &file[HEADER_BYTES..];
    if payload.len() < required {
        return Err(IoStatus::TruncatedPayload);
    }
    if payload.len() > required {
        return Err(IoStatus::TrailingBytes);
    }
    if digest_of(&file[..DESCRIBED_BYTES], payload) != expected {
        return Err(IoStatus::DigestMismatch);
    }

    let mut frames = Vec::new();
    frames
        .try_reserve(count)
        .map_err(|_| IoStatus::OutOfMemory)?;
    for index in 0..count {
        let start = index * frame_bytes;
        let slice = payload
            .get(start..start + frame_bytes)
            .ok_or(IoStatus::TruncatedPayload)?;
        frames.push(Frame::from_packed(description, slice)?);
    }
    Reel::new(timebase, frames)
}

/// Tags as they appear in the file. Deliberately their own numbers: a frame's
/// digest must not change because this container renumbered something.
const fn format_tag(format: PixelFormat) -> u8 {
    match format {
        PixelFormat::Rgba8 => 1,
        PixelFormat::Rgb8 => 2,
        PixelFormat::Gray8 => 3,
        PixelFormat::Yuv420p8 => 4,
        PixelFormat::Yuv422p8 => 5,
        PixelFormat::Yuv444p8 => 6,
    }
}

fn read_format(tag: u8) -> Result<PixelFormat> {
    match tag {
        1 => Ok(PixelFormat::Rgba8),
        2 => Ok(PixelFormat::Rgb8),
        3 => Ok(PixelFormat::Gray8),
        4 => Ok(PixelFormat::Yuv420p8),
        5 => Ok(PixelFormat::Yuv422p8),
        6 => Ok(PixelFormat::Yuv444p8),
        other => Err(IoStatus::UnknownPixelFormat(other)),
    }
}

const fn primaries_tag(primaries: Primaries) -> u8 {
    match primaries {
        Primaries::Bt709 => 1,
        Primaries::Bt601Ntsc => 2,
        Primaries::Bt601Pal => 3,
        Primaries::Bt2020 => 4,
        Primaries::DciP3 => 5,
        Primaries::DisplayP3 => 6,
        Primaries::AcesAp0 => 7,
        Primaries::AcesAp1 => 8,
    }
}

fn read_primaries(tag: u8) -> Result<Primaries> {
    match tag {
        1 => Ok(Primaries::Bt709),
        2 => Ok(Primaries::Bt601Ntsc),
        3 => Ok(Primaries::Bt601Pal),
        4 => Ok(Primaries::Bt2020),
        5 => Ok(Primaries::DciP3),
        6 => Ok(Primaries::DisplayP3),
        7 => Ok(Primaries::AcesAp0),
        8 => Ok(Primaries::AcesAp1),
        other => Err(IoStatus::UnknownColourTag(other)),
    }
}

const fn transfer_tag(transfer: TransferFunction) -> u8 {
    match transfer {
        TransferFunction::Bt709 => 1,
        TransferFunction::Srgb => 2,
        TransferFunction::Bt2020Ten => 3,
        TransferFunction::PerceptualQuantiser => 4,
        TransferFunction::HybridLogGamma => 5,
        TransferFunction::Linear => 6,
        TransferFunction::Gamma22 => 7,
        TransferFunction::Gamma26 => 8,
    }
}

fn read_transfer(tag: u8) -> Result<TransferFunction> {
    match tag {
        1 => Ok(TransferFunction::Bt709),
        2 => Ok(TransferFunction::Srgb),
        3 => Ok(TransferFunction::Bt2020Ten),
        4 => Ok(TransferFunction::PerceptualQuantiser),
        5 => Ok(TransferFunction::HybridLogGamma),
        6 => Ok(TransferFunction::Linear),
        7 => Ok(TransferFunction::Gamma22),
        8 => Ok(TransferFunction::Gamma26),
        other => Err(IoStatus::UnknownColourTag(other)),
    }
}

const fn matrix_tag(matrix: MatrixCoefficients) -> u8 {
    match matrix {
        MatrixCoefficients::Identity => 1,
        MatrixCoefficients::Bt709 => 2,
        MatrixCoefficients::Bt601 => 3,
        MatrixCoefficients::Bt2020NonConstant => 4,
    }
}

fn read_matrix(tag: u8) -> Result<MatrixCoefficients> {
    match tag {
        1 => Ok(MatrixCoefficients::Identity),
        2 => Ok(MatrixCoefficients::Bt709),
        3 => Ok(MatrixCoefficients::Bt601),
        4 => Ok(MatrixCoefficients::Bt2020NonConstant),
        other => Err(IoStatus::UnknownColourTag(other)),
    }
}

const fn range_tag(range: Range) -> u8 {
    match range {
        Range::Limited => 1,
        Range::Full => 2,
    }
}

fn read_range(tag: u8) -> Result<Range> {
    match tag {
        1 => Ok(Range::Limited),
        2 => Ok(Range::Full),
        other => Err(IoStatus::UnknownColourTag(other)),
    }
}

const fn alpha_tag(alpha: Option<AlphaState>) -> u8 {
    match alpha {
        None => 0,
        Some(AlphaState::Straight) => 1,
        Some(AlphaState::Premultiplied) => 2,
    }
}

fn read_alpha(tag: u8) -> Result<Option<AlphaState>> {
    match tag {
        0 => Ok(None),
        1 => Ok(Some(AlphaState::Straight)),
        2 => Ok(Some(AlphaState::Premultiplied)),
        other => Err(IoStatus::UnknownColourTag(other)),
    }
}

const fn siting_tag(siting: Option<ChromaSiting>) -> u8 {
    match siting {
        None => 0,
        Some(ChromaSiting::Left) => 1,
        Some(ChromaSiting::Centre) => 2,
        Some(ChromaSiting::TopLeft) => 3,
    }
}

fn read_siting(tag: u8) -> Result<Option<ChromaSiting>> {
    match tag {
        0 => Ok(None),
        1 => Ok(Some(ChromaSiting::Left)),
        2 => Ok(Some(ChromaSiting::Centre)),
        3 => Ok(Some(ChromaSiting::TopLeft)),
        other => Err(IoStatus::UnknownColourTag(other)),
    }
}
