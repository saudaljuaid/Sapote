// SPDX-License-Identifier: GPL-3.0-only
//! Scale and position frames.
//!
//! Resampling works on premultiplied samples in linear light. [`Mapping::new`]
//! inverts the caller's source-to-destination transform so each destination
//! pixel can find its source region. Singular transforms are rejected.
//! [`Filter::Area`] is intended for reduction; [`Filter::Bilinear`] is intended
//! for enlargement. The caller selects the filter explicitly.

use alloc::vec::Vec;

use sapstudio_core::{FRACTION_BITS, Fixed, Rational};
use sapstudio_media::{AlphaState, Frame, FrameDescription, PixelFormat};

use crate::convert::TransferTable;
use crate::status::{RenderStatus, Result};

/// How many bytes one `Rgba8` pixel occupies.
const CHANNELS: usize = 4;

/// Which of those bytes is the alpha.
const ALPHA: usize = 3;

/// The largest picture this will resample to or from, on either axis.
pub const MAX_EXTENT: usize = 16_384;

/// How to weigh the source under a destination pixel.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Filter {
    /// The exact area-weighted mean of the source region a destination pixel
    /// covers. Right for reduction; nearest-neighbour in effect for
    /// enlargement, because a destination pixel inside one source pixel
    /// covers only that one.
    Area,
    /// The four samples around where a destination pixel's centre lands,
    /// weighted by how near it is to each. Right for enlargement; loses
    /// detail on reduction, because it looks at four samples however many the
    /// destination pixel actually spans.
    Bilinear,
}

/// An affine map from source pixel coordinates to destination ones.
///
/// Stored inverted, because that is the direction a resampler walks.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Mapping {
    /// The inverse map's linear part, row-major.
    inverse: [Rational; 4],
    /// The inverse map's translation.
    offset: (Rational, Rational),
    /// How much larger one destination pixel is than one source pixel, which
    /// is the determinant of the inverse and is what an area average divides
    /// by. Kept because it is exact and computing it per pixel would be four
    /// multiplications to arrive at the same number every time.
    footprint: Rational,
}

impl Mapping {
    /// From the forward map `(x, y) -> (a·x + b·y + e, c·x + d·y + f)`.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::Singular`] for a map with no inverse — one that
    /// squashes the picture onto a line, where every destination pixel would
    /// draw from a region of no area — and [`RenderStatus::Time`] wrapping an
    /// overflow.
    pub fn new(linear: [Rational; 4], offset: (Rational, Rational)) -> Result<Self> {
        let [a, b, c, d] = linear;
        let determinant = a
            .checked_mul(d)
            .map_err(RenderStatus::Time)?
            .checked_sub(b.checked_mul(c).map_err(RenderStatus::Time)?)
            .map_err(RenderStatus::Time)?;
        if determinant.is_zero() {
            return Err(RenderStatus::Singular);
        }
        let invert = |value: Rational| value.checked_div(determinant).map_err(RenderStatus::Time);
        let inverse = [
            invert(d)?,
            invert(b.checked_neg().map_err(RenderStatus::Time)?)?,
            invert(c.checked_neg().map_err(RenderStatus::Time)?)?,
            invert(a)?,
        ];
        // The inverse also moves the origin: `-M⁻¹ · offset`.
        let shifted = (
            apply(inverse, offset)
                .0
                .checked_neg()
                .map_err(RenderStatus::Time)?,
            apply(inverse, offset)
                .1
                .checked_neg()
                .map_err(RenderStatus::Time)?,
        );
        let footprint = inverse[0]
            .checked_mul(inverse[3])
            .map_err(RenderStatus::Time)?
            .checked_sub(
                inverse[1]
                    .checked_mul(inverse[2])
                    .map_err(RenderStatus::Time)?,
            )
            .map_err(RenderStatus::Time)?;
        Ok(Self {
            inverse,
            offset: shifted,
            footprint: magnitude(footprint)?,
        })
    }

    /// A scale about the origin, with a translation after it.
    ///
    /// # Errors
    ///
    /// As [`Mapping::new`], and [`RenderStatus::Singular`] for a scale of
    /// nought on either axis.
    pub fn scaled(across: Rational, down: Rational, offset: (Rational, Rational)) -> Result<Self> {
        Self::new([across, Rational::ZERO, Rational::ZERO, down], offset)
    }

    /// Build a frame-relative transform about an anchor.
    ///
    /// The linear part is dimensionless; offset and anchor are fractions of
    /// the picture. The anchor may lie outside the frame. It is converted to
    /// pixels here because folding a fractional anchor into the offset is not
    /// correct for rotations on non-square frames.
    ///
    /// # Errors
    ///
    /// As [`Mapping::new`], and [`RenderStatus::OutsideDomain`] for a picture
    /// past [`MAX_EXTENT`].
    pub fn about(
        linear: [Rational; 4],
        offset: (Rational, Rational),
        anchor: (Rational, Rational),
        width: u32,
        height: u32,
    ) -> Result<Self> {
        if width == 0
            || height == 0
            || usize::try_from(width).map_err(|_| RenderStatus::OutsideDomain)? > MAX_EXTENT
            || usize::try_from(height).map_err(|_| RenderStatus::OutsideDomain)? > MAX_EXTENT
        {
            return Err(RenderStatus::OutsideDomain);
        }
        let across = Rational::from_integer(i64::from(width));
        let down = Rational::from_integer(i64::from(height));
        let pivot = (
            across.checked_mul(anchor.0).map_err(RenderStatus::Time)?,
            down.checked_mul(anchor.1).map_err(RenderStatus::Time)?,
        );
        // Acting about the pivot contributes `pivot - M·pivot` on each axis,
        // and the move is a fraction of the picture rather than a distance.
        let turned = apply(linear, pivot);
        let shifted = (
            pivot
                .0
                .checked_sub(turned.0)
                .map_err(RenderStatus::Time)?
                .checked_add(across.checked_mul(offset.0).map_err(RenderStatus::Time)?)
                .map_err(RenderStatus::Time)?,
            pivot
                .1
                .checked_sub(turned.1)
                .map_err(RenderStatus::Time)?
                .checked_add(down.checked_mul(offset.1).map_err(RenderStatus::Time)?)
                .map_err(RenderStatus::Time)?,
        );
        Self::new(linear, shifted)
    }

    /// Where a destination point came from in the source, for a test that
    /// needs to ask about a *point* rather than about a pixel.
    ///
    /// A pixel is a square and a fixed point is a point, so the one property
    /// that defines a pivot — that it does not move — cannot be asked of the
    /// resampled picture. Exposed rather than reimplemented in the test,
    /// because a test that recomputed the inverse would be checking its own
    /// arithmetic against itself.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::Time`] wrapping an overflow.
    pub fn source_of_for_test(&self, x: Rational, y: Rational) -> Result<(Rational, Rational)> {
        self.source_of(x, y)
    }

    /// Where a destination point came from in the source.
    fn source_of(&self, x: Rational, y: Rational) -> Result<(Rational, Rational)> {
        let mapped = apply(self.inverse, (x, y));
        Ok((
            mapped
                .0
                .checked_add(self.offset.0)
                .map_err(RenderStatus::Time)?,
            mapped
                .1
                .checked_add(self.offset.1)
                .map_err(RenderStatus::Time)?,
        ))
    }
}

/// A two-by-two matrix times a point.
fn apply(matrix: [Rational; 4], point: (Rational, Rational)) -> (Rational, Rational) {
    let across = matrix[0]
        .checked_mul(point.0)
        .and_then(|held| held.checked_add(matrix[1].checked_mul(point.1)?))
        .unwrap_or(Rational::ZERO);
    let down = matrix[2]
        .checked_mul(point.0)
        .and_then(|held| held.checked_add(matrix[3].checked_mul(point.1)?))
        .unwrap_or(Rational::ZERO);
    (across, down)
}

/// A rational without its sign.
fn magnitude(value: Rational) -> Result<Rational> {
    if value.numerator() < 0 {
        value.checked_neg().map_err(RenderStatus::Time)
    } else {
        Ok(value)
    }
}

/// Move a premultiplied frame onto a new one.
///
/// Everything outside the source contributes nothing — no colour and no
/// coverage — so a picture scaled smaller than its frame arrives surrounded by
/// transparency rather than by an edge pixel smeared outwards.
///
/// # Errors
///
/// [`RenderStatus::AlphaRequired`] for a format with no alpha channel,
/// [`RenderStatus::WrongAlphaState`] for a frame that is not premultiplied,
/// [`RenderStatus::OutsideDomain`] for a picture past [`MAX_EXTENT`],
/// [`RenderStatus::OutOfMemory`], and [`RenderStatus::Time`] wrapping an
/// overflow.
pub fn resample(
    frame: &Frame,
    target: FrameDescription,
    mapping: Mapping,
    filter: Filter,
) -> Result<Frame> {
    let described = *frame.description();
    if described.format() != PixelFormat::Rgba8 || target.format() != PixelFormat::Rgba8 {
        return Err(RenderStatus::AlphaRequired);
    }
    if described.alpha() != Some(AlphaState::Premultiplied)
        || target.alpha() != Some(AlphaState::Premultiplied)
    {
        return Err(RenderStatus::WrongAlphaState);
    }
    if described.colour() != target.colour() {
        // Resampling is not a colour conversion, and doing both at once is how
        // a picture ends up converted twice or not at all (R-1.3).
        return Err(RenderStatus::NotComposable);
    }

    let source = Picture::new(frame)?;
    let width =
        usize::try_from(target.geometry().width()).map_err(|_| RenderStatus::OutsideDomain)?;
    let height =
        usize::try_from(target.geometry().height()).map_err(|_| RenderStatus::OutsideDomain)?;
    if width == 0 || height == 0 || width > MAX_EXTENT || height > MAX_EXTENT {
        return Err(RenderStatus::OutsideDomain);
    }

    let table = TransferTable::build(described.colour())?;
    let mut out = Vec::new();
    out.try_reserve(
        width
            .checked_mul(height)
            .ok_or(RenderStatus::OutsideDomain)?
            .checked_mul(CHANNELS)
            .ok_or(RenderStatus::OutsideDomain)?,
    )
    .map_err(|_| RenderStatus::OutOfMemory)?;
    for y in 0..height {
        for x in 0..width {
            let row = i64::try_from(y).map_err(|_| RenderStatus::OutsideDomain)?;
            let column = i64::try_from(x).map_err(|_| RenderStatus::OutsideDomain)?;
            let pixel = match filter {
                Filter::Area => area_at(&source, &table, mapping, column, row)?,
                Filter::Bilinear => bilinear_at(&source, &table, mapping, column, row)?,
            };
            out.extend_from_slice(&pixel);
        }
    }
    Ok(Frame::from_packed(target, &out)?)
}

/// A source frame's samples, with its size.
struct Picture {
    packed: Vec<u8>,
    width: i64,
    height: i64,
}

impl Picture {
    fn new(frame: &Frame) -> Result<Self> {
        let geometry = frame.description().geometry();
        Ok(Self {
            packed: frame.to_packed()?,
            width: i64::from(geometry.width()),
            height: i64::from(geometry.height()),
        })
    }

    /// One pixel, or nothing outside the picture.
    ///
    /// Nothing rather than the nearest edge pixel: a source that ran off its
    /// own edge would smear its last column outwards forever, and a picture
    /// scaled smaller than its frame would arrive with a streak instead of a
    /// border.
    fn at(&self, x: i64, y: i64) -> Option<[u8; CHANNELS]> {
        if x < 0 || y < 0 || x >= self.width || y >= self.height {
            return None;
        }
        let index = usize::try_from(y * self.width + x).ok()? * CHANNELS;
        let held = self.packed.get(index..index + CHANNELS)?;
        Some([held[0], held[1], held[2], held[3]])
    }

    /// Whether a position lies inside the picture's extent.
    ///
    /// The extent is `[0, width) x [0, height)` in pixel coordinates, which is
    /// the region the picture covers — a different question from which sample
    /// is nearest, and the one that decides whether there is a picture here at
    /// all.
    fn holds(&self, x: Rational, y: Rational) -> Result<bool> {
        let inside = |value: Rational, limit: i64| -> Result<bool> {
            if value.numerator() < 0 {
                return Ok(false);
            }
            Ok(!value
                .checked_sub(Rational::from_integer(limit))
                .map_err(RenderStatus::Time)?
                .is_positive())
        };
        Ok(inside(x, self.width)? && inside(y, self.height)?)
    }

    /// One pixel, with the index brought back to the nearest that exists.
    ///
    /// Only for a position already known to be inside the extent: past the
    /// last sample's centre the best estimate of the signal is the last
    /// sample, and repeating it is what every reconstruction does at a
    /// boundary.
    fn clamped(&self, x: i64, y: i64) -> [u8; CHANNELS] {
        let x = x.clamp(0, self.width.saturating_sub(1));
        let y = y.clamp(0, self.height.saturating_sub(1));
        self.at(x, y).unwrap_or([0; CHANNELS])
    }
}

/// The exact area-weighted mean of the source under one destination pixel.
fn area_at(
    source: &Picture,
    table: &TransferTable,
    mapping: Mapping,
    x: i64,
    y: i64,
) -> Result<[u8; CHANNELS]> {
    // The destination pixel square, taken back into the source. An affine map
    // sends a square to a parallelogram, so this is exact and has four
    // corners however the picture is turned.
    let mut preimage = Vec::new();
    preimage
        .try_reserve(4)
        .map_err(|_| RenderStatus::OutOfMemory)?;
    for (dx, dy) in [(0, 0), (1, 0), (1, 1), (0, 1)] {
        preimage.push(mapping.source_of(
            Rational::from_integer(x + dx),
            Rational::from_integer(y + dy),
        )?);
    }

    let (left, top, right, bottom) = bounds(&preimage)?;
    let mut light = [Fixed::ZERO; CHANNELS];
    for row in top..=bottom {
        for column in left..=right {
            let Some(pixel) = source.at(column, row) else {
                continue;
            };
            let share = overlap(&preimage, column, row)?;
            if share.is_zero() {
                continue;
            }
            let weight = Fixed::from_rational(
                share
                    .checked_div(mapping.footprint)
                    .map_err(RenderStatus::Time)?,
            )?;
            for channel in 0..CHANNELS {
                let value = if channel == ALPHA {
                    coverage_of(pixel[ALPHA])?
                } else {
                    table.decode(pixel[channel])
                };
                light[channel] = light[channel]
                    .checked_add(value.checked_mul(weight)?)
                    .map_err(RenderStatus::Time)?;
            }
        }
    }
    finish(table, light)
}

/// Four samples around where a destination pixel's centre lands.
fn bilinear_at(
    source: &Picture,
    table: &TransferTable,
    mapping: Mapping,
    x: i64,
    y: i64,
) -> Result<[u8; CHANNELS]> {
    // The centre, because a pixel's value belongs at its middle rather than at
    // its corner. Sampling at the corner shifts the whole picture half a pixel
    // up and left, which is the single most common resampling bug and is
    // invisible until two versions of the same shot are compared.
    let half = Rational::new(1, 2).map_err(RenderStatus::Time)?;
    let centre = mapping.source_of(
        Rational::from_integer(x)
            .checked_add(half)
            .map_err(RenderStatus::Time)?,
        Rational::from_integer(y)
            .checked_add(half)
            .map_err(RenderStatus::Time)?,
    )?;
    // And the samples themselves sit at pixel centres, so the grid this
    // interpolates on is offset by half a pixel from the pixel grid.
    let across = centre.0.checked_sub(half).map_err(RenderStatus::Time)?;
    let down = centre.1.checked_sub(half).map_err(RenderStatus::Time)?;
    let left = across.floor().map_err(RenderStatus::Time)?;
    let top = down.floor().map_err(RenderStatus::Time)?;
    let fraction_x = across
        .checked_sub(Rational::from_integer(left))
        .map_err(RenderStatus::Time)?;
    let fraction_y = down
        .checked_sub(Rational::from_integer(top))
        .map_err(RenderStatus::Time)?;

    // Outside the picture's *extent* is nothing at all. Inside it but beyond
    // the last sample's centre is a different thing entirely, and conflating
    // the two is what made the last column of an enlarged picture fade out:
    // the samples sit at pixel centres, so the outer half-pixel of a picture
    // lies beyond every sample and has no second one to interpolate towards.
    // There the reconstruction clamps to the edge sample, which is the best
    // estimate of a signal past its last measurement.
    if !source.holds(centre.0, centre.1)? {
        return Ok([0; CHANNELS]);
    }

    let mut light = [Fixed::ZERO; CHANNELS];
    for (dx, dy) in [(0_i64, 0_i64), (1, 0), (0, 1), (1, 1)] {
        let pixel = source.clamped(left + dx, top + dy);
        let weight_x = if dx == 0 {
            Rational::ONE
                .checked_sub(fraction_x)
                .map_err(RenderStatus::Time)?
        } else {
            fraction_x
        };
        let weight_y = if dy == 0 {
            Rational::ONE
                .checked_sub(fraction_y)
                .map_err(RenderStatus::Time)?
        } else {
            fraction_y
        };
        let weight =
            Fixed::from_rational(weight_x.checked_mul(weight_y).map_err(RenderStatus::Time)?)?;
        for channel in 0..CHANNELS {
            let value = if channel == ALPHA {
                coverage_of(pixel[ALPHA])?
            } else {
                table.decode(pixel[channel])
            };
            light[channel] = light[channel]
                .checked_add(value.checked_mul(weight)?)
                .map_err(RenderStatus::Time)?;
        }
    }
    finish(table, light)
}

/// Coverage as a fraction of one.
///
/// Alpha is not light and never passes through a transfer function: it is a
/// fraction of a pixel's area, so it is linear by definition.
fn coverage_of(code: u8) -> Result<Fixed> {
    Fixed::from_rational(Rational::new(i64::from(code), 255).map_err(RenderStatus::Time)?)
        .map_err(RenderStatus::Time)
}

/// Encode an accumulated pixel back to bytes.
fn finish(table: &TransferTable, light: [Fixed; CHANNELS]) -> Result<[u8; CHANNELS]> {
    let mut out = [0_u8; CHANNELS];
    for channel in 0..ALPHA {
        out[channel] = table.encode(clamp(light[channel]));
    }
    // The same rounding the compositor uses for coverage, stated in one place
    // rather than left to whatever the multiplication happened to do.
    let scaled = clamp(light[ALPHA]).checked_mul(Fixed::from_integer(255)?)?;
    let half = 1_i64 << (FRACTION_BITS - 1);
    let rounded = scaled.raw().saturating_add(half) >> FRACTION_BITS;
    out[ALPHA] = u8::try_from(rounded.clamp(0, 255)).map_err(|_| RenderStatus::OutsideDomain)?;
    Ok(out)
}

/// A value brought back inside nought to one.
fn clamp(value: Fixed) -> Fixed {
    if value.raw() < 0 {
        return Fixed::ZERO;
    }
    if value.raw() > Fixed::ONE.raw() {
        return Fixed::ONE;
    }
    value
}

/// The whole-pixel box a polygon lies inside.
fn bounds(polygon: &[(Rational, Rational)]) -> Result<(i64, i64, i64, i64)> {
    let mut left = polygon[0].0;
    let mut right = polygon[0].0;
    let mut top = polygon[0].1;
    let mut bottom = polygon[0].1;
    for point in polygon {
        if point
            .0
            .checked_sub(left)
            .map_err(RenderStatus::Time)?
            .numerator()
            < 0
        {
            left = point.0;
        }
        if point
            .0
            .checked_sub(right)
            .map_err(RenderStatus::Time)?
            .is_positive()
        {
            right = point.0;
        }
        if point
            .1
            .checked_sub(top)
            .map_err(RenderStatus::Time)?
            .numerator()
            < 0
        {
            top = point.1;
        }
        if point
            .1
            .checked_sub(bottom)
            .map_err(RenderStatus::Time)?
            .is_positive()
        {
            bottom = point.1;
        }
    }
    Ok((
        left.floor().map_err(RenderStatus::Time)?,
        top.floor().map_err(RenderStatus::Time)?,
        right.floor().map_err(RenderStatus::Time)?,
        bottom.floor().map_err(RenderStatus::Time)?,
    ))
}

/// The exact area of one source pixel inside a preimage parallelogram.
///
/// Both are convex, so this is the shape rasteriser's own clip — and the
/// preimage's winding is whatever the map made of it, which under a mirror is
/// the other one. `shape::convex` measures it rather than being told, which is
/// exactly why a mask and a resampled pixel can share it.
fn overlap(preimage: &[(Rational, Rational)], x: i64, y: i64) -> Result<Rational> {
    match crate::shape::convex(preimage) {
        Ok(shape) => shape.coverage(x, y),
        // A preimage with no area is not an error here: an affine map cannot
        // produce one, because a singular map was refused when the mapping was
        // built. It is unreachable rather than impossible, so it answers
        // nought rather than asserting.
        Err(RenderStatus::DegenerateShape) => Ok(Rational::ZERO),
        Err(other) => Err(other),
    }
}
