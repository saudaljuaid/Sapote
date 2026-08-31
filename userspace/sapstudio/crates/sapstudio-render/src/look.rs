// SPDX-License-Identifier: GPL-3.0-only
//! Apply a 3D lookup table to a frame.
//!
//! A [`Look`] records the encoding its table expects and rejects mismatched
//! frames. LUTs operate on straight-alpha code values, not premultiplied or
//! linear-light samples. Callers must convert or unpremultiply explicitly.
//! Partial strength uses `c + s·(f(c) - c)` in the LUT's own code-value space.

use alloc::vec::Vec;

use sapstudio_core::{Digest, Rational, Sha256};
use sapstudio_media::colour::{AlphaState, ColourDescription, MatrixCoefficients};
use sapstudio_media::{Frame, FrameDescription, Geometry, PixelFormat};

use crate::convert::{normalise, quantise};
use crate::lut::{Interpolation, Lut3D};
use crate::status::{RenderStatus, Result};

/// A lookup table, and what it was authored for.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Look {
    /// The cube.
    table: Lut3D,
    /// The encoding the cube's inputs are in.
    expects: ColourDescription,
    /// How to interpolate between its samples.
    interpolation: Interpolation,
}

impl Look {
    /// Pair a table with the encoding it was authored for.
    #[must_use]
    pub const fn new(
        table: Lut3D,
        expects: ColourDescription,
        interpolation: Interpolation,
    ) -> Self {
        Self {
            table,
            expects,
            interpolation,
        }
    }

    /// The cube.
    #[must_use]
    pub const fn table(&self) -> &Lut3D {
        &self.table
    }

    /// The encoding this look expects its input in.
    #[must_use]
    pub const fn expects(&self) -> ColourDescription {
        self.expects
    }

    /// How this look interpolates between its samples.
    #[must_use]
    pub const fn interpolation(&self) -> Interpolation {
        self.interpolation
    }

    /// What this look *is*, as a digest over everything that changes what it
    /// does.
    ///
    /// The samples, the encoding it expects, and the interpolation — because
    /// two of those three are as capable of changing every pixel as the cube
    /// is. A digest over the samples alone would make a table read
    /// tetrahedrally and the same table read trilinearly the same look, and a
    /// cache holding one would answer for the other.
    ///
    /// This is what a project stores and what a render graph names a grade by,
    /// for the same reason media is named by content: the same look in two
    /// projects is the same look, and a handle would cache it twice and could
    /// not tell that the file behind it had been swapped.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::Media`] if the encoding cannot be written down, which
    /// no description this type accepts can fail to be.
    pub fn digest(&self) -> Result<Digest> {
        let mut hasher = Sha256::new();
        hasher.update(b"sapstudio-look-v1");
        hasher.update(&[match self.interpolation {
            Interpolation::Tetrahedral => 1,
            Interpolation::Trilinear => 2,
        }]);
        // Reuse the frame format tags so look identities share one encoding.
        let format = if self.expects.matrix == MatrixCoefficients::Identity {
            PixelFormat::Rgb8
        } else {
            PixelFormat::Yuv444p8
        };
        let description = FrameDescription::square(
            Geometry::new(2, 2).map_err(RenderStatus::Media)?,
            format,
            self.expects,
            None,
            None,
        )
        .map_err(RenderStatus::Media)?;
        let witness = Frame::blank(description).map_err(RenderStatus::Media)?;
        hasher.update(witness.digest().bytes());

        let size = self.table.size();
        hasher.update(&u32::try_from(size).unwrap_or(u32::MAX).to_le_bytes());
        for blue in 0..size {
            for green in 0..size {
                for red in 0..size {
                    for component in self.table.sample(red, green, blue)? {
                        hasher.update(&component.raw().to_le_bytes());
                    }
                }
            }
        }
        Ok(hasher.finish())
    }

    /// Apply this look at a strength from zero to one.
    ///
    /// The output keeps the input description because a look changes code
    /// values within the same encoding. Zero returns the frame unchanged and
    /// one applies the full table. Mixing happens in the LUT's code-value
    /// domain.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::LookSpaceMismatch`] if the frame is not in the encoding
    /// the table was authored for, [`RenderStatus::LookNotRgb`] for a format
    /// that is not red-green-blue, [`RenderStatus::LookPremultiplied`] for
    /// premultiplied coverage, [`RenderStatus::LookStrengthOutOfRange`] for a
    /// strength outside nought to one, and [`RenderStatus::OutOfMemory`].
    ///
    /// Model curves may overshoot and are clamped before this API is called;
    /// direct callers must provide an in-range strength.
    pub fn apply(&self, frame: &Frame, strength: Rational) -> Result<Frame> {
        if strength < Rational::ZERO || strength > Rational::ONE {
            return Err(RenderStatus::LookStrengthOutOfRange);
        }
        let description = *frame.description();
        if description.colour() != self.expects {
            return Err(RenderStatus::LookSpaceMismatch);
        }
        let channels = match description.format() {
            PixelFormat::Rgb8 => 3,
            PixelFormat::Rgba8 => 4,
            // A table maps three numbers to three. A luma-chroma frame needs
            // the matrix taken out of it first and a grey frame has nowhere to
            // put a colour, and each of those is a named step of its own
            // rather than something to do on the way past.
            _ => return Err(RenderStatus::LookNotRgb),
        };
        if description.alpha() == Some(AlphaState::Premultiplied) {
            return Err(RenderStatus::LookPremultiplied);
        }

        let range = description.colour().range;
        let packed = frame.to_packed().map_err(RenderStatus::Media)?;
        let mut out = Vec::new();
        out.try_reserve(packed.len())
            .map_err(|_| RenderStatus::OutOfMemory)?;

        // Converted once for the frame rather than once per pixel. Exact at
        // both ends: `from_rational` rounds half away from zero, and nought
        // and one are both representable to the last bit.
        let mixture = crate::Fixed::from_rational(strength)?;
        for pixel in packed.chunks_exact(channels) {
            let mut colour = [crate::Fixed::ZERO; 3];
            for (channel, slot) in colour.iter_mut().enumerate() {
                *slot = normalise(pixel[channel], range, false)?;
            }
            let looked = self.table.look_up(colour, self.interpolation)?;
            for (channel, component) in looked.into_iter().enumerate() {
                out.push(quantise(
                    mix(colour[channel], component, mixture)?,
                    range,
                    false,
                )?);
            }
            if channels == 4 {
                // Coverage carried through untouched. A table knows nothing
                // about alpha, and the last time something in this crate wrote
                // a constant here instead, every keyed frame that went through
                // it came out a solid rectangle.
                out.push(pixel[3]);
            }
        }

        Frame::from_packed(description, &out).map_err(RenderStatus::Media)
    }
}

/// Move `from` toward `to` by `strength`.
///
/// The difference form performs one fixed-point multiplication instead of two,
/// which reduces rounding error. Endpoints remain exact in either form.
fn mix(from: crate::Fixed, to: crate::Fixed, strength: crate::Fixed) -> Result<crate::Fixed> {
    Ok(from.checked_add(to.checked_sub(from)?.checked_mul(strength)?)?)
}
