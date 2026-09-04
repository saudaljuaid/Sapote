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

Do not replace the platform camera broker with the shell window when updating
this import. Reapply the same mechanical namespace conversion, then run the
kernel strict-warning build and the Phipia shell self-tests.
