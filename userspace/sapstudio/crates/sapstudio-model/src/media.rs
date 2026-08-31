// SPDX-License-Identifier: GPL-3.0-only
//! Media the project refers to but never modifies.

use alloc::vec::Vec;

use sapstudio_core::{Duration, Id, Timebase};

/// A content digest: what a media asset *is*, independent of where it lives.
///
/// The project references media by this and relinks by this (R-9.5), so moving
/// a file breaks nothing and replacing a file's contents is detected rather
/// than silently used. It is [`sapstudio_core::Digest`]: one definition of
/// content identity for the whole application, so that a media digest, a cache
/// key, and a saved file's digest are all the same kind of thing.
pub use sapstudio_core::Digest;

/// A media asset the project can cut from.
///
/// It is described, not owned: SapStudio never modifies source media (R-9.6),
/// so this record is read-only for the whole life of the project.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MediaAsset {
    digest: Digest,
    timebase: Timebase,
    duration: Duration,
    location: Option<Location>,
    source: MediaSource,
}

/// Where an asset's frames come from.
///
/// The only thing that separates a title from a recording. Everything else a
/// project does to media — cutting, trimming, dissolving, grading, masking,
/// moving — does not care and must not have to.
///
/// The planner resolves the source before building a graph so the cache key
/// always describes the rendered content.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum MediaSource {
    /// Bytes somewhere, named by the digest of their content.
    Recorded,
    /// A picture the program makes out of words.
    Title(crate::title::Title),
}

/// How many bytes a location hint may be.
///
/// Two hundred and fifty-five: longer than any path Sapote's read-only FAT16
/// can express, and a bound a hostile project file cannot talk its way past
/// (R-11.2).
pub const MAX_LOCATION_BYTES: usize = 255;

/// Where the bytes were last seen.
///
/// A **hint**, and the word is load-bearing. The digest is what the media
/// *is*; this is only where somebody found it, and the model never resolves
/// it, never opens it, and never compares two assets by it. A project whose
/// media has moved is a project with a stale hint and correct identity, which
/// is the right way round: the hint can be repaired by looking, and identity
/// cannot be recovered by guessing.
///
/// It is bytes rather than a string because a path is whatever the platform
/// says it is, and Sapote's is not decided yet. Refusing to interpret it here
/// is what keeps this crate free of the operating system.
#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Location {
    bytes: Vec<u8>,
}

impl Location {
    /// A hint from its bytes.
    ///
    /// # Errors
    ///
    /// [`crate::ModelStatus::EmptyLocation`] for no bytes at all — a hint that
    /// says nothing is worse than none, because it looks like an answer — and
    /// [`crate::ModelStatus::CapacityExhausted`] past
    /// [`MAX_LOCATION_BYTES`].
    pub fn new(bytes: &[u8]) -> crate::Result<Self> {
        if bytes.is_empty() {
            return Err(crate::ModelStatus::EmptyLocation);
        }
        if bytes.len() > MAX_LOCATION_BYTES {
            return Err(crate::ModelStatus::CapacityExhausted);
        }
        let mut held = Vec::new();
        held.try_reserve(bytes.len())
            .map_err(|_| crate::ModelStatus::OutOfMemory)?;
        held.extend_from_slice(bytes);
        Ok(Self { bytes: held })
    }

    /// The bytes, uninterpreted.
    #[must_use]
    pub fn bytes(&self) -> &[u8] {
        &self.bytes
    }
}

impl MediaAsset {
    /// Describe an asset.
    ///
    /// # Errors
    ///
    /// [`crate::ModelStatus::Time`] wrapping
    /// [`sapstudio_core::CoreStatus::TimebaseMismatch`] if the duration is not
    /// counted in the asset's own timebase.
    pub fn new(digest: Digest, timebase: Timebase, duration: Duration) -> crate::Result<Self> {
        if duration.timebase() != timebase {
            return Err(sapstudio_core::CoreStatus::TimebaseMismatch.into());
        }
        Ok(Self {
            digest,
            timebase,
            duration,
            location: None,
            source: MediaSource::Recorded,
        })
    }

    /// An asset the program draws rather than reads.
    ///
    /// Its digest is the digest of the title's own description, which is what
    /// content addressing already meant: the same title in two projects is the
    /// same title, two clips of it share a cached frame, and changing a word
    /// makes a *different* asset rather than quietly changing what every clip
    /// of it shows.
    ///
    /// # Errors
    ///
    /// [`crate::ModelStatus::Time`] wrapping a timebase mismatch or an
    /// overflow.
    pub fn titled(
        title: crate::title::Title,
        timebase: Timebase,
        duration: Duration,
    ) -> crate::Result<Self> {
        if duration.timebase() != timebase {
            return Err(sapstudio_core::CoreStatus::TimebaseMismatch.into());
        }
        Ok(Self {
            digest: title.digest()?,
            timebase,
            duration,
            // Nothing to find, so nothing to hint at. A title that carried a
            // location would be inviting somebody to relink it to a file, and
            // the file would be a different asset the moment it was opened.
            location: None,
            source: MediaSource::Title(title),
        })
    }

    /// Where this asset's frames come from.
    #[must_use]
    pub const fn source(&self) -> &MediaSource {
        &self.source
    }

    /// The title this asset draws, if it draws one.
    #[must_use]
    pub const fn title(&self) -> Option<&crate::title::Title> {
        match &self.source {
            MediaSource::Recorded => None,
            MediaSource::Title(title) => Some(title),
        }
    }

    /// What this asset is.
    #[must_use]
    pub const fn digest(&self) -> Digest {
        self.digest
    }

    /// The rate this asset is counted in.
    #[must_use]
    pub const fn timebase(&self) -> Timebase {
        self.timebase
    }

    /// Where the bytes were last seen, if anybody said.
    #[must_use]
    pub const fn location(&self) -> Option<&Location> {
        self.location.as_ref()
    }

    /// The same asset with a hint about where to find it.
    ///
    /// # Errors
    ///
    /// [`crate::ModelStatus::NotRecordedMedia`] for a generated asset, which
    /// has nowhere to be. Accepting the hint and ignoring it would let a
    /// project hold a title that claims to be a file, and the next thing to
    /// read it would be entitled to believe that.
    pub fn with_location(&self, location: Option<Location>) -> crate::Result<Self> {
        if !matches!(self.source, MediaSource::Recorded) {
            return Err(crate::ModelStatus::NotRecordedMedia);
        }
        Ok(Self {
            location,
            ..self.clone()
        })
    }

    /// How long it runs.
    #[must_use]
    pub const fn duration(&self) -> Duration {
        self.duration
    }
}

/// A reference to a media asset in a project's library.
pub type MediaId = Id<MediaAsset>;
