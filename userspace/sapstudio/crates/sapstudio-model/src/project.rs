// SPDX-License-Identifier: GPL-3.0-only
//! The project: media, sequences, and the history over them.
//!
//! This is the single source of truth (R-9.1). Everything else in SapStudio —
//! caches, rendered frames, waveform overviews, interface state — is derived
//! from it and can be thrown away.

use sapstudio_core::Timebase;

use crate::edit::Edit;
use crate::item::Item;
use crate::journal::EditJournal;
use crate::media::{MediaAsset, MediaId};
use crate::sequence::{Sequence, SequenceId};
use crate::status::{ModelStatus, Result};
use crate::store::Store;

/// How many media assets one project may hold.
pub const MAX_MEDIA: usize = 65_536;

/// How many sequences one project may hold.
pub const MAX_SEQUENCES: usize = 1024;

/// A whole editing project.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Project {
    media: Store<MediaAsset>,
    sequences: Store<Sequence>,
    history: EditJournal,
}

impl Default for Project {
    fn default() -> Self {
        Self::new()
    }
}

impl Project {
    /// An empty project.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            media: Store::new(MAX_MEDIA),
            sequences: Store::new(MAX_SEQUENCES),
            history: EditJournal::new(),
        }
    }

    /// The media library.
    #[must_use]
    pub const fn media(&self) -> &Store<MediaAsset> {
        &self.media
    }

    /// The sequences.
    #[must_use]
    pub const fn sequences(&self) -> &Store<Sequence> {
        &self.sequences
    }

    /// The history.
    #[must_use]
    pub const fn history(&self) -> &EditJournal {
        &self.history
    }

    /// Add a media asset to the library, or find the one already there.
    ///
    /// Media is content-addressed, so each digest maps to one asset identifier.
    /// Adding the same content again returns the existing identifier. This keeps
    /// conform imports unambiguous.
    ///
    /// The location hint is deliberately *not* part of the comparison, and the
    /// existing one is kept. The same content found in a second place is the
    /// same content; moving the hint is [`Project::set_media_location`]'s job, and
    /// doing it here would rewrite a project's records as a side effect of
    /// opening a file.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::MediaContradiction`] if an asset with this digest is
    /// already held and describes it differently — the same bytes cannot be
    /// two lengths — and [`ModelStatus::CapacityExhausted`] or
    /// [`ModelStatus::OutOfMemory`].
    pub fn add_media(&mut self, asset: MediaAsset) -> Result<MediaId> {
        if let Some(held) = self.find_media(asset.digest()) {
            let existing = self.media.get(held)?;
            if existing.timebase() != asset.timebase() || existing.duration() != asset.duration() {
                return Err(ModelStatus::MediaContradiction);
            }
            return Ok(held);
        }
        self.media.insert(asset)
    }

    /// Which asset holds this content, if any.
    #[must_use]
    pub fn find_media(&self, digest: crate::media::Digest) -> Option<MediaId> {
        self.media
            .iter()
            .find(|(_, asset)| asset.digest() == digest)
            .map(|(id, _)| id)
    }

    /// Update an asset's location hint and return the previous hint.
    ///
    /// Content identity remains the digest. Location changes are project-level
    /// metadata and are not stored in the sequence edit journal.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping a stale or unknown identifier, and
    /// [`ModelStatus::NotRecordedMedia`] for a title, which has nowhere to be.
    pub fn set_media_location(
        &mut self,
        id: MediaId,
        location: Option<crate::media::Location>,
    ) -> Result<Option<crate::media::Location>> {
        let asset = self.media.get_mut(id)?;
        let previous = asset.location().cloned();
        // Built before the write, so a refusal leaves the library exactly as
        // it was (R-1.4).
        let relinked = asset.with_location(location)?;
        *asset = relinked;
        Ok(previous)
    }

    /// Remove a media asset, if nothing cuts from it.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::MediaInUse`] if any sequence still refers to it, or an
    /// identifier refusal.
    pub fn remove_media(&mut self, id: MediaId) -> Result<MediaAsset> {
        for (_, sequence) in self.sequences.iter() {
            for track in sequence.tracks() {
                for item in track.items() {
                    if matches!(item, Item::Clip(clip) if clip.media() == id) {
                        return Err(ModelStatus::MediaInUse);
                    }
                }
            }
        }
        self.media.remove(id)
    }

    /// Add an empty sequence at a rate.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::CapacityExhausted`] or [`ModelStatus::OutOfMemory`].
    pub fn add_sequence(&mut self, timebase: Timebase) -> Result<SequenceId> {
        self.sequences.insert(Sequence::new(timebase))
    }

    /// Borrow a sequence.
    ///
    /// # Errors
    ///
    /// An identifier refusal.
    pub fn sequence(&self, id: SequenceId) -> Result<&Sequence> {
        self.sequences.get(id)
    }

    /// Apply an edit to a sequence and record it in the project's history.
    ///
    /// Every clip the edit introduces is checked against the library first:
    /// the media must exist, share the sequence's timebase, and contain the
    /// whole source range the clip asks for. A refusal leaves both the
    /// sequence and the history untouched.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::UnknownMedia`], [`ModelStatus::MediaTimebaseMismatch`],
    /// [`ModelStatus::SourceAfterEnd`], or whatever the edit itself refuses.
    pub fn apply(&mut self, id: SequenceId, edit: Edit) -> Result<()> {
        self.validate(id, &edit)?;
        let sequence = self.sequences.get_mut(id)?;
        self.history.apply(sequence, edit)
    }

    /// Undo the most recent edit to a sequence.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::NothingToDo`], or an identifier refusal.
    pub fn undo(&mut self, id: SequenceId) -> Result<Edit> {
        let sequence = self.sequences.get_mut(id)?;
        self.history.undo(sequence)
    }

    /// Redo the most recently undone edit to a sequence.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::NothingToDo`], or an identifier refusal.
    pub fn redo(&mut self, id: SequenceId) -> Result<Edit> {
        let sequence = self.sequences.get_mut(id)?;
        self.history.redo(sequence)
    }

    /// Discard the undo history, keeping the project exactly as it is.
    ///
    /// Loading a file uses this. The edits that build a project out of its
    /// saved structure are how the structure is expressed, not something the
    /// user did, and offering to undo them would offer to undo the file
    /// itself. A freshly opened project has nothing to undo, which is what
    /// every editor means by opening one.
    pub fn forget_history(&mut self) {
        self.history = EditJournal::new();
    }

    /// Check an edit against the media library before it is applied.
    ///
    /// Four edits can put a clip's source range outside its media: inserting a
    /// clip, lengthening one, slipping one, and **retiming** one (a freeze is
    /// the one that cannot, and is checked anyway). Each is
    /// checked here, on the clip the edit would produce, before anything
    /// changes. Removals, splits, joins, and track edits cannot introduce a
    /// reference or widen a range, so they have nothing to check.
    ///
    /// Validation uses [`crate::Clip::source_span`], so retimed clips are checked
    /// against the source range they actually read rather than their timeline
    /// duration.
    fn validate(&self, id: SequenceId, edit: &Edit) -> Result<()> {
        let sequence = self.sequences.get(id)?;
        let held = |track: &usize, index: &usize| -> Result<Option<crate::Clip>> {
            match sequence.track(*track)?.item(*index)? {
                Item::Clip(clip) => Ok(Some(clip.clone())),
                Item::Gap(_) => Ok(None),
            }
        };
        match edit {
            Edit::InsertItem { item, .. } => {
                let Item::Clip(clip) = item else {
                    return Ok(());
                };
                self.check_source(sequence, clip)
            }
            Edit::SetItemDuration {
                track,
                index,
                duration,
            } => {
                let Some(clip) = held(track, index)? else {
                    return Ok(());
                };
                self.check_source(sequence, &clip.with_duration(*duration)?)
            }
            Edit::SetClipSource {
                track,
                index,
                source_start,
            } => {
                let Some(clip) = held(track, index)? else {
                    return Ok(());
                };
                self.check_source(sequence, &clip.with_source(*source_start)?)
            }
            Edit::SetClipPlayback {
                track,
                index,
                playback,
            } => {
                let Some(clip) = held(track, index)? else {
                    return Ok(());
                };
                // The same construction the edit itself uses, asked for by
                // name rather than written out again here.
                self.check_source(sequence, &playback.applied_to(&clip)?)
            }
            _ => Ok(()),
        }
    }

    /// Whether what a clip reads lies inside the media it names.
    fn check_source(&self, sequence: &Sequence, clip: &crate::Clip) -> Result<()> {
        let asset = self
            .media
            .get(clip.media())
            .map_err(|_| ModelStatus::UnknownMedia)?;
        if asset.timebase() != sequence.timebase() {
            return Err(ModelStatus::MediaTimebaseMismatch);
        }
        let (lowest, highest) = clip.source_span()?;
        if lowest < 0 {
            return Err(ModelStatus::SourceBeforeStart);
        }
        // The span's ends are both frames the clip shows, so the last one has
        // to *be* a frame of the asset rather than sit one past the last --
        // which is why this compares against the length rather than to it.
        if highest >= asset.duration().ticks() {
            return Err(ModelStatus::SourceAfterEnd);
        }
        Ok(())
    }
}
