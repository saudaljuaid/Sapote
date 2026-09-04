# Phipia shell upstream

The Phipia desktop shell is imported from
`https://github.com/saudaljuaid/win10-sap` at commit
`8b1c170f187a` from its `New-Phipia-Ui` branch.

The imported set is the 43 kernel-facing files named by upstream
`docs/INTEGRATION.md`. Forty files retain their upstream path and contents.
The three Camera-window files are installed as `phipia_camera.*` and
`phipia_camera_glyphs.h` because Phipia already has a platform camera frame
broker at `camera.*`. Only the Camera-window identifiers and include paths are
namespaced; its generated artwork and behavior otherwise remain upstream.

The repository and public C include namespace remain `Sapote` and `sapote/`
for source compatibility. User-visible product and shell identity is Phipia.

The global Phipia identity assets also come from that pinned shell commit. The
logo is copied byte-for-byte from `assets/logo/phipia.png` to
`assets/phipia/logo.png` (SHA-256
`6a07abe324c2d80aa0f1dd3a318c103c3b6a81fef1f72d5f4808d589626b1e88`).
The default wallpaper is derived deterministically from
`assets/wallpaper/phipia-lake.jpg` (source SHA-256
`b98c9301b1bf86b7d3ee9fa943644cc0ed5052b25822082075abc628b3e176f6`):
it is center-cropped to 4:3 and bilinear-resized to 1024x768 as
`assets/phipia/wallpaper.png` (SHA-256
`ce1df11fa3a5575b55b47a2fba7216216b21ce09c53824d85610515848184981`).
The normal asset generators convert those two files into the boot/runtime
formats. Existing optional wallpapers remain selectable; the Phipia lake is
the default frame.

The Media Editor icon is imported from the private SapStudio repository at
commit `f205f90c6842ecf1b0e3e6f8bb8b41f8136ebf26` and retained at
`assets/phipia/media-editor.png` with SHA-256
`c5d706b274132b5fcaf0bb016d0da56ddd1dc54b417709364874ad1a58611eb5`.
`src/kernel/taskbar_art.h` was regenerated from the Phipia application artwork
with this image in the `editor` slot.

The complete portable editor source in `userspace/sapstudio/` is synchronized
from that repository's `engineering-foundation` branch at commit
`3717f02d35fede9a37d62d37ce05b0eb47ea1a95`. The mirrored application keeps
its stable crate, file-format, and ABI identifiers for compatibility while the
desktop presents it as **Media Editor**. This snapshot includes nested
sequences, bounded row and frame-window rendering, storage-backed media,
streamed reel and audio export, captions, VTT sidecars, and caption burn-in.
The Phipia window in `src/kernel/editor.c` is the interactive platform surface;
`src/kernel/ui.c` owns the durable FAT32 adapter until the native image can use
the full framebuffer, input, storage, and audio seams directly.

Do not replace the platform camera broker with the shell window when updating
this import. Reapply the same mechanical namespace conversion, then run the
kernel strict-warning build, the Phipia shell self-tests, and the mirrored
Media Editor verification suite.
