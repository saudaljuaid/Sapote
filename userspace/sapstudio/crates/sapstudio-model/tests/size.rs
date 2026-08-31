// SPDX-License-Identifier: GPL-3.0-only
//! What the model's central types cost, kept by something that runs.
//!
//! `Item` carries a `clippy::large_enum_variant` expectation and the argument
//! for it rests on a number: a clip is 520 bytes. `Edit` carried one too until
//! a lift gave it a *second* variant holding an item — at which point the lint
//! stopped firing, the `expect` said so, and the exemption went. The ceiling
//! below stayed, because the ceiling is the half that was doing the work.
//!
//! A clip is 520 bytes and an edit is 544. A number that lives only in a doc comment is a number that
//! goes stale, and these would go stale silently — nothing else in the suite
//! would notice a field that doubled the cost of every item on every track,
//! or every entry in the history.
//!
//! So these are bounds rather than equalities. `repr(Rust)` layout is
//! deliberately unspecified and a compiler is free to pack these differently
//! tomorrow; what this crate is entitled to require is a *ceiling*, and a
//! ceiling is what a platform with seventy-six kibibytes of address space
//! actually cares about.

use sapstudio_model::journal::MAX_HISTORY;
use sapstudio_model::{Clip, Edit, Item, Mask, Motion, Transform};

/// What one item on a track may cost.
///
/// The current item is about 520 bytes. This ceiling tolerates compiler layout
/// differences while still catching a material size increase. Any increase
/// should be checked against the freestanding image measurements in the
/// [platform contract](../../../docs/PLATFORM_CONTRACT.md).
const MAX_ITEM_BYTES: usize = 576;

/// What one edit in the history may cost.
///
/// The largest variants are `InsertItem` and `DropItem`, which each carry a
/// whole item, so this tracks [`MAX_ITEM_BYTES`] and sits the same distance
/// above what it is.
const MAX_EDIT_BYTES: usize = 592;

#[test]
fn an_item_costs_no_more_than_the_argument_for_its_shape_assumes() {
    let item = core::mem::size_of::<Item>();
    assert!(
        item <= MAX_ITEM_BYTES,
        "an item is {item} bytes, past the {MAX_ITEM_BYTES} the enum's \
         documented exemption argues from -- either shrink it or rewrite the \
         argument, but do not leave the two disagreeing"
    );
    assert_eq!(
        core::mem::size_of::<Clip>(),
        item,
        "the clip variant is the whole of an item's cost, which is the premise \
         the exemption rests on"
    );
}

#[test]
fn the_decorations_are_what_a_clip_is_made_of() {
    // Named individually because the exemption's argument is that no *one* of
    // these can be made smaller without giving up exact arithmetic -- four
    // rationals are four rationals -- and a future reader deserves to see
    // which one grew rather than only that something did.
    let decorations = core::mem::size_of::<Option<Transform>>()
        + core::mem::size_of::<Option<Motion>>() * 2
        + core::mem::size_of::<Option<Mask>>();
    assert!(
        decorations < core::mem::size_of::<Clip>(),
        "the decorations are {decorations} bytes of a clip's {}, and a clip \
         also holds its media, its source position and its length",
        core::mem::size_of::<Clip>()
    );
    assert!(
        decorations * 2 > core::mem::size_of::<Clip>(),
        "and they are the majority of it, which is why boxing them is the \
         remedy clippy suggests and R-5.2 refuses"
    );
}

#[test]
fn an_edit_costs_no_more_than_the_argument_for_its_shape_assumes() {
    let edit = core::mem::size_of::<Edit>();
    assert!(
        edit <= MAX_EDIT_BYTES,
        "an edit is {edit} bytes, past the {MAX_EDIT_BYTES} this crate's \
         history budget argues from"
    );
    assert!(
        edit >= core::mem::size_of::<Item>(),
        "and it is at least an item, because two of its variants carry one -- \
         which is why this number follows the one above"
    );
}

#[test]
fn the_history_is_bounded_by_the_platform_before_it_is_bounded_by_its_constant() {
    // MAX_HISTORY is a policy ceiling, and on a machine with room it is the
    // one that bites. On Sapote it is not, and the arithmetic here is the
    // reason: a program gets nineteen mapped pages, and one entry is a pair.
    const MAPPED_BYTES: usize = 19 * 4096;
    let pair = core::mem::size_of::<Edit>() * 2;
    let affordable = MAPPED_BYTES / pair;
    assert!(
        affordable < MAX_HISTORY,
        "the constant would be reachable in {MAPPED_BYTES} bytes, which would \
         make this test's premise wrong rather than merely its number stale"
    );
    // Not an assertion about the exact figure -- the project itself, the
    // media library and the render caches are in those pages too, so the true
    // depth is well under this. What is asserted is the order of the two
    // limits: the allocator's typed refusal arrives first, and it is
    // `ModelStatus::OutOfMemory` rather than `CapacityExhausted`. A reader of
    // `MAX_HISTORY` should not take four thousand as a promise.
    assert!(
        affordable < 200,
        "a hundred and sixteen pairs of edits is the whole of the address \
         space, and the real figure is {affordable}"
    );
}
