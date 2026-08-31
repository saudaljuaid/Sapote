// SPDX-License-Identifier: GPL-3.0-only
//! Clips and gaps stored on a track.
//!
//! Explicit gaps keep the track fully tiled without overlaps or missing spans.

use sapstudio_core::{Duration, Instant, Rational, Timebase};

use crate::media::MediaId;
use crate::status::{ModelStatus, Result};

/// A range of a media asset placed on a track.
///
/// Source positions use the track timebase. The media and sequence timebases
/// must match (R-1.2).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Clip {
    media: MediaId,
    source_start: i64,
    duration: Duration,
    grade: Option<crate::media::Digest>,
    mask: Option<crate::mask::Mask>,
    transform: Option<crate::transform::Transform>,
    motion: Option<crate::transform::Motion>,
    /// Fade lengths in ticks of the clip timebase.
    fade_in: i64,
    fade_out: i64,
    playback: Playback,
    /// Animation applied to the clip mask.
    mask_motion: Option<crate::transform::Motion>,
    /// Opacity curve in clip-local time.
    opacity: Option<crate::curve::Curve>,
    /// Grade blend curve in clip-local time.
    grade_strength: Option<crate::curve::Curve>,
}

/// How a clip advances through source media.
///
/// Frozen playback consumes one source frame, while timed playback consumes a
/// span determined by its speed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Playback {
    /// An exact, nonzero fraction of real time.
    At(Rational),
    /// Held on one frame of media, however long the clip runs.
    Frozen,
}

impl Playback {
    /// Apply this playback mode to a clip.
    ///
    /// # Errors
    ///
    /// Returns any error from [`Clip::with_speed`]. Frozen playback is
    /// infallible.
    pub fn applied_to(self, clip: &Clip) -> Result<Clip> {
        match self {
            Self::At(speed) => clip.with_speed(speed),
            Self::Frozen => Ok(clip.frozen()),
        }
    }
}

impl Clip {
    /// Refer to `duration` of `media`, beginning `source_start` ticks in.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::EmptyItem`] for a zero length, and
    /// [`ModelStatus::SourceBeforeStart`] for a negative source position.
    pub fn new(media: MediaId, source_start: i64, duration: Duration) -> Result<Self> {
        if duration.is_zero() {
            return Err(ModelStatus::EmptyItem);
        }
        if source_start < 0 {
            return Err(ModelStatus::SourceBeforeStart);
        }
        Ok(Self {
            media,
            source_start,
            duration,
            grade: None,
            mask: None,
            transform: None,
            motion: None,
            fade_in: 0,
            fade_out: 0,
            playback: Playback::At(Rational::ONE),
            mask_motion: None,
            opacity: None,
            grade_strength: None,
        })
    }

    /// The media this cuts from.
    #[must_use]
    pub const fn media(&self) -> MediaId {
        self.media
    }

    /// The first tick of the media this uses.
    #[must_use]
    pub const fn source_start(&self) -> i64 {
        self.source_start
    }

    /// How much of the media this uses.
    #[must_use]
    pub const fn duration(&self) -> Duration {
        self.duration
    }

    /// How this clip consumes its media: at a speed, or held on one frame.
    #[must_use]
    pub const fn playback(&self) -> Playback {
        self.playback
    }

    /// Return the exact playback speed, or [`None`] for a frozen clip.
    ///
    /// One is real time, values between zero and one are slow motion, values
    /// above one are fast motion, and negative values play backwards.
    #[must_use]
    pub const fn speed(&self) -> Option<Rational> {
        match self.playback {
            Playback::At(speed) => Some(speed),
            Playback::Frozen => None,
        }
    }

    /// Whether this clip plays forwards in real time.
    #[must_use]
    pub fn is_real_time(&self) -> bool {
        self.playback == Playback::At(Rational::ONE)
    }

    /// Whether this clip is held on one frame.
    #[must_use]
    pub fn is_frozen(&self) -> bool {
        self.playback == Playback::Frozen
    }

    /// The animation of this clip's mask, if there is one.
    #[must_use]
    pub const fn mask_motion(&self) -> Option<&crate::transform::Motion> {
        self.mask_motion.as_ref()
    }

    /// Return this clip with the supplied mask animation.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::NoMaskToAnimate`] if the clip has no mask.
    pub fn with_mask_motion(&self, motion: Option<crate::transform::Motion>) -> Result<Self> {
        if motion.is_some() && self.mask.is_none() {
            return Err(ModelStatus::NoMaskToAnimate);
        }
        Ok(Self {
            mask_motion: motion,
            ..self.clone()
        })
    }

    /// Resolve this clip's mask at `offset` ticks.
    ///
    /// # Errors
    ///
    /// Returns any error from the mask animation.
    pub fn mask_at(&self, offset: i64) -> Result<Option<crate::mask::Mask>> {
        let Some(shape) = &self.mask else {
            return Ok(None);
        };
        let Some(motion) = &self.mask_motion else {
            return Ok(Some(shape.clone()));
        };
        let (scale, across, down, turn) = motion.at(sapstudio_core::Instant::new(
            offset,
            self.duration.timebase(),
        ))?;
        Ok(Some(shape.moved_by(scale, across, down, turn)?))
    }

    /// The curve driving this clip's opacity, if there is one.
    #[must_use]
    pub const fn opacity(&self) -> Option<&crate::curve::Curve> {
        self.opacity.as_ref()
    }

    /// Return this clip with the supplied opacity curve.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::WrongTimebase`] if the curve and clip use different
    /// timebases.
    pub fn with_opacity(&self, opacity: Option<crate::curve::Curve>) -> Result<Self> {
        if let Some(curve) = &opacity {
            if curve.timebase() != self.duration.timebase() {
                return Err(ModelStatus::WrongTimebase);
            }
        }
        Ok(Self {
            opacity,
            ..self.clone()
        })
    }

    /// Resolve opacity at `offset` ticks, clamped to zero through one.
    ///
    /// A clip without an opacity curve returns one.
    ///
    /// # Errors
    ///
    /// Returns any error from the curve.
    pub fn opacity_at(&self, offset: i64) -> Result<Rational> {
        let Some(curve) = &self.opacity else {
            return Ok(Rational::ONE);
        };
        let held = curve.value_at(sapstudio_core::Instant::new(
            offset,
            self.duration.timebase(),
        ))?;
        Ok(held.clamp(Rational::ZERO, Rational::ONE))
    }

    /// The curve bringing this clip's grade on, if there is one.
    #[must_use]
    pub const fn grade_strength(&self) -> Option<&crate::curve::Curve> {
        self.grade_strength.as_ref()
    }

    /// Return this clip with the supplied grade-strength curve.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::NoGradeToAnimate`] if the clip has no grade, or
    /// [`ModelStatus::WrongTimebase`] if the curve and clip use different
    /// timebases.
    pub fn with_grade_strength(&self, strength: Option<crate::curve::Curve>) -> Result<Self> {
        if let Some(curve) = &strength {
            if self.grade.is_none() {
                return Err(ModelStatus::NoGradeToAnimate);
            }
            if curve.timebase() != self.duration.timebase() {
                return Err(ModelStatus::WrongTimebase);
            }
        }
        Ok(Self {
            grade_strength: strength,
            ..self.clone()
        })
    }

    /// Resolve grade strength at `offset` ticks, clamped to zero through one.
    ///
    /// A clip without a strength curve returns one.
    ///
    /// # Errors
    ///
    /// Returns any error from the curve.
    pub fn grade_strength_at(&self, offset: i64) -> Result<Rational> {
        let Some(curve) = &self.grade_strength else {
            return Ok(Rational::ONE);
        };
        let held = curve.value_at(sapstudio_core::Instant::new(
            offset,
            self.duration.timebase(),
        ))?;
        Ok(held.clamp(Rational::ZERO, Rational::ONE))
    }

    /// Return this clip frozen on its source in-point.
    #[must_use]
    pub fn frozen(&self) -> Self {
        Self {
            playback: Playback::Frozen,
            ..self.clone()
        }
    }

    /// The same clip played at a different speed.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::SpeedNotUsable`] for zero speed,
    /// [`ModelStatus::SourceBeforeStart`] if reverse playback crosses the start
    /// of the asset, or [`ModelStatus::Time`] on overflow.
    pub fn with_speed(&self, speed: Rational) -> Result<Self> {
        if speed.is_zero() {
            return Err(ModelStatus::SpeedNotUsable);
        }
        let retimed = Self {
            playback: Playback::At(speed),
            ..self.clone()
        };
        // The clip can validate the lower source bound. Project validation
        // checks the upper bound against the media asset.
        if retimed.source_span()?.0 < 0 {
            return Err(ModelStatus::SourceBeforeStart);
        }
        Ok(retimed)
    }

    /// Return the source tick shown at `offset` ticks into this clip.
    ///
    /// The mapping floors `offset * abs(speed)` before applying the playback
    /// direction, which keeps forward and reverse cadence symmetric.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping an overflow.
    pub fn source_at(&self, offset: i64) -> Result<i64> {
        let Playback::At(speed) = self.playback else {
            // Frozen playback always uses the source in-point.
            return Ok(self.source_start);
        };
        let forwards = speed.is_positive();
        let size = if forwards {
            speed
        } else {
            Rational::ZERO
                .checked_sub(speed)
                .map_err(ModelStatus::from)?
        };
        let along = floor_of(
            size.checked_mul(Rational::new(offset, 1)?)
                .map_err(ModelStatus::from)?,
        );
        if forwards {
            self.source_start.checked_add(along)
        } else {
            self.source_start.checked_sub(along)
        }
        .ok_or(ModelStatus::Time(sapstudio_core::CoreStatus::Overflow))
    }

    /// Return the first source tick past this clip in playback order.
    ///
    /// Use [`Clip::source_span`] for ordered lower and upper bounds.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping an overflow.
    pub fn source_end(&self) -> Result<i64> {
        if self.is_frozen() {
            // Frozen playback consumes exactly one source frame.
            return self
                .source_start
                .checked_add(1)
                .ok_or(ModelStatus::Time(sapstudio_core::CoreStatus::Overflow));
        }
        self.source_at(self.duration.ticks())
    }

    /// Return the inclusive lower and upper source ticks read by this clip.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping an overflow.
    pub fn source_span(&self) -> Result<(i64, i64)> {
        // Clips are non-empty, so a final timeline tick always exists.
        let last = self.source_at(self.duration.ticks().saturating_sub(1))?;
        Ok(if last < self.source_start {
            (last, self.source_start)
        } else {
            (self.source_start, last)
        })
    }

    /// The source position as an instant in the track's timebase.
    #[must_use]
    pub const fn source_instant(&self, timebase: Timebase) -> Instant {
        Instant::new(self.source_start, timebase)
    }

    /// Return the digest of the look applied to this clip.
    #[must_use]
    pub const fn grade(&self) -> Option<crate::media::Digest> {
        self.grade
    }

    /// The mask on this clip, if it has one.
    #[must_use]
    pub const fn mask(&self) -> Option<&crate::mask::Mask> {
        self.mask.as_ref()
    }

    /// Where this clip sits in the frame, if it has been moved.
    #[must_use]
    pub const fn transform(&self) -> Option<crate::transform::Transform> {
        self.transform
    }

    /// How long this clip takes to come up from nothing.
    #[must_use]
    pub fn fade_in(&self) -> Duration {
        self.counted(self.fade_in)
    }

    /// And to go back down to it.
    #[must_use]
    pub fn fade_out(&self) -> Duration {
        self.counted(self.fade_out)
    }

    /// Convert a stored fade length to the clip timebase.
    fn counted(&self, ticks: i64) -> Duration {
        let timebase = self.duration.timebase();
        Duration::new(ticks, timebase).unwrap_or_else(|_| Duration::zero(timebase))
    }

    /// Return this clip with the supplied fade-in and fade-out lengths.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] on a timebase mismatch, or
    /// [`ModelStatus::FadesLongerThanClip`] if the combined fades exceed the
    /// clip length.
    pub fn with_fades(&self, fade_in: Duration, fade_out: Duration) -> Result<Self> {
        let timebase = self.duration.timebase();
        if fade_in.timebase() != timebase || fade_out.timebase() != timebase {
            return Err(sapstudio_core::CoreStatus::TimebaseMismatch.into());
        }
        if fade_in.checked_add(fade_out)?.compare(self.duration)? == core::cmp::Ordering::Greater {
            return Err(ModelStatus::FadesLongerThanClip);
        }
        Ok(Self {
            fade_in: fade_in.ticks(),
            fade_out: fade_out.ticks(),
            ..self.clone()
        })
    }

    /// Return the clip fade level at `offset` ticks.
    ///
    /// Unfaded clips return one. Off-clip offsets return zero.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] on arithmetic overflow.
    pub fn fade_at(&self, offset: i64) -> Result<Rational> {
        let ticks = self.duration.ticks();
        let rising = ramp(offset, self.fade_in)?;
        let falling = ramp(
            ticks
                .checked_sub(1)
                .and_then(|last| last.checked_sub(offset))
                .ok_or(ModelStatus::Time(sapstudio_core::CoreStatus::Overflow))?,
            self.fade_out,
        )?;
        Ok(if rising < falling { rising } else { falling })
    }

    /// Return the framing animation in clip-local time.
    #[must_use]
    pub const fn motion(&self) -> Option<&crate::transform::Motion> {
        self.motion.as_ref()
    }

    /// Return this clip with the supplied framing animation.
    #[must_use]
    pub fn with_motion(&self, motion: Option<crate::transform::Motion>) -> Self {
        Self {
            motion,
            ..self.clone()
        }
    }

    /// The same clip moved, or left where it was.
    #[must_use]
    pub fn with_transform(&self, transform: Option<crate::transform::Transform>) -> Self {
        Self {
            transform,
            ..self.clone()
        }
    }

    /// The same clip with a mask, or with none.
    #[must_use]
    pub fn with_mask(&self, mask: Option<crate::mask::Mask>) -> Self {
        Self {
            mask,
            ..self.clone()
        }
    }

    /// The same clip with a look on it, or with none.
    #[must_use]
    pub fn with_grade(&self, grade: Option<crate::media::Digest>) -> Self {
        Self {
            grade,
            ..self.clone()
        }
    }

    /// Return this clip with a different source in-point.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::SourceBeforeStart`] for a negative source position.
    pub fn with_source(&self, source_start: i64) -> Result<Self> {
        if source_start < 0 {
            return Err(ModelStatus::SourceBeforeStart);
        }
        Ok(Self {
            source_start,
            ..self.clone()
        })
    }

    /// The same clip cut to a different length, keeping everything else.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::EmptyItem`] for a zero length.
    pub fn with_duration(&self, duration: Duration) -> Result<Self> {
        if duration.is_zero() {
            return Err(ModelStatus::EmptyItem);
        }
        if self
            .fade_in
            .checked_add(self.fade_out)
            .is_none_or(|together| together > duration.ticks())
        {
            // Trimming never clamps or removes existing fades.
            return Err(ModelStatus::FadesLongerThanClip);
        }
        Ok(Self {
            duration,
            ..self.clone()
        })
    }
}

/// Whether `second` continues `first` in source media.
///
/// Adjacent frozen clips continue when they hold the same source frame.
fn continues_source(first: &Clip, second: &Clip) -> bool {
    if first.is_frozen() {
        return first.source_start == second.source_start;
    }
    first
        .source_end()
        .is_ok_and(|end| end == second.source_start)
}

/// Return the mathematical floor of a rational value.
///
/// Euclidean division preserves floor behavior for negative playback offsets.
fn floor_of(value: Rational) -> i64 {
    value.numerator().div_euclid(value.denominator())
}

/// Return a zero-to-one ramp clamped at both ends.
fn ramp(along: i64, over: i64) -> Result<Rational> {
    if over == 0 {
        return Ok(Rational::ONE);
    }
    if along <= 0 {
        return Ok(Rational::ZERO);
    }
    if along >= over {
        return Ok(Rational::ONE);
    }
    Rational::new(along, over).map_err(ModelStatus::from)
}

/// Whether `second` continues `first`'s rebased framing animation.
fn continues_motion(first: &Clip, second: &Clip) -> bool {
    match (&first.motion, &second.motion) {
        (None, None) => true,
        (Some(held), Some(later)) => held
            .shifted(-first.duration.ticks())
            .is_ok_and(|rebased| &rebased == later),
        _ => false,
    }
}

/// Whether two clips' mask animations line up across a cut.
fn continues_mask_motion(first: &Clip, second: &Clip) -> bool {
    match (&first.mask_motion, &second.mask_motion) {
        (None, None) => true,
        (Some(held), Some(later)) => held
            .shifted(-first.duration.ticks())
            .is_ok_and(|rebased| &rebased == later),
        _ => false,
    }
}

/// Whether two clips' grade-strength curves line up across a cut.
fn continues_grade_strength(first: &Clip, second: &Clip) -> bool {
    match (&first.grade_strength, &second.grade_strength) {
        (None, None) => true,
        (Some(held), Some(later)) => held
            .shifted(-first.duration.ticks())
            .is_ok_and(|rebased| &rebased == later),
        _ => false,
    }
}

/// Whether two clips' opacity curves line up across a cut.
fn continues_opacity(first: &Clip, second: &Clip) -> bool {
    match (&first.opacity, &second.opacity) {
        (None, None) => true,
        (Some(held), Some(later)) => held
            .shifted(-first.duration.ticks())
            .is_ok_and(|rebased| &rebased == later),
        _ => false,
    }
}

/// One entry on a track.
///
/// `Clip` remains inline because boxing would violate the fallible-allocation
/// rules (R-5.1, R-5.2). Tracks are clip-heavy, and `tests/size.rs` pins the
/// resulting enum size.
#[expect(
    clippy::large_enum_variant,
    reason = "the remedy is boxing, and R-5.2 forbids an infallible allocation"
)]
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Item {
    /// Material.
    Clip(Clip),
    /// Deliberate absence of material, with a length.
    Gap(Duration),
}

impl Item {
    /// A gap of a given length.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::EmptyItem`] for a zero length.
    pub fn gap(duration: Duration) -> Result<Self> {
        if duration.is_zero() {
            return Err(ModelStatus::EmptyItem);
        }
        Ok(Self::Gap(duration))
    }

    /// How long this item occupies its track.
    #[must_use]
    pub const fn duration(&self) -> Duration {
        match self {
            Self::Clip(clip) => clip.duration(),
            Self::Gap(duration) => *duration,
        }
    }

    /// The timebase this item is counted in.
    #[must_use]
    pub const fn timebase(&self) -> Timebase {
        self.duration().timebase()
    }

    /// This item with a different length, keeping its source position.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::EmptyItem`] for a zero length, or a timebase mismatch.
    pub fn with_duration(&self, duration: Duration) -> Result<Self> {
        if duration.timebase() != self.timebase() {
            return Err(sapstudio_core::CoreStatus::TimebaseMismatch.into());
        }
        if duration.is_zero() {
            return Err(ModelStatus::EmptyItem);
        }
        Ok(match self {
            // Trimming preserves all clip metadata.
            Self::Clip(clip) => Self::Clip(clip.with_duration(duration)?),
            Self::Gap(_) => Self::Gap(duration),
        })
    }

    /// This item cut in two at `offset` ticks from its start.
    ///
    /// The two pieces are contiguous in their source, so joining them is the
    /// exact inverse of this operation.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::SplitOutsideItem`] if the offset would leave either
    /// piece empty.
    pub fn split(&self, offset: i64) -> Result<(Self, Self)> {
        if offset <= 0 || offset >= self.duration().ticks() {
            return Err(ModelStatus::SplitOutsideItem);
        }
        let timebase = self.timebase();
        let head_length = Duration::new(offset, timebase).map_err(ModelStatus::from)?;
        let tail_length = self.duration().checked_sub(head_length)?;
        let head = self.with_duration(head_length)?;
        let tail = match self {
            Self::Clip(clip) => {
                // Map through playback speed so the tail starts at the source
                // frame immediately after the head.
                let source_start = clip.source_at(offset)?;
                // Rebase clip-local animation lanes onto the new tail.
                let rebased = match &clip.motion {
                    None => None,
                    Some(motion) => Some(motion.shifted(-offset)?),
                };
                let faded = match &clip.opacity {
                    None => None,
                    Some(curve) => Some(curve.shifted(-offset)?),
                };
                let reshaped = match &clip.mask_motion {
                    None => None,
                    Some(motion) => Some(motion.shifted(-offset)?),
                };
                let brought = match &clip.grade_strength {
                    None => None,
                    Some(curve) => Some(curve.shifted(-offset)?),
                };
                Self::Clip(
                    clip.with_source(source_start)?
                        .with_duration(tail_length)?
                        .with_motion(rebased)
                        .with_mask_motion(reshaped)?
                        .with_opacity(faded)?
                        .with_grade_strength(brought)?,
                )
            }
            Self::Gap(_) => Self::Gap(tail_length),
        };
        Ok((head, tail))
    }

    /// Whether `later` continues this item without a break in its source.
    #[must_use]
    pub fn continues_into(&self, later: &Self) -> bool {
        match (self, later) {
            (Self::Gap(_), Self::Gap(_)) => self.timebase() == later.timebase(),
            (Self::Clip(first), Self::Clip(second)) => {
                first.media == second.media
                    && first.duration.timebase() == second.duration.timebase()
                    // Joining must preserve the complete clip treatment.
                    && first.grade == second.grade
                    && first.playback == second.playback
                    && continues_motion(first, second)
                    && continues_mask_motion(first, second)
                    && continues_opacity(first, second)
                    && continues_grade_strength(first, second)
                    && continues_source(first, second)
            }
            _ => false,
        }
    }

    /// This item and the one that continues it, as one item.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::ItemsNotContiguous`] if the two are not adjacent in
    /// their source, which is what makes join the exact inverse of split.
    pub fn join(&self, later: &Self) -> Result<Self> {
        if !self.continues_into(later) {
            return Err(ModelStatus::ItemsNotContiguous);
        }
        let duration = self.duration().checked_add(later.duration())?;
        self.with_duration(duration)
    }
}
