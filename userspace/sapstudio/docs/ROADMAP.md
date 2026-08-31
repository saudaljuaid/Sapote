<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Roadmap

This file tracks product priorities. Implemented details belong in
[`ARCHITECTURE.md`](ARCHITECTURE.md) and the changelog.

## Current baseline

The editor core already includes rational time, a project and edit model,
undo/redo, versioned persistence, frame and audio types, a render graph,
compositing, colour conversion, LUTs, scopes, transitions, masks, titles,
retiming, keyframes, interchange, and a freestanding Sapote image.

Redwood exposes a smaller integrated surface: BMP import, clip selection and
trim, project save, and BMP export.

## Active priorities

### Interactive editor

- Connect the project model to source, viewer, timeline, inspector, and mixer
  panels.
- Add deterministic layout, keyboard focus, pointer capture, scrolling, and
  damage tracking.
- Route edits through the existing undo journal.

Completion means the visible timeline and saved model remain synchronized
through editing and recovery.

### Media pipeline

- Add image-sequence and uncompressed video readers first.
- Connect decode, cache, graph evaluation, compositing, and export.
- Add compressed codecs only through reviewed, vendored dependencies.
- Preserve explicit colour metadata from input through output.

Completion means a project renders a reproducible image sequence that reopens
with matching frame descriptions.

### Audio

- Connect decoded samples to the mixer and meters.
- Add Sapote audio submission, clocking, and underrun reporting.
- Keep allocation and locks outside the real-time path.

Completion means synchronized picture and sound play through Sapote and offline
export matches the same timeline.

### Performance and hardware

- Add saved floating-point and SIMD state to Sapote userspace.
- Measure hot loops before introducing optimized leaves.
- Add userspace threads and multicore graph execution while keeping output
  independent of scheduling order.
- Introduce GPU acceleration only after the software path is complete.

## Release criteria

The first full SapStudio release needs:

- native launch and clean shutdown;
- project creation, import, editing, save, recovery, and export;
- synchronized playback with clear underrun behavior;
- reproducible target images and pinned dependency sources;
- parser fuzzing and reference renders for supported formats;
- documented resource limits and unsupported formats.

## Outside the current release

Live collaboration, cloud services, plugin hosting, scripting, broad codec
coverage, GPU effects, and distributed rendering are outside the first
release. Their absence should not complicate the core editor.
