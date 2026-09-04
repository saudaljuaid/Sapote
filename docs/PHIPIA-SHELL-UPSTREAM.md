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

The Media Editor icon is imported from the private SapStudio repository at
commit `f205f90c6842ecf1b0e3e6f8bb8b41f8136ebf26` and retained at
`assets/phipia/media-editor.png` with SHA-256
`c5d706b274132b5fcaf0bb016d0da56ddd1dc54b417709364874ad1a58611eb5`.
That repository currently contains the icon and license only; it has no newer
editor implementation to import. `src/kernel/taskbar_art.h` was regenerated
from the Phipia application artwork with this image in the `editor` slot.

Do not replace the platform camera broker with the shell window when updating
this import. Reapply the same mechanical namespace conversion, then run the
kernel strict-warning build and the Phipia shell self-tests.
