// SPDX-License-Identifier: GPL-3.0-only
//! Mix sequence audio over a time span.
//!
//! Sample boundaries use [`sapstudio_core::Instant::floor_into`] at both ends,
//! so fractional samples per video frame alternate without drift, overlap, or
//! gaps. Each track uses its saved fader and automation. Decibel values are
//! converted to gain here; muted tracks contribute no source.

use alloc::vec::Vec;

use sapstudio_audio::mix::Source;
use sapstudio_audio::{AudioBuffer, Gain, MixReport, SampleRate};
use sapstudio_core::{Instant, Rational, TimeRange, Timebase};
use sapstudio_model::{Lane, MediaId, Sequence};

use crate::SlateStatus;

/// Where samples of media come from.
///
/// The sound counterpart of [`sapstudio_render::Library`].
pub trait SampleSource {
    /// `count` samples of `media`, beginning at sample `start` of it.
    ///
    /// The buffer must have the rate and channel count that was asked for, and
    /// exactly `count` samples. A source that returns a different length has
    /// answered a different question.
    ///
    /// # Errors
    ///
    /// Whatever the source cannot do.
    fn samples(
        &mut self,
        media: MediaId,
        start: i64,
        count: usize,
    ) -> Result<AudioBuffer, SlateStatus>;
}

/// The mixer's gain for a fader position.
fn gain_of(fader: sapstudio_model::Fader) -> Result<Gain, SlateStatus> {
    match fader.decibels() {
        None => Ok(Gain::SILENT),
        Some(decibels) => Gain::decibels(decibels).map_err(SlateStatus::Audio),
    }
}

/// The timebase a sample rate counts in.
fn timebase_of(rate: SampleRate) -> Timebase {
    match rate {
        SampleRate::Hz44100 => Timebase::AUDIO_44K1,
        SampleRate::Hz48000 => Timebase::AUDIO_48K,
        SampleRate::Hz88200 => Timebase::AUDIO_88K2,
        SampleRate::Hz96000 => Timebase::AUDIO_96K,
    }
}

/// Which sample a timeline instant falls in.
fn sample_of(instant: Instant, rate: SampleRate) -> Result<i64, SlateStatus> {
    Ok(instant.floor_into(timebase_of(rate))?.ticks())
}

/// Mix a sequence's sound tracks over a span.
///
/// The result holds exactly the samples the span covers, which is
/// `sample_of(end) - sample_of(start)` and is *not* the frame count times
/// anything.
///
/// # Errors
///
/// [`SlateStatus::Model`] if the range is not in the sequence's timebase,
/// [`SlateStatus::Audio`] if the sources do not agree with each other or with
/// what was asked for, and whatever a source refuses.
pub fn mix(
    sequence: &Sequence,
    range: TimeRange,
    rate: SampleRate,
    channels: usize,
    source: &mut dyn SampleSource,
) -> Result<(AudioBuffer, MixReport), SlateStatus> {
    let start = sample_of(range.start(), rate)?;
    let end = sample_of(
        range.end().map_err(sapstudio_model::ModelStatus::Time)?,
        rate,
    )?;
    let total = usize::try_from(end - start)
        .map_err(|_| SlateStatus::Audio(sapstudio_audio::AudioStatus::BufferTooLong))?;

    let mut written: Vec<Vec<i32>> = Vec::new();
    written
        .try_reserve(channels)
        .map_err(|_| SlateStatus::Audio(sapstudio_audio::AudioStatus::OutOfMemory))?;
    for _ in 0..channels {
        let mut channel = Vec::new();
        channel
            .try_reserve(total)
            .map_err(|_| SlateStatus::Audio(sapstudio_audio::AudioStatus::OutOfMemory))?;
        written.push(channel);
    }
    let mut report = MixReport::default();

    // Frame by frame, because that is the granularity at which the stack can
    // change: an item boundary is on a frame, so within one frame the set of
    // layers is fixed and their sample spans are contiguous.
    let mut frame = range.start();
    let end_instant = range.end().map_err(sapstudio_model::ModelStatus::Time)?;
    while frame.ticks() < end_instant.ticks() {
        let next = Instant::new(
            frame.ticks().checked_add(1).ok_or(SlateStatus::Model(
                sapstudio_model::ModelStatus::Time(sapstudio_core::CoreStatus::Overflow),
            ))?,
            frame.timebase(),
        );
        let span = sample_of(next, rate)? - sample_of(frame, rate)?;
        let count = usize::try_from(span)
            .map_err(|_| SlateStatus::Audio(sapstudio_audio::AudioStatus::BufferTooLong))?;

        let block = frame_block(
            sequence,
            Block {
                from: frame,
                to: next,
                rate,
                channels,
                count,
            },
            source,
            &mut report,
        )?;
        for (channel, samples) in written.iter_mut().enumerate() {
            let held = block.channel(channel).map_err(SlateStatus::Audio)?;
            samples
                .try_reserve(held.len())
                .map_err(|_| SlateStatus::Audio(sapstudio_audio::AudioStatus::OutOfMemory))?;
            samples.extend_from_slice(held);
        }
        frame = next;
    }

    // The blocks are supposed to tile the span exactly — that is the whole
    // claim of the floor. Checking it here costs one comparison and turns a
    // silent disagreement into a named refusal: a buffer that is a sample
    // short is a click at the end of every export, and it would otherwise
    // arrive as a perfectly valid buffer of the wrong length.
    if written.first().map(Vec::len) != Some(total) {
        return Err(SlateStatus::Audio(sapstudio_audio::AudioStatus::NotMixable));
    }

    Ok((
        AudioBuffer::new(rate, written).map_err(SlateStatus::Audio)?,
        report,
    ))
}

/// One frame of the mix: the span it covers and the buffer it must produce.
///
/// The two instants are this frame and the next, not this frame and a length.
/// A fader is read at both, and the second is by construction the *first*
/// instant of the following block — so a move that crosses a frame boundary
/// is handed to both blocks as the same number and they join without a step.
#[derive(Clone, Copy)]
struct Block {
    /// The instant this frame begins.
    from: Instant,
    /// The instant the next frame begins.
    to: Instant,
    /// What rate the samples are counted at.
    rate: SampleRate,
    /// How many channels to produce.
    channels: usize,
    /// How many samples this frame holds — 1601 or 1602 at 29.97, never both.
    count: usize,
}

/// One frame's worth of samples, with every layer on it summed.
fn frame_block(
    sequence: &Sequence,
    block: Block,
    source: &mut dyn SampleSource,
    report: &mut MixReport,
) -> Result<AudioBuffer, SlateStatus> {
    let Block {
        from: frame,
        to: next,
        rate,
        channels,
        count,
    } = block;
    let stack = sequence.stack_at(Lane::Sound, frame)?;
    let silence = AudioBuffer::silence(rate, channels, count).map_err(SlateStatus::Audio)?;
    if stack.is_empty() {
        // Nothing on any track is silence, not a hole. A programme with no
        // sound at an instant is a programme with silence there, and an export
        // writes it.
        return Ok(silence);
    }

    let mut held = Vec::new();
    held.try_reserve(stack.len())
        .map_err(|_| SlateStatus::Audio(sapstudio_audio::AudioStatus::OutOfMemory))?;
    let mut gains = Vec::new();
    gains
        .try_reserve(stack.len())
        .map_err(|_| SlateStatus::Audio(sapstudio_audio::AudioStatus::OutOfMemory))?;
    for layer in &stack {
        let track = sequence.track(layer.track())?;
        // Both ends of the block, because a fader may be moving across it. The
        // second is the *next* frame's position, which is the first position
        // of the next block — so the ramps tile the move exactly, for the same
        // reason the sample counts tile the span.
        let gain = gain_of(track.fader_at(frame)?)?;
        let arriving = gain_of(track.fader_at(next)?)?;
        if gain.is_silent() {
            // A muted track is not decoded. There is nothing to fetch and
            // nothing a fetch could contribute, and reading media for a track
            // nobody will hear is the difference between a mix that keeps up
            // and one that does not.
            //
            // One end is enough, and only because mute cannot change within a
            // block: it is a switch on the track rather than a position a
            // curve can reach, so a track is muted for the whole mix or none
            // of it. If mute ever gets automation of its own — a separate lane
            // of its own, as a console has — this becomes a two-ended question
            // and skipping the fetch on the strength of the first sample would
            // put a hole at the start of every unmute.
            continue;
        }
        gains.push((gain, arriving));
        // Where the clip's own fade is at each end of the block. The fader is
        // a gain in decibels and this is a fraction of the material, and they
        // are two different things multiplied together -- so the fade scales
        // the samples and the fader scales the source, rather than one of them
        // being converted into the other's units.
        let fading = (layer.fade(), layer.fade_arriving());
        // The layer's source position is a timeline tick of the media, so it
        // goes through the same floor: a clip that starts on a frame boundary
        // starts on the sample that frame boundary falls in, and two clips of
        // the same material at the same offset ask for the same samples.
        let begins = sample_of(Instant::new(layer.source(), frame.timebase()), rate)?;
        let block = source.samples(layer.media(), begins, count)?;
        if block.rate() != rate || block.channel_count() != channels || block.len() != count {
            // A source that answers with a different span has answered a
            // different question, and resampling or padding it here would be a
            // decision made in the wrong place (R-1.3).
            return Err(SlateStatus::Audio(sapstudio_audio::AudioStatus::NotMixable));
        }
        held.push(if fading.0 == Rational::ONE && fading.1 == Rational::ONE {
            // A clip nobody has faded is not copied. Scaling every sample by
            // one is exact and is still a pass over the whole block.
            block
        } else {
            block
                .faded(fading.0, fading.1)
                .map_err(SlateStatus::Audio)?
        });
    }

    if held.is_empty() {
        // Every track that reaches here was muted, which is silence — the same
        // answer as no tracks at all.
        return Ok(silence);
    }

    let sources: Vec<Source<'_>> = held
        .iter()
        .zip(gains.iter())
        .map(|(buffer, (gain, arriving))| {
            if gain == arriving {
                // A fader that is not moving stays a copy at unity and a
                // nought at silence, which the ramp cannot promise.
                Source::new(buffer, *gain)
            } else {
                Source::ramped(buffer, *gain, *arriving)
            }
        })
        .collect();
    let (mixed, block_report) = sapstudio_audio::mix(&sources).map_err(SlateStatus::Audio)?;
    report.clipped = report.clipped.saturating_add(block_report.clipped);
    report.overshoot = report.overshoot.max(block_report.overshoot);
    Ok(mixed)
}
