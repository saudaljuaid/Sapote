// SPDX-License-Identifier: GPL-3.0-only
//! Render a sequence's layer stack.
//!
//! Layers composite bottom-up over opaque black using premultiplied linear-light
//! `over`. Dissolves use the same operator with animated opacity. Media access is
//! provided through [`sapstudio_render::Library`]. Rendering builds a graph whose
//! digest-based keys allow decoded frames and shared media to be cached safely.

use alloc::vec::Vec;

use sapstudio_core::Instant;
use sapstudio_media::{AlphaState, Frame, FrameDescription, FramePool, TestPattern};
use sapstudio_model::{Lane, Layer, Project, Sequence, SequenceId};
use sapstudio_render::{Graph, Library, Node, NodeId};

use crate::SlateStatus;

/// How many characters of a digest a slate shows.
///
/// Eight. Enough to pick one clip out of a bin by eye, short enough to read
/// off a screen at a glance, and the same prefix the conform module already
/// writes into a reel name — so the slate and the edit decision list name the
/// same thing the same way.
const SLATE_DIGEST_CHARACTERS: usize = 8;

/// What a slate says about a clip whose media is not there.
///
/// The hex is written out here rather than taken from the digest's `Display`,
/// because that would want a `String` built by an allocation nobody can refuse
/// — and everything in this program allocates by asking first (R-5.2).
fn missing(
    media: sapstudio_core::Digest,
) -> Result<(alloc::string::String, alloc::string::String), SlateStatus> {
    const LABEL: &str = "MEDIA OFFLINE ";
    const HEX: [u8; 16] = *b"0123456789ABCDEF";
    let out_of_memory = || SlateStatus::Render(sapstudio_render::RenderStatus::OutOfMemory);
    let mut brief = alloc::string::String::new();
    brief
        .try_reserve(SLATE_DIGEST_CHARACTERS)
        .map_err(|_| out_of_memory())?;
    for byte in media.bytes().iter().take(SLATE_DIGEST_CHARACTERS / 2) {
        brief.push(char::from(HEX[usize::from(byte >> 4)]));
        brief.push(char::from(HEX[usize::from(byte & 0x0F)]));
    }
    let mut text = alloc::string::String::new();
    text.try_reserve(LABEL.len() + brief.len())
        .map_err(|_| out_of_memory())?;
    text.push_str(LABEL);
    text.push_str(&brief);
    Ok((text, brief))
}

/// The node that draws a title card.
///
/// The alignment is translated rather than shared, because the model and the
/// renderer are *siblings* and neither may depend on the other: the model owns
/// which alignment somebody chose, and the renderer owns what the choice
/// means. This one `match` is the price of that, and it is the price the
/// resampler's filter already pays.
fn typeset(title: &sapstudio_model::Title, description: FrameDescription) -> Node {
    Node::Type {
        description,
        lines: title.lines().to_vec(),
        size: title.size(),
        across: title.across(),
        down: title.down(),
        alignment: match title.alignment() {
            sapstudio_model::Alignment::Left => sapstudio_render::font::Alignment::Left,
            sapstudio_model::Alignment::Centre => sapstudio_render::font::Alignment::Centre,
            sapstudio_model::Alignment::Right => sapstudio_render::font::Alignment::Right,
        },
        // The ink crosses as three rationals rather than as a type, because
        // both sides already agree on what a fraction of full light is and
        // neither has to learn the other's name for a colour.
        ink: title.ink().channels(),
    }
}

/// Build the graph that renders one instant of a sequence.
///
/// Separate from evaluating it, because they are separate questions and a
/// caller may want only the first: an export that wants to know whether an
/// instant needs decoding at all, or an interface drawing what is on the
/// timeline, has no frames to fetch.
///
/// # Errors
///
/// [`SlateStatus::Model`] if the instant is not in the sequence's timebase or
/// the sequence is not in the project, [`SlateStatus::BaseNotPremultiplied`]
/// if the description is not one this can composite onto, and
/// [`SlateStatus::Render`] if the graph is at capacity.
pub fn plan(
    project: &Project,
    sequence: SequenceId,
    instant: Instant,
    description: FrameDescription,
    library: &mut dyn Library,
) -> Result<(Graph, NodeId), SlateStatus> {
    if description.alpha() != Some(AlphaState::Premultiplied) {
        // Compositing is only correct on premultiplied values, so the base of
        // the stack has to be one. Refusing rather than converting keeps the
        // decision where the caller made it (R-1.3).
        return Err(SlateStatus::BaseNotPremultiplied);
    }
    let held = project.sequence(sequence)?;
    let stack = held.stack_at(Lane::Picture, instant)?;

    let mut graph = Graph::new();
    // The programme is opaque: an empty instant is black leader, not a hole. A
    // viewer shows black and an export writes it, and neither shows whatever
    // was behind the window.
    let mut top = graph.add(Node::Blank { description })?;
    for layer in &stack {
        let asset = project.media().get(layer.media())?;
        // A graded layer is fetched *straight*. A look is a non-linear
        // function and on premultiplied samples computes `f(ac)` where
        // `a·f(c)` was wanted; `over` is only correct on premultiplied ones.
        // The two want opposite things, so the frame arrives the way the look
        // needs it and is associated afterwards — which loses nothing, since
        // it was never premultiplied to begin with. Unpremultiplying a frame
        // that already had been would.
        let fetched = match layer.grade() {
            None => description,
            Some(_) => description
                .with_alpha(AlphaState::Straight)
                .map_err(SlateStatus::Media)?,
        };
        // Ask whether the bytes are there *before* naming them, rather than
        // finding out while evaluating. A source node's identity covers the
        // media, the tick and the description and not whether the file
        // happened to be reachable — so a node that fell back to a slate
        // during evaluation would put that slate in the cache under the real
        // picture's key and hand it back after the drive came home. The
        // fallback belongs here, where nothing is cached by it.
        let source = if let Some(title) = asset.title() {
            // A title is media the program *makes*, so it goes where a source
            // would and everything above it -- the grade, the mask, the
            // transform, the fade -- is the machinery a recording already goes
            // through. Nothing else in this function had to be told.
            //
            // And it is never offline. There is nothing to find, so the
            // question the branch below asks does not arise: asking the
            // library whether it has a title would be asking about a file that
            // does not exist, and a library that answered "no" would put a
            // slate where somebody's card should be.
            graph.add(typeset(title, fetched))?
        } else if library.available(asset.digest()) {
            graph.add(Node::Source {
                media: asset.digest(),
                tick: layer.source(),
                description: fetched,
            })?
        } else {
            let slate = graph.add(Node::Pattern {
                pattern: TestPattern::Offline,
                description: fetched,
            })?;
            // Name missing media by digest because clips refer to content;
            // filenames are only location hints.
            let (text, brief) = missing(asset.digest())?;
            graph.add(Node::Legend {
                input: slate,
                text,
                brief,
            })?
        };
        // The grade goes on before the fade, and that order is a decision. A
        // look is a function of colour; fading is a statement about how much
        // of a layer is showing. Grading a half-faded layer would ask the
        // table about a colour that is neither on the screen nor in the clip.
        // So: graded, then shown at whatever opacity it is at.
        let graded = match layer.grade() {
            None => source,
            Some(grade) => {
                // The node is added whatever the strength reads, including
                // nought. Skipping it there would be an optimisation whose
                // absence changes no answer — a look at no strength hands back
                // the frame's own code values exactly — and this project has
                // four entries in its notes about guards no test can hold.
                // The look is fetched straight for the same reason at every
                // strength, so there would be nothing else to skip either.
                let looked = graph.add(Node::Look {
                    input: source,
                    look: grade.look(),
                    strength: grade.strength(),
                })?;
                graph.add(Node::Associate {
                    input: looked,
                    target: description,
                })?
            }
        };
        // The transform goes between the grade and the mask, and both halves
        // of that are decisions. After the grade, because a look is a function
        // of colour and resampling averages colours -- grading the average is
        // not the average of the grade, and the table was written for the
        // clip's own values rather than for whatever a scale produced. Before
        // the mask, because a mask is in *frame* coordinates: moving a clip
        // moves the picture through a stationary mask, which is what a
        // garbage matte and a split screen both want. A mask that travelled
        // with its clip would be a different feature, and it would have to say
        // so.
        //
        // A transform that moves nothing is skipped entirely rather than
        // resampled by the identity. Exact is a stronger promise than "the
        // arithmetic works out", and it is the promise a project deserves
        // after being opened and saved a hundred times.
        let placed = match layer.transform() {
            Some(transform) if !transform.is_still() => graph.add(Node::Transform {
                input: graded,
                linear: transform.linear(),
                offset: transform.offset(),
                anchor: transform.anchor(),
                bilinear: matches!(
                    transform.resampling(),
                    sapstudio_model::Resampling::Bilinear
                ),
            })?,
            _ => graded,
        };
        // The mask goes on after the grade and before the fade, and both
        // halves of that are decisions. After the grade, because a mask is
        // about *where* and a look is about colour, and grading only the part
        // that survives would ask the table about a picture nobody assembled.
        // Before the fade, because the mask says what this clip *is* and the
        // fade says how much of the track is showing — and a fade applied
        // first would then be masked away in the places the mask cuts, which
        // is the same number by accident and the wrong order in principle.
        let shaped = match layer.mask() {
            None => placed,
            Some(mask) => graph.add(Node::Mask {
                input: placed,
                corners: mask.corners().to_vec(),
                inverted: mask.is_inverted(),
            })?,
        };
        // The track's opacity and the clip's own fade multiply, and they go
        // into one node rather than two. Two would be two rounding points and
        // two cache entries for one picture; one is exact, because the product
        // of two rationals is a rational.
        let showing = layer
            .opacity()
            .checked_mul(layer.fade())
            .map_err(sapstudio_model::ModelStatus::from)?;
        let faded = if showing == sapstudio_core::Rational::ONE {
            shaped
        } else {
            graph.add(Node::Fade {
                input: shaped,
                opacity: showing,
            })?
        };
        // A wipe after the fade, and that order is the same decision the grade
        // makes in the other direction. The fade is the track's automation,
        // which says how much of this track is showing at all; the wipe is the
        // transition, which says how much of *this clip* has been revealed.
        // Wiping first and then fading would give the same picture here, since
        // both are multiplications by a coverage — but only because neither is
        // a function of colour, and putting the one that is (the grade) in the
        // wrong place is exactly the mistake this ordering exists to avoid.
        let revealed = match layer.wipe() {
            None => faded,
            Some(sweep) => graph.add(Node::Wipe {
                input: faded,
                across: sweep.wipe().across(),
                down: sweep.wipe().down(),
                fraction: sweep.fraction(),
                softness: sweep.wipe().softness(),
            })?,
        };
        top = graph.add(Node::Over {
            layers: [top, revealed],
        })?;
    }
    Ok((graph, top))
}

/// Render one instant of a sequence.
///
/// The picture tracks are composited bottom first onto opaque black in
/// `description`, so the result is always exactly that description and always
/// opaque — a viewer never has to ask what it is looking at.
///
/// # Errors
///
/// As [`plan`], plus [`sapstudio_render::RenderStatus::SourceDescriptionMismatch`]
/// if a source hands back a frame that is not what was asked for, and whatever
/// the source itself refuses.
pub fn render(
    project: &Project,
    sequence: SequenceId,
    instant: Instant,
    description: FrameDescription,
    pool: &mut FramePool,
    library: &mut dyn Library,
) -> Result<Frame, SlateStatus> {
    let (graph, root) = plan(project, sequence, instant, description, library)?;
    Ok(graph.evaluate(root, pool, library)?)
}

/// What a sequence shows at an instant, without rendering it.
///
/// Useful on its own: this is what a timeline draws in its own interface, and
/// what an export uses to decide whether an instant needs decoding at all.
///
/// # Errors
///
/// As [`sapstudio_model::Sequence::stack_at`].
pub fn stack(sequence: &Sequence, instant: Instant) -> Result<Vec<Layer>, SlateStatus> {
    Ok(sequence.stack_at(Lane::Picture, instant)?)
}
