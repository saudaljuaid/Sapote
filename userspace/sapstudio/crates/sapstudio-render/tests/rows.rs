// SPDX-License-Identifier: GPL-3.0-only
//! The graph, evaluated a row at a time.
//!
//! One property does the work here and everything else supports it: **for
//! every node and every row, `row(id, y)` is byte for byte the `y`-th row of
//! `evaluate(id)`**. Two evaluators that disagreed anywhere would be two
//! answers to one question, and the whole value of the row form is that it is
//! the same picture computed differently.
//!
//! Why it exists: a 1920×1080 eight-bit RGBA frame is 8,294,400 bytes and a
//! Sapote program is mapped 76 KiB, so `evaluate` allocates a hundred and six
//! times the program's whole address space *per node*. One row of it is 7,680
//! bytes.

use sapstudio_core::{Digest, Rational};
use sapstudio_media::colour::AlphaState;
use sapstudio_media::{
    ColourDescription, Frame, FrameDescription, FramePool, Geometry, PixelFormat, Plane,
};
use sapstudio_render::{Graph, Library, Look, Node, NodeId, RenderStatus};

const WIDE: u32 = 7;
const TALL: u32 = 5;

fn described(format: PixelFormat) -> FrameDescription {
    FrameDescription::square(
        Geometry::new(WIDE, TALL).expect("a geometry"),
        format,
        ColourDescription::srgb_full(),
        None,
        if format == PixelFormat::Rgba8 {
            Some(AlphaState::Premultiplied)
        } else {
            None
        },
    )
    .expect("a description")
}

/// A picture whose every byte is a function of where it is, so a row taken
/// from the wrong place is visible rather than plausible.
fn picture(description: FrameDescription, tint: u8) -> Frame {
    let format = description.format();
    let geometry = description.geometry();
    let stride = format.plane_row_bytes(geometry, 0).expect("a stride");
    let rows = geometry.height() as usize;
    let mut samples = std::vec::Vec::new();
    for row in 0..rows {
        for column in 0..stride {
            // Premultiplied RGBA has to hold colour no brighter than its own
            // coverage, so the alpha byte is full and the rest is bounded.
            let value = if format == PixelFormat::Rgba8 && column % 4 == 3 {
                255
            } else {
                u8::try_from((row * 29 + column * 11 + tint as usize) % 200).expect("a byte")
            };
            samples.push(value);
        }
    }
    Frame::new(
        description,
        std::vec![Plane::new(samples, stride).expect("a plane")],
    )
    .expect("a frame")
}

/// A library that serves whole frames and rows out of the same pictures.
///
/// The row it hands back is sliced from the frame it would have handed back,
/// which is what makes the agreement below a statement about the *graph*
/// rather than about two different sets of pixels.
struct Pictures {
    held: std::vec::Vec<(Digest, Frame)>,
    looks: std::vec::Vec<(Digest, Look)>,
    /// How many whole frames the library was asked for.
    frames_asked: usize,
    /// How many rows.
    rows_asked: usize,
    /// Answer rows with a frame of the wrong width.
    lies_in_rows: bool,
}

impl Pictures {
    fn new(held: std::vec::Vec<(Digest, Frame)>) -> Self {
        Self {
            held,
            looks: std::vec::Vec::new(),
            frames_asked: 0,
            rows_asked: 0,
            lies_in_rows: false,
        }
    }

    fn find(&self, media: Digest) -> Result<&Frame, RenderStatus> {
        self.held
            .iter()
            .find(|(digest, _)| *digest == media)
            .map(|(_, frame)| frame)
            .ok_or(RenderStatus::MediaAbsent)
    }
}

impl Library for Pictures {
    fn frame(
        &mut self,
        media: Digest,
        _tick: i64,
        description: FrameDescription,
    ) -> Result<Frame, RenderStatus> {
        self.frames_asked += 1;
        let held = self.find(media)?.clone();
        if held.description() != &description {
            return Err(RenderStatus::SourceDescriptionMismatch);
        }
        Ok(held)
    }

    fn look(&mut self, look: Digest) -> Result<Look, RenderStatus> {
        self.looks
            .iter()
            .find(|(digest, _)| *digest == look)
            .map(|(_, held)| held.clone())
            .ok_or(RenderStatus::LookAbsent)
    }

    fn row(
        &mut self,
        media: Digest,
        _tick: i64,
        description: FrameDescription,
        row: usize,
    ) -> Result<Frame, RenderStatus> {
        self.rows_asked += 1;
        let held = self.find(media)?.clone();
        if held.description() != &description {
            return Err(RenderStatus::SourceDescriptionMismatch);
        }
        if self.lies_in_rows {
            // A row of the right picture at the wrong width: legal on its own
            // and not what was asked for, which is what the graph's own check
            // is there to catch. A library that got this wrong by accident
            // would composite a short row into a full one.
            return slice(&picture(described(PixelFormat::Gray8), 1), row);
        }
        slice(&held, row)
    }
}

/// The `row`-th row of a frame, as a frame one row high.
fn slice(frame: &Frame, row: usize) -> Result<Frame, RenderStatus> {
    let description = *frame.description();
    let geometry = description.geometry();
    if row >= geometry.height() as usize {
        return Err(RenderStatus::OutsideDomain);
    }
    let plane = frame.plane(0)?;
    let stride = plane.stride();
    let one = FrameDescription::new(
        Geometry::new(geometry.width(), 1).map_err(RenderStatus::Media)?,
        description.format(),
        description.colour(),
        description.siting(),
        description.alpha(),
        description.pixel_aspect(),
    )
    .map_err(RenderStatus::Media)?;
    Frame::new(
        one,
        std::vec![
            Plane::new(
                plane.samples()[row * stride..(row + 1) * stride].to_vec(),
                stride,
            )
            .map_err(RenderStatus::Media)?
        ],
    )
    .map_err(RenderStatus::Media)
}

/// The linear part of an exact rotation: the three-four-five triangle, whose
/// cosine and sine are both rational and whose determinant is one.
///
/// A turn is the only kind of map this file can provoke `NotRowLocal` with
/// now. A scale, a translation, a mirror and a horizontal shear all take
/// horizontals to horizontals, so a row of each has a band of source rows for
/// a preimage and every one of them scans. A rotation takes a horizontal to a
/// slope, and a slope crosses every row of the picture it lies in.
fn turned() -> [Rational; 4] {
    let cosine = Rational::new(4, 5).expect("a cosine");
    let sine = Rational::new(3, 5).expect("a sine");
    [cosine, sine.checked_neg().expect("a sine"), sine, cosine]
}

fn pool() -> FramePool {
    FramePool::new(64, 1 << 20)
}

/// Evaluate a node whole and a row at a time, and require them to agree.
fn agrees(graph: &Graph, id: NodeId, library: &mut Pictures) {
    let whole = graph
        .evaluate(id, &mut pool(), library)
        .expect("a whole frame");
    for row in 0..whole.description().geometry().height() as usize {
        let one = graph.row(id, row, library).expect("a row");
        assert_eq!(
            one,
            slice(&whole, row).expect("a slice"),
            "row {row} of the two evaluators disagrees"
        );
    }
    // And one past the end is refused rather than wrapping.
    assert_eq!(
        graph
            .row(
                id,
                whole.description().geometry().height() as usize,
                library
            )
            .err(),
        Some(RenderStatus::OutsideDomain)
    );
}

#[test]
fn a_blank_and_an_empty_agree_row_for_row() {
    let mut graph = Graph::new();
    let blank = graph
        .add(Node::Blank {
            description: described(PixelFormat::Rgb8),
        })
        .expect("a node");
    let empty = graph
        .add(Node::Empty {
            description: described(PixelFormat::Rgba8),
        })
        .expect("a node");
    let mut library = Pictures::new(std::vec::Vec::new());
    agrees(&graph, blank, &mut library);
    agrees(&graph, empty, &mut library);
}

#[test]
fn a_source_agrees_row_for_row() {
    let description = described(PixelFormat::Rgb8);
    let media = Digest::of(b"a shot");
    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media,
            tick: 0,
            description,
        })
        .expect("a node");
    let mut library = Pictures::new(std::vec![(media, picture(description, 3))]);
    agrees(&graph, source, &mut library);
    // And the row form asked for rows rather than for frames: one whole frame
    // for the whole-frame evaluation, and one row per row.
    assert_eq!(library.frames_asked, 1);
    assert_eq!(
        library.rows_asked,
        usize::try_from(TALL).expect("a height"),
        "the row past the end was asked of the library rather than refused first"
    );
}

#[test]
fn every_row_local_node_agrees_row_for_row() {
    // The property, over a chain that uses every node kind the row form
    // covers: a source, faded, associated, masked, wiped, and composited over
    // another source.
    let description = described(PixelFormat::Rgba8);
    let under = Digest::of(b"under");
    let over = Digest::of(b"over");
    let mut graph = Graph::new();
    let bottom = graph
        .add(Node::Source {
            media: under,
            tick: 0,
            description,
        })
        .expect("a node");
    let top = graph
        .add(Node::Source {
            media: over,
            tick: 0,
            description,
        })
        .expect("a node");
    let faded = graph
        .add(Node::Fade {
            input: top,
            opacity: Rational::new(3, 5).expect("an opacity"),
        })
        .expect("a node");
    let masked = graph
        .add(Node::Mask {
            input: faded,
            corners: std::vec![
                (
                    Rational::new(1, 8).expect("a corner"),
                    Rational::new(1, 8).expect("a corner")
                ),
                (
                    Rational::new(7, 8).expect("a corner"),
                    Rational::new(1, 6).expect("a corner")
                ),
                (
                    Rational::new(3, 4).expect("a corner"),
                    Rational::new(7, 8).expect("a corner")
                ),
                (
                    Rational::new(1, 6).expect("a corner"),
                    Rational::new(3, 4).expect("a corner")
                ),
            ],
            inverted: false,
        })
        .expect("a node");
    let wiped = graph
        .add(Node::Wipe {
            input: masked,
            across: Rational::new(2, 3).expect("a direction"),
            down: Rational::new(1, 3).expect("a direction"),
            fraction: Rational::new(2, 5).expect("a fraction"),
            softness: Rational::new(1, 4).expect("a softness"),
        })
        .expect("a node");
    let composited = graph
        .add(Node::Over {
            layers: [bottom, wiped],
        })
        .expect("a node");
    let mut library = Pictures::new(std::vec![
        (under, picture(description, 7)),
        (over, picture(description, 61)),
    ]);
    agrees(&graph, composited, &mut library);
    // And every node along the way, not only the last.
    for id in [bottom, top, faded, masked, wiped] {
        agrees(&graph, id, &mut library);
    }
}

#[test]
fn an_inverted_mask_and_a_hard_wipe_agree_too() {
    // The two branches the coverage code takes that the chain above does not:
    // an inverted mask, and a wipe with no softness at all.
    let description = described(PixelFormat::Rgba8);
    let media = Digest::of(b"a shot");
    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media,
            tick: 0,
            description,
        })
        .expect("a node");
    let inverted = graph
        .add(Node::Mask {
            input: source,
            corners: std::vec![
                (
                    Rational::new(1, 4).expect("a corner"),
                    Rational::new(1, 4).expect("a corner")
                ),
                (
                    Rational::new(3, 4).expect("a corner"),
                    Rational::new(1, 4).expect("a corner")
                ),
                (
                    Rational::new(3, 4).expect("a corner"),
                    Rational::new(3, 4).expect("a corner")
                ),
            ],
            inverted: true,
        })
        .expect("a node");
    let hard = graph
        .add(Node::Wipe {
            input: inverted,
            across: Rational::ONE,
            down: Rational::ZERO,
            fraction: Rational::new(1, 2).expect("a fraction"),
            softness: Rational::ZERO,
        })
        .expect("a node");
    let mut library = Pictures::new(std::vec![(media, picture(description, 11))]);
    agrees(&graph, hard, &mut library);
}

#[test]
fn a_conversion_and_an_association_agree_row_for_row() {
    let straight = FrameDescription::square(
        Geometry::new(WIDE, TALL).expect("a geometry"),
        PixelFormat::Rgba8,
        ColourDescription::srgb_full(),
        None,
        Some(AlphaState::Straight),
    )
    .expect("a description");
    let premultiplied = described(PixelFormat::Rgba8);
    let media = Digest::of(b"a shot");
    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media,
            tick: 0,
            description: premultiplied,
        })
        .expect("a node");
    let associated = graph
        .add(Node::Associate {
            input: source,
            target: straight,
        })
        .expect("a node");
    let mut library = Pictures::new(std::vec![(media, picture(premultiplied, 23))]);
    agrees(&graph, associated, &mut library);
}

#[test]
fn a_turn_is_refused_because_a_row_is_not_its_preimage() {
    // Not "this build cannot": an output row of a resampled frame has a *line*
    // in its source for a preimage, and only a linear map that takes
    // horizontals to horizontals makes that line lie in a band of rows. A
    // rotation never does -- its preimage is a slope, and a slope crosses
    // every row of the picture it lies in. The status says which of the two it
    // is.
    let description = described(PixelFormat::Rgba8);
    let media = Digest::of(b"a shot");
    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media,
            tick: 0,
            description,
        })
        .expect("a node");
    let moved = graph
        .add(Node::Transform {
            input: source,
            linear: turned(),
            offset: (
                Rational::new(1, 10).expect("an offset"),
                Rational::new(1, 10).expect("an offset"),
            ),
            anchor: (
                Rational::new(1, 2).expect("an anchor"),
                Rational::new(1, 2).expect("an anchor"),
            ),
            bilinear: false,
        })
        .expect("a node");
    let mut library = Pictures::new(std::vec![(media, picture(description, 5))]);
    assert_eq!(
        graph.row(moved, 0, &mut library),
        Err(RenderStatus::NotRowLocal)
    );
    // And a node *above* a transform is refused too, because reaching it means
    // walking through one.
    let faded = graph
        .add(Node::Fade {
            input: moved,
            opacity: Rational::new(1, 2).expect("an opacity"),
        })
        .expect("a node");
    assert_eq!(
        graph.row(faded, 0, &mut library),
        Err(RenderStatus::NotRowLocal)
    );
    // The whole-frame evaluator still does it, which is the point of keeping
    // both.
    graph
        .evaluate(moved, &mut pool(), &mut library)
        .expect("a whole frame");
}

#[test]
fn a_row_of_a_source_reaches_the_caller_without_being_copied() {
    // `Graph::row` on a source hands back what the library handed it, and the
    // point is that it hands back the *bytes*, not a copy of them. Measured by
    // address rather than by contents: a graph that copied would still be
    // correct and would still pass every other test in this file.
    struct FromAWindow {
        /// Where the window this handed out lived.
        lent: usize,
    }

    impl Library for FromAWindow {
        fn frame(
            &mut self,
            _media: Digest,
            _tick: i64,
            _description: FrameDescription,
        ) -> Result<Frame, RenderStatus> {
            Err(RenderStatus::MediaAbsent)
        }

        fn look(&mut self, _look: Digest) -> Result<Look, RenderStatus> {
            Err(RenderStatus::LookAbsent)
        }

        fn row(
            &mut self,
            _media: Digest,
            _tick: i64,
            description: FrameDescription,
            row: usize,
        ) -> Result<Frame, RenderStatus> {
            let one = sapstudio_render::graph::row_description(description, row)?;
            let bytes = one.packed_bytes().map_err(RenderStatus::Media)?;
            let window: std::vec::Vec<u8> = (0..bytes)
                .map(|index| u8::try_from((index + row) % 251).unwrap_or(0))
                .collect();
            self.lent = window.as_ptr() as usize;
            Frame::from_owned(one, window).map_err(RenderStatus::Media)
        }
    }

    let description = described(PixelFormat::Rgba8);
    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media: Digest::of(b"a shot"),
            tick: 0,
            description,
        })
        .expect("a node");
    let mut library = FromAWindow { lent: 0 };
    let held = graph.row(source, 2, &mut library).expect("a row");
    assert!(held.is_packed());
    assert_eq!(
        held.packed().expect("bytes").as_ptr() as usize,
        library.lent,
        "the row was copied between the library and the caller"
    );
}

#[test]
fn every_banded_transform_agrees_row_for_row() {
    // Four maps that take horizontals to horizontals, each scanned row by row
    // and compared against the whole render. If `band` said the wrong rows,
    // the row it drew would not be the row the frame holds -- and `picture`
    // makes every byte a function of where it is, so a row taken from one
    // place and compared against another is visible rather than plausible.
    let description = described(PixelFormat::Rgba8);
    let media = Digest::of(b"a shot");
    let half = Rational::new(1, 2).expect("a scale");
    let maps: std::vec::Vec<(&str, [Rational; 4], (Rational, Rational))> = std::vec![
        // A move, which reads one band offset from the row it writes.
        (
            "a move",
            [Rational::ONE, Rational::ZERO, Rational::ZERO, Rational::ONE],
            (Rational::ZERO, Rational::new(1, 5).expect("a move")),
        ),
        // A shrink, which is the case a band is *for*: one destination row
        // reads two source rows and the answer is their weighted mean.
        (
            "a shrink",
            [half, Rational::ZERO, Rational::ZERO, half],
            (Rational::ZERO, Rational::ZERO),
        ),
        // A vertical mirror, where the band walks the picture backwards.
        (
            "a mirror",
            [
                Rational::ONE,
                Rational::ZERO,
                Rational::ZERO,
                Rational::ONE.checked_neg().expect("a mirror"),
            ],
            (Rational::ZERO, Rational::ZERO),
        ),
        // A horizontal shear, which slides each row sideways by an amount
        // that depends on the row -- and still reads one band, because what
        // it moves is `u` and a band is about `v`.
        (
            "a shear",
            [Rational::ONE, half, Rational::ZERO, Rational::ONE],
            (Rational::ZERO, Rational::ZERO),
        ),
    ];
    for bilinear in [false, true] {
        for (name, linear, offset) in &maps {
            let mut graph = Graph::new();
            let source = graph
                .add(Node::Source {
                    media,
                    tick: 0,
                    description,
                })
                .expect("a node");
            let moved = graph
                .add(Node::Transform {
                    input: source,
                    linear: *linear,
                    offset: *offset,
                    anchor: (
                        Rational::new(1, 2).expect("an anchor"),
                        Rational::new(1, 2).expect("an anchor"),
                    ),
                    bilinear,
                })
                .expect("a node");
            let mut library = Pictures::new(std::vec![(media, picture(description, 5))]);
            assert_eq!(graph.row_local(moved), Ok(()), "{name} does not scan");
            agrees(&graph, moved, &mut library);
            // And through something above it, because reaching a fade means
            // walking the transform underneath.
            let faded = graph
                .add(Node::Fade {
                    input: moved,
                    opacity: Rational::new(1, 2).expect("an opacity"),
                })
                .expect("a node");
            assert_eq!(graph.row_local(faded), Ok(()), "{name} under a fade");
            agrees(&graph, faded, &mut library);
        }
    }
}

#[test]
fn a_band_reads_only_the_rows_it_needs() {
    // The whole claim of the milestone, measured rather than asserted: a
    // bilinear move asks its source for two rows per destination row and not
    // for the picture. Five rows out of a five-row source would be exactly
    // what a scan exists to avoid -- and would still produce every correct
    // pixel, which is why this counts instead of comparing.
    let description = described(PixelFormat::Rgba8);
    let media = Digest::of(b"a shot");
    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media,
            tick: 0,
            description,
        })
        .expect("a node");
    let moved = graph
        .add(Node::Transform {
            input: source,
            linear: [Rational::ONE, Rational::ZERO, Rational::ZERO, Rational::ONE],
            offset: (Rational::ZERO, Rational::new(1, 5).expect("a move")),
            anchor: (
                Rational::new(1, 2).expect("an anchor"),
                Rational::new(1, 2).expect("an anchor"),
            ),
            bilinear: true,
        })
        .expect("a node");
    let mut library = Pictures::new(std::vec![(media, picture(description, 5))]);
    graph.row(moved, 2, &mut library).expect("a row");
    assert_eq!(
        library.rows_asked, 2,
        "a bilinear row read {} source rows",
        library.rows_asked
    );
}

#[test]
fn a_shrink_steeper_than_a_band_is_refused_rather_than_holding_a_frame() {
    // `MAX_BAND_ROWS` is not a limit somebody picked: past it a band is the
    // frame, and a scan that holds a frame has bought nothing. So a downscale
    // steep enough to want more rows than that refuses by name -- and the
    // refusal arrives from `band`, before a single source row is fetched.
    let tall = FrameDescription::square(
        Geometry::new(4, 512).expect("a geometry"),
        PixelFormat::Rgba8,
        ColourDescription::srgb_full(),
        None,
        Some(AlphaState::Premultiplied),
    )
    .expect("a description");
    let media = Digest::of(b"a tall shot");
    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media,
            tick: 0,
            description: tall,
        })
        .expect("a node");
    // One hundred and twenty-eight to one: each destination row covers 128
    // source rows, which is twice what a band holds.
    let steep = Rational::new(1, 128).expect("a scale");
    let moved = graph
        .add(Node::Transform {
            input: source,
            linear: [Rational::ONE, Rational::ZERO, Rational::ZERO, steep],
            offset: (Rational::ZERO, Rational::ZERO),
            anchor: (
                Rational::new(1, 2).expect("an anchor"),
                Rational::new(1, 2).expect("an anchor"),
            ),
            bilinear: false,
        })
        .expect("a node");
    let mut library = Pictures::new(std::vec![(media, picture(tall, 3))]);
    // It says it scans, because whether it does is a question about the map
    // and this map takes horizontals to horizontals. What it cannot do is
    // afford this particular row, and that is a different sentence.
    assert_eq!(graph.row_local(moved), Ok(()));
    assert_eq!(
        graph.row(moved, 256, &mut library),
        Err(RenderStatus::BandTooTall)
    );
    assert_eq!(
        library.rows_asked, 0,
        "a source row was fetched before the refusal"
    );
    // And the whole-frame evaluator still does it, which is what a caller
    // that sees this refusal should fall back to.
    graph
        .evaluate(moved, &mut pool(), &mut library)
        .expect("a whole frame");
}

#[test]
fn every_generator_agrees_row_for_row() {
    // These three refused with `NoRowForm` until this milestone, and the
    // refusal was always a statement about the build rather than about the
    // operation: a test pattern's colour is a function of position, and so is
    // a glyph's coverage. So the property holds for them exactly as it does
    // for a source.
    //
    // What each is placed against is the whole picture, which is the part that
    // could have been got wrong: bars are eighths of the *width*, a
    // checkerboard is counted from the *top*, and an em is a fraction of the
    // *height*. Drawn into a frame one row high, each would be a different
    // picture rather than a row of this one.
    let description = described(PixelFormat::Rgb8);
    let mut graph = Graph::new();
    let mut library = Pictures::new(std::vec::Vec::new());

    for pattern in [
        sapstudio_media::TestPattern::Bars,
        sapstudio_media::TestPattern::Ramp,
        sapstudio_media::TestPattern::Offline,
        sapstudio_media::TestPattern::Checkerboard { square: 2 },
        sapstudio_media::TestPattern::Flat { value: 77 },
    ] {
        let id = graph
            .add(Node::Pattern {
                pattern,
                description,
            })
            .expect("a node");
        agrees(&graph, id, &mut library);
    }

    // A card, and a legend over a pattern -- which is what an offline slate
    // is. Both draw letters over what is behind them, so both need a
    // description that can carry coverage -- and one **large enough for the
    // letters to fit**. Seven by five is what the rest of this file uses and
    // it is far too small for a caption, so `lettered` hands the picture back
    // untouched and the type is never drawn at all. A control found that: the
    // legend's layout could be got wrong and nothing noticed. A hundred and
    // twenty by sixty is what the legend's own tests use, and it is the
    // smallest this file has that the words fit in.
    let lettered = FrameDescription::square(
        Geometry::new(120, 60).expect("a geometry"),
        PixelFormat::Rgba8,
        ColourDescription::srgb_full(),
        None,
        Some(AlphaState::Premultiplied),
    )
    .expect("a description");
    let card = graph
        .add(Node::Type {
            description: lettered,
            lines: std::vec!["SAPSTUDIO".into(), "take two".into()],
            size: Rational::new(1, 4).expect("a size"),
            across: Rational::new(1, 2).expect("a place"),
            down: Rational::new(1, 2).expect("a place"),
            alignment: sapstudio_render::font::Alignment::Centre,
            ink: [Rational::ONE, Rational::ONE, Rational::ONE],
        })
        .expect("a node");
    agrees(&graph, card, &mut library);

    let slate = graph
        .add(Node::Pattern {
            pattern: sapstudio_media::TestPattern::Offline,
            description: lettered,
        })
        .expect("a node");
    let legend = graph
        .add(Node::Legend {
            input: slate,
            text: "media offline".into(),
            brief: "0000".into(),
        })
        .expect("a node");
    // The letters really are drawn, which is what makes the rows worth
    // comparing: a legend too small for its caption is the bare pattern.
    let whole = graph
        .evaluate(legend, &mut pool(), &mut library)
        .expect("a frame");
    let bare = graph
        .evaluate(slate, &mut pool(), &mut library)
        .expect("a frame");
    assert_ne!(whole, bare, "the legend drew no letters");
    agrees(&graph, legend, &mut library);
    // And every one of them says it can be scanned before it is.
    assert_eq!(graph.row_local(legend), Ok(()));
    assert_eq!(graph.row_local(card), Ok(()));
}

#[test]
fn a_subsampled_format_has_no_row() {
    // A sharper refusal than the transform's, and a truer one: one chroma row
    // of a 4:2:0 frame serves two luma rows, so a luma row is not independent
    // of its neighbour. That is a fact about the format rather than about this
    // build, so it is `NotRowLocal`.
    let planar = FrameDescription::new(
        Geometry::new(8, 4).expect("a geometry"),
        PixelFormat::Yuv420p8,
        ColourDescription::new(
            sapstudio_media::colour::Primaries::Bt709,
            sapstudio_media::colour::TransferFunction::Srgb,
            sapstudio_media::colour::MatrixCoefficients::Bt709,
            sapstudio_media::colour::Range::Full,
        ),
        Some(sapstudio_media::colour::ChromaSiting::Centre),
        None,
        Rational::ONE,
    )
    .expect("a description");
    let mut graph = Graph::new();
    let blank = graph
        .add(Node::Blank {
            description: planar,
        })
        .expect("a node");
    let mut library = Pictures::new(std::vec::Vec::new());
    assert_eq!(
        graph.row(blank, 0, &mut library),
        Err(RenderStatus::NotRowLocal)
    );
}

#[test]
fn a_library_without_a_row_form_says_so_rather_than_loading_a_frame() {
    // The default refuses instead of fetching the whole frame and slicing it,
    // and that is the decision worth having a test for: a default that quietly
    // loaded eight megabytes to hand back six thousand bytes would make a
    // row-at-a-time renderer look like it was working while doing exactly the
    // thing it exists to avoid.
    struct WholeFramesOnly(Frame);

    impl Library for WholeFramesOnly {
        fn frame(
            &mut self,
            _media: Digest,
            _tick: i64,
            _description: FrameDescription,
        ) -> Result<Frame, RenderStatus> {
            Ok(self.0.clone())
        }

        fn look(&mut self, _look: Digest) -> Result<Look, RenderStatus> {
            Err(RenderStatus::LookAbsent)
        }
    }

    let description = described(PixelFormat::Rgb8);
    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media: Digest::of(b"a shot"),
            tick: 0,
            description,
        })
        .expect("a node");
    let mut library = WholeFramesOnly(picture(description, 1));
    assert_eq!(
        graph.row(source, 0, &mut library),
        Err(RenderStatus::NoRowForm)
    );
    // And the whole-frame path still works through the same library.
    graph
        .evaluate(source, &mut pool(), &mut library)
        .expect("a whole frame");
}

#[test]
fn a_source_that_answers_the_wrong_question_is_refused_in_both_forms() {
    let asked = described(PixelFormat::Rgb8);
    let other = FrameDescription::square(
        Geometry::new(WIDE, TALL).expect("a geometry"),
        PixelFormat::Gray8,
        ColourDescription::srgb_full(),
        None,
        None,
    )
    .expect("a description");
    let media = Digest::of(b"a shot");
    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media,
            tick: 0,
            description: asked,
        })
        .expect("a node");
    let mut library = Pictures::new(std::vec![(media, picture(other, 9))]);
    assert_eq!(
        graph.row(source, 0, &mut library),
        Err(RenderStatus::SourceDescriptionMismatch)
    );
    assert_eq!(
        graph.evaluate(source, &mut pool(), &mut library),
        Err(RenderStatus::SourceDescriptionMismatch)
    );
}

#[test]
fn a_row_that_is_not_the_row_that_was_asked_for_is_refused() {
    // The library's own check passes -- it holds the right picture and was
    // asked for the right description -- and it hands back a row described
    // some other way. Only the graph's check catches that, which is why it is
    // there as well as in the library.
    let description = described(PixelFormat::Rgb8);
    let media = Digest::of(b"a shot");
    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media,
            tick: 0,
            description,
        })
        .expect("a node");
    let mut library = Pictures::new(std::vec![(media, picture(description, 3))]);
    library.lies_in_rows = true;
    assert_eq!(
        graph.row(source, 0, &mut library),
        Err(RenderStatus::SourceDescriptionMismatch)
    );
    // And with the library honest it is the row it should be.
    library.lies_in_rows = false;
    agrees(&graph, source, &mut library);
}

#[test]
fn a_mask_over_a_turn_is_refused_before_a_row_is_computed() {
    // A mask is placed against the whole frame, so it has to know how tall
    // that frame is -- and above a turn nothing here can know. The
    // geometry is asked for *before* the source's row, so this refuses
    // without doing the work rather than after.
    let description = described(PixelFormat::Rgba8);
    let media = Digest::of(b"a shot");
    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media,
            tick: 0,
            description,
        })
        .expect("a node");
    let moved = graph
        .add(Node::Transform {
            input: source,
            linear: turned(),
            offset: (Rational::ZERO, Rational::ZERO),
            anchor: (
                Rational::new(1, 2).expect("an anchor"),
                Rational::new(1, 2).expect("an anchor"),
            ),
            bilinear: false,
        })
        .expect("a node");
    let masked = graph
        .add(Node::Mask {
            input: moved,
            corners: std::vec![
                (Rational::ZERO, Rational::ZERO),
                (Rational::ONE, Rational::ZERO),
                (Rational::ONE, Rational::ONE),
            ],
            inverted: false,
        })
        .expect("a node");
    let mut library = Pictures::new(std::vec![(media, picture(description, 2))]);
    assert_eq!(
        graph.row(masked, 0, &mut library),
        Err(RenderStatus::NotRowLocal)
    );
    assert_eq!(
        library.rows_asked, 0,
        "a row was computed before the refusal"
    );
    // The geometry walk itself does *not* refuse a turn, and that is
    // deliberate: a transform resamples into its source's own description, so
    // it changes what the picture looks like rather than how large it is. The
    // refusal above is `row`'s, one step later, which is the only one there
    // is now.
    let (width, height) = (
        masked_extent(&graph, masked).0,
        masked_extent(&graph, masked).1,
    );
    assert_eq!((width, height), (WIDE, TALL));
}

/// The geometry a node produces, read back out of the graph the way
/// `extent_of` does.
///
/// Written here rather than exposed, because a test that wanted to check the
/// private walk should say so rather than widen the crate's surface for it.
fn masked_extent(graph: &Graph, id: NodeId) -> (u32, u32) {
    let node = graph.node(id).expect("a node");
    match node {
        Node::Mask { input, .. } | Node::Wipe { input, .. } | Node::Fade { input, .. } => {
            masked_extent(graph, *input)
        }
        Node::Transform { input, .. } => masked_extent(graph, *input),
        Node::Source { description, .. } | Node::Blank { description } => (
            description.geometry().width(),
            description.geometry().height(),
        ),
        other => panic!("no geometry for {other:?}"),
    }
}

#[test]
fn asking_first_gives_the_same_answer_as_asking_at_the_row() {
    // `row_local` exists so a caller can find out *before* it commits, and the
    // only thing that makes it worth having is that it agrees with `row`. Two
    // answers to one question would be worse than no question: a caller that
    // asked, was told yes, and was refused at row nought would have paid for
    // the question and got nothing.
    //
    // So every case is checked twice here, against the same graph, and the
    // statuses have to match — not merely both be errors.
    let description = described(PixelFormat::Rgba8);
    let media = Digest::of(b"a shot");
    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media,
            tick: 0,
            description,
        })
        .expect("a node");
    let mut library = Pictures::new(std::vec![(media, picture(description, 7))]);
    // A source scans, and so does a chain of decorations over it.
    assert_eq!(graph.row_local(source), Ok(()));
    let faded = graph
        .add(Node::Fade {
            input: source,
            opacity: Rational::new(1, 3).expect("an opacity"),
        })
        .expect("a node");
    let masked = graph
        .add(Node::Mask {
            input: faded,
            corners: std::vec![
                (Rational::ZERO, Rational::ZERO),
                (Rational::ONE, Rational::ZERO),
                (Rational::ONE, Rational::ONE),
            ],
            inverted: false,
        })
        .expect("a node");
    assert_eq!(graph.row_local(masked), Ok(()));
    assert!(graph.row(masked, 0, &mut library).is_ok());
    // A turn does not, and neither does anything reached through one.
    let moved = graph
        .add(Node::Transform {
            input: masked,
            linear: turned(),
            offset: (Rational::new(1, 10).expect("an offset"), Rational::ZERO),
            anchor: (
                Rational::new(1, 2).expect("an anchor"),
                Rational::new(1, 2).expect("an anchor"),
            ),
            bilinear: false,
        })
        .expect("a node");
    let over_a_turn = graph
        .add(Node::Fade {
            input: moved,
            opacity: Rational::ONE,
        })
        .expect("a node");
    assert_eq!(graph.row_local(moved), Err(RenderStatus::NotRowLocal));
    assert_eq!(
        graph.row_local(over_a_turn),
        Err(RenderStatus::NotRowLocal),
        "the walk stopped at the fade instead of going through it"
    );
    assert_eq!(
        graph.row(over_a_turn, 0, &mut library),
        Err(RenderStatus::NotRowLocal)
    );

    // A generator scans now, and says so before it is asked -- which is the
    // same agreement, in the direction that used to be a refusal.
    let pattern = graph
        .add(Node::Pattern {
            pattern: sapstudio_media::TestPattern::Checkerboard { square: 2 },
            description,
        })
        .expect("a node");
    assert_eq!(graph.row_local(pattern), Ok(()));
    assert!(graph.row(pattern, 0, &mut library).is_ok());
}

#[test]
fn asking_first_looks_at_both_layers_of_a_composite() {
    // Both, and in a graph where only one of them is the problem — twice, once
    // each way round. A walk that checked the top layer only would pass the
    // first of these and fail the second, and a stack is built bottom first,
    // so the layer a planner puts the framing on is whichever one the cutter
    // happened to frame.
    let description = described(PixelFormat::Rgba8);
    let media = Digest::of(b"a shot");
    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media,
            tick: 0,
            description,
        })
        .expect("a node");
    let moved = graph
        .add(Node::Transform {
            input: source,
            linear: turned(),
            offset: (Rational::ZERO, Rational::ZERO),
            anchor: (Rational::ZERO, Rational::ZERO),
            bilinear: false,
        })
        .expect("a node");
    let plain = graph.add(Node::Blank { description }).expect("a node");
    let framing_on_top = graph
        .add(Node::Over {
            layers: [plain, moved],
        })
        .expect("a node");
    let framing_beneath = graph
        .add(Node::Over {
            layers: [moved, plain],
        })
        .expect("a node");
    assert_eq!(
        graph.row_local(framing_on_top),
        Err(RenderStatus::NotRowLocal)
    );
    assert_eq!(
        graph.row_local(framing_beneath),
        Err(RenderStatus::NotRowLocal),
        "the lower layer was not walked"
    );
    assert_eq!(graph.row_local(plain), Ok(()));
}

#[test]
fn asking_first_refuses_a_description_that_has_no_rows() {
    // The subsampling refusal, arriving at the question rather than at the
    // first row. Worth its own case because the description is carried by the
    // node rather than reached through one: a walk that only looked at node
    // *kinds* would say yes to every one of these.
    let planar = FrameDescription::new(
        Geometry::new(8, 4).expect("a geometry"),
        PixelFormat::Yuv420p8,
        ColourDescription::new(
            sapstudio_media::colour::Primaries::Bt709,
            sapstudio_media::colour::TransferFunction::Srgb,
            sapstudio_media::colour::MatrixCoefficients::Bt709,
            sapstudio_media::colour::Range::Full,
        ),
        Some(sapstudio_media::colour::ChromaSiting::Centre),
        None,
        Rational::ONE,
    )
    .expect("a description");
    let straight = described(PixelFormat::Rgb8);
    let media = Digest::of(b"a shot");
    let mut graph = Graph::new();
    for node in [
        Node::Blank {
            description: planar,
        },
        Node::Empty {
            description: planar,
        },
        Node::Source {
            media,
            tick: 0,
            description: planar,
        },
    ] {
        let id = graph.add(node).expect("a node");
        assert_eq!(graph.row_local(id), Err(RenderStatus::NotRowLocal));
    }
    // And a conversion is refused for the target it converts *into*, not only
    // for what it reads: a chain that ends in 4:2:0 cannot be scanned however
    // row-local its source is.
    let source = graph
        .add(Node::Source {
            media,
            tick: 0,
            description: straight,
        })
        .expect("a node");
    assert_eq!(graph.row_local(source), Ok(()));
    let converted = graph
        .add(Node::Convert {
            input: source,
            target: planar,
        })
        .expect("a node");
    assert_eq!(graph.row_local(converted), Err(RenderStatus::NotRowLocal));
}
