// SPDX-License-Identifier: GPL-3.0-only
//! Timeline markers.
//!
//! A marker stores text at an absolute programme instant. It is not a track
//! item, does not render, and does not move with ripple edits. Each sequence
//! allows at most one marker per instant, matching keyframe collision rules.

use alloc::string::String;

use sapstudio_core::Instant;

use crate::status::{ModelStatus, Result};

/// How many characters a marker may carry.
///
/// Shared with the title line limit so user-entered text has one bound.
pub const MAX_MARKER_TEXT: usize = 128;

/// How many markers one sequence may hold.
///
/// A policy bound. A feature-length programme carries tens of notes; a
/// sequence that reaches this has been generated rather than edited.
pub const MAX_MARKERS_PER_SEQUENCE: usize = 4096;

/// A note at an instant.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Marker {
    at: Instant,
    text: String,
}

impl Marker {
    /// A marker at an instant, saying something.
    ///
    /// Empty text is allowed for an unnamed position marker.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::MarkerTextTooLong`] past [`MAX_MARKER_TEXT`] characters,
    /// counted in characters rather than bytes for the reason a title's are:
    /// a bound in bytes is a bound that means something different in every
    /// language.
    pub fn new(at: Instant, text: String) -> Result<Self> {
        if text.chars().count() > MAX_MARKER_TEXT {
            return Err(ModelStatus::MarkerTextTooLong);
        }
        Ok(Self { at, text })
    }

    /// Where it is.
    #[must_use]
    pub const fn at(&self) -> Instant {
        self.at
    }

    /// What it says.
    #[must_use]
    pub fn text(&self) -> &str {
        &self.text
    }

    /// The same marker, moved.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping a timebase mismatch.
    pub fn moved_to(&self, at: Instant) -> Result<Self> {
        if at.timebase() != self.at.timebase() {
            return Err(sapstudio_core::CoreStatus::TimebaseMismatch.into());
        }
        Ok(Self {
            at,
            text: self.text.clone(),
        })
    }
}
