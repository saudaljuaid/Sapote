// SPDX-License-Identifier: GPL-3.0-only
//! The SapStudio project file.
//!
//! ```text
//! offset  size  field
//! 0       4     magic, "SPRJ"
//! 4       2     format version, little-endian
//! 6       2     reserved, must be zero
//! 8       8     payload length in bytes, little-endian
//! 16      32    SHA-256 of the payload
//! 48      N     payload
//! ```
//!
//! The file is versioned, length-prefixed, and protected by a payload digest
//! (R-9.3). The hand-written codec uses the model's own bounds. Edit history is
//! session state and is not stored in the project file.

use alloc::vec::Vec;

use sapstudio_core::{Digest, Duration, Instant, Rational, Timebase};
use sapstudio_model::curve::MAX_KEYFRAMES;
use sapstudio_model::mask::{MAX_CORNERS, Mask};
use sapstudio_model::media::{Location, MAX_LOCATION_BYTES};
use sapstudio_model::project::{MAX_MEDIA, MAX_SEQUENCES};
use sapstudio_model::sequence::MAX_TRACKS_PER_SEQUENCE;
use sapstudio_model::title::{Alignment, Ink, MAX_TITLE_LINES, MAX_TITLE_TEXT, Title};
use sapstudio_model::track::MAX_ITEMS_PER_TRACK;
use sapstudio_model::transform::{Motion, Resampling, Transform};
use sapstudio_model::{
    Clip, Curve, Edit, Fader, Interpolation, Item, Keyframe, MAX_MARKER_TEXT,
    MAX_MARKERS_PER_SEQUENCE, MediaAsset, MediaId, Playback, Project, TrackKind, Transition,
    TransitionKind, Wipe,
};

use crate::bytes::{Reader, Writer};
use crate::status::{IoStatus, Result};

/// The four bytes every project file begins with.
pub const MAGIC: [u8; 4] = *b"SPRJ";

/// The format this build writes.
/// Six. Version two added the fader, three the dissolves, four a picture
/// track's opacity over time, five a sound track's fader over time, and six a
/// clip's grade.
///
/// Version one had no fader. A project written by version one and read as
/// version two would have its faders read out of whatever followed them, which
/// is why the number moves for every change to what the bytes mean and not
/// only for changes that break (R-1.2). A clip's grade is written after its
/// length, so a version-five file read as version six would find a flag byte
/// in the next item's tag.
pub const FORMAT_VERSION: u16 = 24;

/// How long the fixed header is.
pub const HEADER_BYTES: usize = 48;

/// The largest payload this format accepts.
///
/// Sixteen mebibytes: a project is structure, not media, and a structural
/// description larger than this is a generated one rather than an edited one.
pub const MAX_PAYLOAD_BYTES: usize = 16 * 1024 * 1024;

/// How an item is tagged in the payload.
const TAG_CLIP: u8 = 0;
const TAG_GAP: u8 = 1;

/// How a track's kind is tagged in the payload.
const KIND_VIDEO: u8 = 0;
const KIND_AUDIO: u8 = 1;

/// A transition that cross-fades the whole frame.
const KIND_DISSOLVE: u8 = 0;

/// A transition that sweeps a straight edge across it.
const KIND_WIPE: u8 = 1;

/// Encode a project as a complete file.
///
/// # Errors
///
/// [`IoStatus::PayloadTooLarge`], [`IoStatus::OutOfMemory`], or a model
/// refusal if the project holds something the format cannot describe.
pub fn encode(project: &Project) -> Result<Vec<u8>> {
    let payload = encode_payload(project)?;
    let digest = Digest::of(&payload);

    let mut file = Writer::new(HEADER_BYTES + MAX_PAYLOAD_BYTES);
    file.bytes(&MAGIC)?;
    file.u16(FORMAT_VERSION)?;
    file.u16(0)?;
    file.u64(payload.len() as u64)?;
    file.bytes(digest.bytes())?;
    file.bytes(&payload)?;
    Ok(file.finish())
}

/// Write one item.
///
/// Keep this field order aligned with `read_item`.
fn write_item(writer: &mut Writer, item: &Item, media: &[(MediaId, &MediaAsset)]) -> Result<()> {
    match item {
        Item::Clip(clip) => {
            writer.u8(TAG_CLIP)?;
            let index = media
                .iter()
                .position(|(id, _)| *id == clip.media())
                .ok_or(IoStatus::MediaIndexOutOfRange)?;
            writer.u32(u32::try_from(index).map_err(|_| IoStatus::TooMany)?)?;
            writer.i64(clip.source_start())?;
            writer.i64(clip.duration().ticks())?;
            // A flag rather than a fixed thirty-two bytes, because
            // most clips have no grade and an all-zero digest is a
            // real digest of something rather than a spare value
            // to spend on "none".
            match clip.transform() {
                None => writer.u8(0)?,
                Some(transform) => {
                    writer.u8(1)?;
                    writer.u8(match transform.resampling() {
                        Resampling::Area => 0,
                        Resampling::Bilinear => 1,
                    })?;
                    for held in transform.linear() {
                        write_rational(writer, held)?;
                    }
                    write_rational(writer, transform.offset().0)?;
                    write_rational(writer, transform.offset().1)?;
                    // The pivot, after the move it is not interchangeable
                    // with. Thirty-two bytes a transform rather than a flag
                    // and a default, because a transform is already the most
                    // expensive thing a clip carries and a fifth tag to
                    // reserve would buy less than it costs to explain.
                    write_rational(writer, transform.anchor().0)?;
                    write_rational(writer, transform.anchor().1)?;
                }
            }
            // After the transform it changes, and before the mask, because a
            // motion is a modification of the framing rather than a fourth
            // independent property of the clip -- and a reader that has just
            // built the transform is the reader in the best position to refuse
            // an animation with nothing to animate.
            match clip.motion() {
                None => writer.u8(MOTION_NONE)?,
                Some(motion) => {
                    writer.u8(MOTION_PRESENT)?;
                    for lane in sapstudio_model::transform::lanes(motion) {
                        write_curve(writer, lane)?;
                    }
                }
            }
            // A count of nought when there is none, which is what
            // `write_curve` already writes for an absent lane -- four bytes,
            // the same as the motion's lanes cost, and no new tag to reserve.
            write_curve(writer, clip.opacity())?;
            // The mask's animation, written exactly as the framing's is and
            // read back the same way. Its tag comes *before* the mask itself
            // reads, which is deliberate: the decoder can then refuse an
            // animation with no shape at the moment it has both in hand.
            match clip.mask_motion() {
                None => writer.u8(MOTION_NONE)?,
                Some(motion) => {
                    writer.u8(MOTION_PRESENT)?;
                    for lane in sapstudio_model::transform::lanes(motion) {
                        write_curve(writer, lane)?;
                    }
                }
            }
            match clip.mask() {
                None => writer.u8(MASK_NONE)?,
                Some(mask) => {
                    writer.u8(MASK_PRESENT)?;
                    writer.u8(u8::from(mask.is_inverted()))?;
                    writer
                        .u32(u32::try_from(mask.corners().len()).map_err(|_| IoStatus::TooMany)?)?;
                    for corner in mask.corners() {
                        write_rational(writer, corner.0)?;
                        write_rational(writer, corner.1)?;
                    }
                }
            }
            // A flag again, and for the same reason: real time is the common
            // case and a fraction of one over one is sixteen bytes of saying
            // so.
            match clip.playback() {
                Playback::At(speed) if speed == sapstudio_core::Rational::ONE => {
                    writer.u8(SPEED_REAL_TIME)?;
                }
                Playback::At(speed) => {
                    writer.u8(SPEED_PRESENT)?;
                    write_rational(writer, speed)?;
                }
                // A tag and nothing after it. A freeze has no number: the
                // frame it holds is the clip's in point, which is already in
                // the file a few bytes above this.
                Playback::Frozen => writer.u8(SPEED_FROZEN)?,
            }
            // A flag, because a clip nobody has faded is the common one and
            // two lengths of nought are sixteen bytes of saying so.
            if clip.fade_in().is_zero() && clip.fade_out().is_zero() {
                writer.u8(FADES_NONE)?;
            } else {
                writer.u8(FADES_PRESENT)?;
                writer.i64(clip.fade_in().ticks())?;
                writer.i64(clip.fade_out().ticks())?;
            }
            match clip.grade() {
                None => writer.u8(GRADE_NONE)?,
                Some(grade) => {
                    writer.u8(GRADE_PRESENT)?;
                    writer.bytes(grade.bytes())?;
                }
            }
            // Grade strength follows the grade by format convention. An absent
            // curve uses write_curve's zero count.
            write_curve(writer, clip.grade_strength())?;
        }
        Item::Gap(duration) => {
            writer.u8(TAG_GAP)?;
            writer.i64(duration.ticks())?;
        }
    }
    Ok(())
}

/// Decode a complete file into a project.
///
/// Nothing is published until every byte has been accounted for: the header,
/// the digest, the whole structure, and the absence of anything after it.
///
/// # Errors
///
/// Any [`IoStatus`]. On a refusal nothing is returned, so a caller cannot act
/// on a partly decoded project (R-1.4).
pub fn decode(file: &[u8]) -> Result<Project> {
    if file.len() < HEADER_BYTES {
        return Err(IoStatus::TruncatedHeader);
    }
    let mut header = Reader::new(&file[..HEADER_BYTES]);
    if header.take(4)? != MAGIC {
        return Err(IoStatus::NotAProjectFile);
    }
    let version = header.u16()?;
    if version != FORMAT_VERSION {
        return Err(IoStatus::UnsupportedVersion(version));
    }
    if header.u16()? != 0 {
        return Err(IoStatus::ReservedFieldSet);
    }
    let declared = header.u64()?;
    let expected = Digest::new(header.digest_bytes()?);

    let declared = usize::try_from(declared).map_err(|_| IoStatus::PayloadTooLarge)?;
    if declared > MAX_PAYLOAD_BYTES {
        return Err(IoStatus::PayloadTooLarge);
    }
    let available = file.len() - HEADER_BYTES;
    if declared > available {
        return Err(IoStatus::TruncatedPayload);
    }
    if declared < available {
        return Err(IoStatus::TrailingBytes);
    }

    let payload = &file[HEADER_BYTES..];
    if Digest::of(payload) != expected {
        return Err(IoStatus::DigestMismatch);
    }
    decode_payload(payload)
}

/// Encode just the payload.
fn encode_payload(project: &Project) -> Result<Vec<u8>> {
    let mut writer = Writer::new(MAX_PAYLOAD_BYTES);

    // Media are written in slot order and referred to afterwards by their
    // position in this list. Generational identifiers are a runtime idea: they
    // describe which occupancy of a slot a reference meant, and a file has no
    // slots and no occupancies.
    let media: Vec<(MediaId, &MediaAsset)> = project.media().iter().collect();
    writer.u32(u32::try_from(media.len()).map_err(|_| IoStatus::TooMany)?)?;
    for (_, asset) in &media {
        writer.bytes(asset.digest().bytes())?;
        write_timebase(&mut writer, asset.timebase())?;
        writer.i64(asset.duration().ticks())?;
        // A length and then the bytes, uninterpreted. A path is whatever the
        // platform says it is, and this format does not know which platform
        // wrote it -- so it carries the hint and declines to read it.
        match asset.location() {
            None => writer.u32(0)?,
            Some(location) => {
                writer
                    .u32(u32::try_from(location.bytes().len()).map_err(|_| IoStatus::TooMany)?)?;
                writer.bytes(location.bytes())?;
            }
        }
        // Where its frames come from. A recording is a tag and nothing more --
        // the digest above already says what it is -- and a title is the
        // description the program draws.
        match asset.title() {
            None => writer.u8(SOURCE_RECORDED)?,
            Some(title) => {
                writer.u8(SOURCE_TITLE)?;
                writer.u32(u32::try_from(title.lines().len()).map_err(|_| IoStatus::TooMany)?)?;
                for line in title.lines() {
                    let words = line.as_bytes();
                    writer.u32(u32::try_from(words.len()).map_err(|_| IoStatus::TooMany)?)?;
                    writer.bytes(words)?;
                }
                writer.u8(match title.alignment() {
                    Alignment::Left => ALIGN_LEFT,
                    Alignment::Centre => ALIGN_CENTRE,
                    Alignment::Right => ALIGN_RIGHT,
                })?;
                write_rational(&mut writer, title.size())?;
                write_rational(&mut writer, title.across())?;
                write_rational(&mut writer, title.down())?;
                // A tag before the three channels, so a card nobody coloured
                // costs one byte to say so rather than forty-eight to say
                // white three times.
                if title.ink().is_white() {
                    writer.u8(INK_WHITE)?;
                } else {
                    writer.u8(INK_PRESENT)?;
                    for channel in title.ink().channels() {
                        write_rational(&mut writer, channel)?;
                    }
                }
            }
        }
    }

    let sequences: Vec<_> = project.sequences().iter().collect();
    writer.u32(u32::try_from(sequences.len()).map_err(|_| IoStatus::TooMany)?)?;
    for (_, sequence) in &sequences {
        write_timebase(&mut writer, sequence.timebase())?;
        writer.u32(u32::try_from(sequence.track_count()).map_err(|_| IoStatus::TooMany)?)?;
        for track in sequence.tracks() {
            writer.u8(match track.kind() {
                TrackKind::Video => KIND_VIDEO,
                TrackKind::Audio => KIND_AUDIO,
            })?;
            write_fader(&mut writer, track.fader())?;
            writer.u32(u32::try_from(track.len()).map_err(|_| IoStatus::TooMany)?)?;
            for item in track.items() {
                write_item(&mut writer, item, &media)?;
            }
            // Transitions come after the items rather than before, because
            // they name item indices and a reader cannot check one against a
            // track it has not read yet.
            let held = track.transitions();
            writer.u32(u32::try_from(held.len()).map_err(|_| IoStatus::TooMany)?)?;
            for transition in held {
                writer.u32(u32::try_from(transition.boundary()).map_err(|_| IoStatus::TooMany)?)?;
                writer.i64(transition.duration().ticks())?;
                // A tag before the parameters, so that a later kind adds a
                // tag rather than changing what the bytes after the duration
                // mean. Version six wrote no tag at all because there was one
                // kind, which is exactly the shape that makes adding a second
                // a format break rather than an extension.
                match transition.kind() {
                    TransitionKind::Dissolve => writer.u8(KIND_DISSOLVE)?,
                    TransitionKind::Wipe(wipe) => {
                        writer.u8(KIND_WIPE)?;
                        write_rational(&mut writer, wipe.across())?;
                        write_rational(&mut writer, wipe.down())?;
                        write_rational(&mut writer, wipe.softness())?;
                    }
                }
            }
            write_curve(&mut writer, track.opacity())?;
            write_curve(&mut writer, track.level())?;
        }
        // The markers come after every track, because a marker names an
        // instant in the *programme* rather than an index into anything -- so
        // unlike a transition, a reader needs nothing read first to check one.
        // Last rather than first only so that the file's order matches the
        // order somebody reading the two halves of this module would expect.
        let notes = sequence.markers();
        writer.u32(u32::try_from(notes.len()).map_err(|_| IoStatus::TooMany)?)?;
        for marker in notes {
            writer.i64(marker.at().ticks())?;
            let text = marker.text().as_bytes();
            writer.u32(u32::try_from(text.len()).map_err(|_| IoStatus::TooMany)?)?;
            writer.bytes(text)?;
        }
    }
    Ok(writer.finish())
}

/// Decode just the payload.
fn decode_payload(payload: &[u8]) -> Result<Project> {
    let mut reader = Reader::new(payload);
    let mut project = Project::new();

    let media_count = bounded(reader.u32()?, MAX_MEDIA)?;
    let mut media = Vec::new();
    media
        .try_reserve(media_count)
        .map_err(|_| IoStatus::OutOfMemory)?;
    for _ in 0..media_count {
        let digest = Digest::new(reader.digest_bytes()?);
        let timebase = read_timebase(&mut reader)?;
        let ticks = reader.i64()?;
        let duration = Duration::new(ticks, timebase)?;
        let length = bounded(reader.u32()?, MAX_LOCATION_BYTES)?;
        let location = if length == 0 {
            None
        } else {
            Some(Location::new(reader.take(length)?).map_err(IoStatus::Model)?)
        };
        // One asset per digest, so a file that lists one twice is not a file
        // this encoder could have written. `add_media` would quietly hand back
        // the identifier it already had, and every clip indexing the second
        // record would then point at the first -- which is a different
        // programme, arrived at silently. Refused instead.
        if project.find_media(digest).is_some() {
            return Err(IoStatus::DuplicateMedia);
        }
        let asset = read_asset(&mut reader, digest, timebase, duration, location)?;
        media.push(project.add_media(asset)?);
    }

    let sequence_count = bounded(reader.u32()?, MAX_SEQUENCES)?;
    for _ in 0..sequence_count {
        let timebase = read_timebase(&mut reader)?;
        let id = project.add_sequence(timebase)?;
        let track_count = bounded(reader.u32()?, MAX_TRACKS_PER_SEQUENCE)?;
        for track in 0..track_count {
            let tag = reader.u8()?;
            let kind = match tag {
                KIND_VIDEO => TrackKind::Video,
                KIND_AUDIO => TrackKind::Audio,
                other => return Err(IoStatus::UnknownTrackKind(other)),
            };
            project.apply(id, Edit::AddTrack { index: track, kind })?;
            let fader = read_fader(&mut reader)?;
            if fader != Fader::UNITY {
                // Applied as an edit like everything else, so that loading a
                // project uses the one way in and nothing can set a value the
                // model would have refused.
                project.apply(id, Edit::SetTrackFader { track, fader })?;
            }
            let item_count = bounded(reader.u32()?, MAX_ITEMS_PER_TRACK)?;
            for index in 0..item_count {
                let item = read_item(&mut reader, timebase, &media)?;
                project.apply(id, Edit::InsertItem { track, index, item })?;
            }
            let transition_count = bounded(reader.u32()?, MAX_ITEMS_PER_TRACK)?;
            for _ in 0..transition_count {
                let boundary = bounded(reader.u32()?, MAX_ITEMS_PER_TRACK)?;
                let duration = Duration::new(reader.i64()?, timebase).map_err(IoStatus::Time)?;
                let tag = reader.u8()?;
                let kind = match tag {
                    KIND_DISSOLVE => TransitionKind::Dissolve,
                    KIND_WIPE => {
                        let across = read_rational(&mut reader)?;
                        let down = read_rational(&mut reader)?;
                        let softness = read_rational(&mut reader)?;
                        TransitionKind::Wipe(
                            Wipe::soft(across, down, softness).map_err(IoStatus::Model)?,
                        )
                    }
                    other => return Err(IoStatus::UnknownTransitionTag(other)),
                };
                let transition =
                    Transition::of(boundary, duration, kind).map_err(IoStatus::Model)?;
                // Through the edit, like everything else, so a file cannot set
                // a dissolve the model itself would have refused — one on a
                // gap, one longer than its clips, or one whose incoming side
                // has no room for handles.
                project.apply(id, Edit::AddTransition { track, transition })?;
            }
            if let Some(opacity) = read_curve(&mut reader, timebase)? {
                // Through the edit, like everything else, so a file cannot set
                // a curve the model itself would have refused — one on the
                // wrong kind of track, or one counted in another timebase.
                project.apply(
                    id,
                    Edit::SetTrackOpacity {
                        track,
                        opacity: Some(opacity),
                    },
                )?;
            }
            if let Some(level) = read_curve(&mut reader, timebase)? {
                project.apply(
                    id,
                    Edit::SetTrackLevel {
                        track,
                        level: Some(level),
                    },
                )?;
            }
        }
        let marker_count = bounded(reader.u32()?, MAX_MARKERS_PER_SEQUENCE)?;
        for _ in 0..marker_count {
            let at = Instant::new(reader.i64()?, timebase);
            let words = bounded(reader.u32()?, MAX_MARKER_TEXT)?;
            let text = alloc::string::String::from_utf8(reader.take(words)?.to_vec())
                .map_err(|_| IoStatus::MarkerNotText)?;
            // Through the edit, like everything else, so a file cannot hold a
            // marker the model would have refused -- two at one instant, one
            // before the programme starts, or one carrying more text than the
            // bound allows.
            project.apply(id, Edit::AddMarker { at, text })?;
        }
    }

    if !reader.is_finished() {
        return Err(IoStatus::TrailingBytes);
    }

    // Loading is not editing: a freshly opened project has nothing to undo.
    // The edits above are how the structure is built, not a history the user
    // performed, and offering to undo them would offer to undo the file.
    project.forget_history();
    Ok(project)
}

/// A clip's framing, if it has one.
///
/// Out of [`read_item`] with its two neighbours, because a clip's decorations
/// are each a tag and a body and reading them in line made one function long
/// enough for the length lint to say so. Each of them ends by handing what it
/// read to the model's own constructor, which is the property worth being able
/// to see at a glance.
fn read_transform(reader: &mut Reader<'_>) -> Result<Option<Transform>> {
    match reader.u8()? {
        0 => Ok(None),
        1 => {
            let resampling = match reader.u8()? {
                0 => Resampling::Area,
                1 => Resampling::Bilinear,
                other => return Err(IoStatus::UnknownTransformTag(other)),
            };
            let mut linear = [sapstudio_core::Rational::ZERO; 4];
            for slot in &mut linear {
                *slot = read_rational(reader)?;
            }
            let across = read_rational(reader)?;
            let down = read_rational(reader)?;
            let pivot = (read_rational(reader)?, read_rational(reader)?);
            // Through the model's own constructor, so a file cannot hold a
            // transform the model would have refused -- one that flattens the
            // picture onto a line. The pivot goes on afterwards through
            // `with_anchor`, which is the same path an edit takes and which
            // refuses nothing: every point is a pivot, including points
            // outside the frame.
            Ok(Some(
                Transform::new(linear, (across, down), resampling)
                    .map_err(IoStatus::Model)?
                    .with_anchor(pivot),
            ))
        }
        other => Err(IoStatus::UnknownTransformTag(other)),
    }
}

/// A clip's animation of its framing, if it has one.
fn read_motion(reader: &mut Reader<'_>, timebase: Timebase) -> Result<Option<Motion>> {
    match reader.u8()? {
        MOTION_NONE => Ok(None),
        MOTION_PRESENT => {
            let scale = read_curve(reader, timebase)?;
            let across = read_curve(reader, timebase)?;
            let down = read_curve(reader, timebase)?;
            // The turn's lane last, matching `transform::lanes` -- which is
            // what the writer iterates, so the two orders are one list rather
            // than two that have to be kept agreeing.
            let turn = read_curve(reader, timebase)?;
            // Through the model's own constructor, so a file cannot hold a
            // motion the model would have refused -- one with no lanes at all,
            // or one that scales to nothing.
            Ok(Some(
                Motion::new(scale, across, down, turn).map_err(IoStatus::Model)?,
            ))
        }
        other => Err(IoStatus::UnknownMotionTag(other)),
    }
}

/// A clip's mask, if it has one.
fn read_mask(reader: &mut Reader<'_>) -> Result<Option<Mask>> {
    match reader.u8()? {
        MASK_NONE => Ok(None),
        MASK_PRESENT => {
            let inverted = match reader.u8()? {
                0 => false,
                1 => true,
                other => return Err(IoStatus::UnknownMaskTag(other)),
            };
            let count = bounded(reader.u32()?, MAX_CORNERS)?;
            let mut corners = Vec::new();
            for _ in 0..count {
                let x = read_rational(reader)?;
                let y = read_rational(reader)?;
                corners.push((x, y));
            }
            // Through the model's own constructor, like everything else, so a
            // file cannot hold a shape the model would have refused -- a
            // concave one, or one with no area.
            Ok(Some(
                Mask::new(corners)
                    .map_err(IoStatus::Model)?
                    .with_inversion(inverted),
            ))
        }
        other => Err(IoStatus::UnknownMaskTag(other)),
    }
}

/// One item.
fn read_item(reader: &mut Reader<'_>, timebase: Timebase, media: &[MediaId]) -> Result<Item> {
    match reader.u8()? {
        TAG_CLIP => {
            let index = usize::try_from(reader.u32()?).map_err(|_| IoStatus::TooMany)?;
            let id = *media.get(index).ok_or(IoStatus::MediaIndexOutOfRange)?;
            let source_start = reader.i64()?;
            let duration = Duration::new(reader.i64()?, timebase)?;
            let transform = read_transform(reader)?;
            let motion = read_motion(reader, timebase)?;
            if motion.is_some() && transform.is_none() {
                // The invariant the edit enforces, enforced again here. A
                // decoder that skipped it would let a file produce a project
                // no sequence of edits could have produced, and every later
                // reader of that project would be entitled to assume it could.
                return Err(IoStatus::Model(
                    sapstudio_model::ModelStatus::NoTransformToAnimate,
                ));
            }
            let opacity = read_curve(reader, timebase)?;
            let mask_motion = read_motion(reader, timebase)?;
            let mask = read_mask(reader)?;
            let playback = match reader.u8()? {
                SPEED_REAL_TIME => None,
                SPEED_PRESENT => Some(Playback::At(read_rational(reader)?)),
                SPEED_FROZEN => Some(Playback::Frozen),
                other => return Err(IoStatus::UnknownSpeedTag(other)),
            };
            let fades = match reader.u8()? {
                FADES_NONE => None,
                FADES_PRESENT => {
                    let rising = Duration::new(reader.i64()?, timebase)?;
                    let falling = Duration::new(reader.i64()?, timebase)?;
                    Some((rising, falling))
                }
                other => return Err(IoStatus::UnknownFadeTag(other)),
            };
            let grade = match reader.u8()? {
                GRADE_NONE => None,
                GRADE_PRESENT => Some(Digest::new(reader.digest_bytes()?)),
                other => return Err(IoStatus::UnknownGradeTag(other)),
            };
            let grade_strength = read_curve(reader, timebase)?;
            let clip = Clip::new(id, source_start, duration)?
                .with_grade(grade)
                .with_mask(mask)
                .with_transform(transform)
                .with_motion(motion)
                // Through the model's own constructor, so a file cannot hold
                // an animation of a mask that is not there -- a project no
                // sequence of edits could have produced.
                .with_mask_motion(mask_motion)
                .map_err(IoStatus::Model)?
                // Through the model's own constructor, like everything else,
                // though here the refusal cannot actually arise: the curve is
                // read at the sequence's timebase and the clip's duration is
                // built from the same one, so the two agree by construction.
                // The `?` is plumbing rather than a guard, and saying so is
                // better than leaving a reader to look for the test that
                // exercises it.
                .with_opacity(opacity)
                .map_err(IoStatus::Model)?
                // Through the model's own constructor again, and here the
                // refusal is a real one rather than plumbing: a file holding a
                // strength for a clip with no grade would produce a project no
                // sequence of edits could, and every later reader of it would
                // be entitled to assume the strength meant something.
                .with_grade_strength(grade_strength)
                .map_err(IoStatus::Model)?;
            let clip = match fades {
                None => clip,
                // Through the model's own constructor, so a file cannot hold a
                // clip whose fades outlast it.
                Some((rising, falling)) => {
                    clip.with_fades(rising, falling).map_err(IoStatus::Model)?
                }
            };
            Ok(Item::Clip(match playback {
                None => clip,
                // Likewise: a file cannot hold a clip stopped at a speed of
                // nought, or one reversed so far it reads before its media
                // begins. A freeze goes through `Clip::frozen`, which refuses
                // nothing -- it shrinks what the clip reads to one frame.
                Some(Playback::At(held)) => clip.with_speed(held).map_err(IoStatus::Model)?,
                Some(Playback::Frozen) => clip.frozen(),
            }))
        }
        TAG_GAP => {
            let duration = Duration::new(reader.i64()?, timebase)?;
            Ok(Item::gap(duration)?)
        }
        other => Err(IoStatus::UnknownItemTag(other)),
    }
}

/// Read one media asset's source, and build the asset.
///
/// Out of [`decode_payload`] because it reads before it decides, which is the
/// shape worth having a name — and because a title's own checks belong beside
/// the bytes they check rather than in the middle of a loop over sequences.
fn read_asset(
    reader: &mut Reader<'_>,
    digest: Digest,
    timebase: Timebase,
    duration: Duration,
    location: Option<Location>,
) -> Result<MediaAsset> {
    match reader.u8()? {
        SOURCE_RECORDED => {
            Ok(MediaAsset::new(digest, timebase, duration)?.with_location(location)?)
        }
        SOURCE_TITLE => {
            let count = bounded(reader.u32()?, MAX_TITLE_LINES)?;
            let mut lines = Vec::new();
            lines
                .try_reserve(count)
                .map_err(|_| IoStatus::OutOfMemory)?;
            for _ in 0..count {
                let words = bounded(reader.u32()?, MAX_TITLE_TEXT)?;
                lines.push(
                    alloc::string::String::from_utf8(reader.take(words)?.to_vec())
                        .map_err(|_| IoStatus::TitleNotText)?,
                );
            }
            let alignment = match reader.u8()? {
                ALIGN_LEFT => Alignment::Left,
                ALIGN_CENTRE => Alignment::Centre,
                ALIGN_RIGHT => Alignment::Right,
                other => return Err(IoStatus::UnknownAlignmentTag(other)),
            };
            let size = read_rational(reader)?;
            let across = read_rational(reader)?;
            let down = read_rational(reader)?;
            let ink = match reader.u8()? {
                INK_WHITE => None,
                INK_PRESENT => {
                    let mut channels = [sapstudio_core::Rational::ZERO; 3];
                    for slot in &mut channels {
                        *slot = read_rational(reader)?;
                    }
                    let [red, green, blue] = channels;
                    // Through the model's own constructor, so a file cannot
                    // hold an ink the model would have refused -- one brighter
                    // than white, or one that subtracts.
                    Some(Ink::new(red, green, blue).map_err(IoStatus::Model)?)
                }
                other => return Err(IoStatus::UnknownInkTag(other)),
            };
            // Through the model's own constructor, like everything else, so a
            // file cannot hold a title the model would have refused.
            let title =
                Title::new(lines, size, across, down, alignment).map_err(IoStatus::Model)?;
            let title = match ink {
                None => title,
                Some(held) => title.with_ink(held),
            };
            let asset = MediaAsset::titled(title, timebase, duration)?;
            if asset.digest() != digest {
                // A title is *named by* its description, so the two are one
                // fact written twice and a file where they disagree is a file
                // that has been edited. Recomputing and accepting whichever
                // came out would silently repoint every clip of the card;
                // refusing says what happened.
                return Err(IoStatus::TitleDigestMismatch);
            }
            if location.is_some() {
                // A title has nowhere to be, and the model refuses a hint on
                // one. A file carrying both would describe something no
                // sequence of edits could produce.
                return Err(IoStatus::Model(
                    sapstudio_model::ModelStatus::NotRecordedMedia,
                ));
            }
            Ok(asset)
        }
        other => Err(IoStatus::UnknownMediaSourceTag(other)),
    }
}

/// How a title's alignment is tagged.
///
/// The numbers are part of the format and may never be reassigned: a tag that
/// changed meaning would silently re-set every saved card (R-1.2).
const ALIGN_LEFT: u8 = 0;
/// Every line centred on the block's middle.
const ALIGN_CENTRE: u8 = 1;
/// Every line ending at the block's right edge.
const ALIGN_RIGHT: u8 = 2;

/// An asset whose bytes are somewhere.
const SOURCE_RECORDED: u8 = 0;

/// An asset the program draws from a description that follows.
const SOURCE_TITLE: u8 = 1;

/// A title nobody has coloured, which is white.
const INK_WHITE: u8 = 0;

/// A title whose three channels of light follow.
const INK_PRESENT: u8 = 1;

/// A clip playing its media in real time and forwards.
const SPEED_REAL_TIME: u8 = 0;

/// A clip whose speed follows as an exact fraction.
const SPEED_PRESENT: u8 = 1;

/// A clip held on the one frame at its in point.
const SPEED_FROZEN: u8 = 2;

/// A clip nobody has faded.
const FADES_NONE: u8 = 0;

/// A clip whose two fade lengths follow.
const FADES_PRESENT: u8 = 1;

/// A clip whose framing is held still.
const MOTION_NONE: u8 = 0;

/// A clip whose animation follows as three curves: scale, across, down.
const MOTION_PRESENT: u8 = 1;

/// A clip with no shape on it.
const MASK_NONE: u8 = 0;

/// A clip whose shape follows.
const MASK_PRESENT: u8 = 1;

/// A clip with no look on it.
const GRADE_NONE: u8 = 0;
/// A clip whose look follows as thirty-two bytes.
const GRADE_PRESENT: u8 = 1;

/// How the interpolations are tagged.
///
/// The numbers are part of the format and may never be reassigned: a tag that
/// changed meaning would turn every saved ease into a hold, silently (R-1.2).
const HOW_HOLD: u8 = 1;
/// A straight run to the next keyframe.
const HOW_LINEAR: u8 = 2;
/// A cubic Bézier, followed by its four handle components.
const HOW_EASE: u8 = 3;

/// Write one of a track's automation curves, or the absence of one.
///
/// A count of nought means no automation, which is not the same as a curve
/// holding one: a track with no automation has none to read, and the model
/// keeps them apart so automation can be switched off and back on without
/// losing the shape somebody drew.
fn write_curve(writer: &mut Writer, curve: Option<&Curve>) -> Result<()> {
    let Some(curve) = curve else {
        return writer.u32(0);
    };
    let keyframes = curve.keyframes();
    writer.u32(u32::try_from(keyframes.len()).map_err(|_| IoStatus::TooMany)?)?;
    for keyframe in keyframes {
        writer.i64(keyframe.at().ticks())?;
        write_rational(writer, keyframe.value())?;
        match keyframe.interpolation() {
            Interpolation::Hold => writer.u8(HOW_HOLD)?,
            Interpolation::Linear => writer.u8(HOW_LINEAR)?,
            Interpolation::Ease {
                out_x,
                out_y,
                in_x,
                in_y,
            } => {
                writer.u8(HOW_EASE)?;
                for handle in [out_x, out_y, in_x, in_y] {
                    write_rational(writer, handle)?;
                }
            }
        }
    }
    Ok(())
}

/// Read one of a track's automation curves, if it has one.
fn read_curve(reader: &mut Reader<'_>, timebase: Timebase) -> Result<Option<Curve>> {
    let count = bounded(reader.u32()?, MAX_KEYFRAMES)?;
    if count == 0 {
        return Ok(None);
    }
    let mut keyframes = Vec::new();
    keyframes
        .try_reserve(count)
        .map_err(|_| IoStatus::OutOfMemory)?;
    for _ in 0..count {
        let at = Instant::new(reader.i64()?, timebase);
        let value = read_rational(reader)?;
        let tag = reader.u8()?;
        let how = match tag {
            HOW_HOLD => Interpolation::Hold,
            HOW_LINEAR => Interpolation::Linear,
            HOW_EASE => Interpolation::Ease {
                out_x: read_rational(reader)?,
                out_y: read_rational(reader)?,
                in_x: read_rational(reader)?,
                in_y: read_rational(reader)?,
            },
            other => return Err(IoStatus::UnknownInterpolationTag(other)),
        };
        // Through the model's own constructor, so a file cannot carry a handle
        // that folds the curve back on itself.
        keyframes.push(Keyframe::new(at, value, how).map_err(IoStatus::Model)?);
    }
    Ok(Some(Curve::new(keyframes).map_err(IoStatus::Model)?))
}

/// Write a rational as its two halves.
///
/// Both, rather than a single scaled integer, because the whole point of the
/// type is that a third is a third: writing it as a decimal would be the one
/// place in the project file where an exact number stopped being one.
fn write_rational(writer: &mut Writer, value: Rational) -> Result<()> {
    writer.i64(value.numerator())?;
    writer.i64(value.denominator())
}

/// Read a rational, refusing one the core type would not build.
fn read_rational(reader: &mut Reader<'_>) -> Result<Rational> {
    let numerator = reader.i64()?;
    let denominator = reader.i64()?;
    Rational::new(numerator, denominator).map_err(IoStatus::Time)
}

/// How a muted fader is tagged, as against one at a level.
const FADER_MUTED: u8 = 0;

/// How a fader at a level is tagged.
const FADER_LEVEL: u8 = 1;

/// Write a track's fader.
///
/// A tag then a ratio, because "off" is not a point on the decibel scale and
/// encoding it as a very small number would make it one — a reader could then
/// not tell a muted track from a track turned all the way down, and unmuting
/// would not restore the level.
fn write_fader(writer: &mut Writer, fader: Fader) -> Result<()> {
    match fader.decibels() {
        None => {
            writer.u8(FADER_MUTED)?;
            writer.i64(0)?;
            writer.i64(1)
        }
        Some(decibels) => {
            writer.u8(FADER_LEVEL)?;
            writer.i64(decibels.numerator())?;
            writer.i64(decibels.denominator())
        }
    }
}

/// Read a track's fader, refusing a level the model would not accept.
fn read_fader(reader: &mut Reader<'_>) -> Result<Fader> {
    let tag = reader.u8()?;
    let numerator = reader.i64()?;
    let denominator = reader.i64()?;
    match tag {
        FADER_MUTED => Ok(Fader::MUTED),
        FADER_LEVEL => {
            let level = Rational::new(numerator, denominator).map_err(IoStatus::Time)?;
            Ok(Fader::at(level)?)
        }
        other => Err(IoStatus::UnknownFaderTag(other)),
    }
}

fn write_timebase(writer: &mut Writer, timebase: Timebase) -> Result<()> {
    writer.i64(timebase.rate().numerator())?;
    writer.i64(timebase.rate().denominator())
}

/// Read a timebase, refusing anything that is not a rate.
fn read_timebase(reader: &mut Reader<'_>) -> Result<Timebase> {
    let numerator = reader.i64()?;
    let denominator = reader.i64()?;
    Ok(Timebase::new(Rational::new(numerator, denominator)?)?)
}

/// Refuse a declared count the model could not hold anyway.
///
/// This is the check that keeps a hostile file from asking for four billion
/// tracks and being refused only after the allocator has tried (R-11.2).
fn bounded(declared: u32, limit: usize) -> Result<usize> {
    let count = usize::try_from(declared).map_err(|_| IoStatus::TooMany)?;
    if count > limit {
        return Err(IoStatus::TooMany);
    }
    Ok(count)
}
