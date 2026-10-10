# MVP release checklist

This checklist is the publication gate for the Windows x64 and Ubuntu 24.04
x86_64 0.1.0 MVP Preview.
Record the tested commit and machine configuration in the release notes.
Check an item only for the exact release-candidate commit. A successful local
run from another revision is useful evidence but does not complete the tagged
release gate. The current project stage is tracked in `docs/roadmap.md`.
The latest pre-release evidence is recorded in
`docs/reports/2026-10-04-mvp-release-readiness.md`.

## Repository

- [ ] Release from a clean worktree whose commit is present on `main`.
- [ ] Set the release date in `CHANGELOG.md` and tag the commit as `v0.1.0`.
- [ ] Confirm that the GitHub Actions Windows and Ubuntu jobs pass for the tag.
- [ ] Run the compliance tests and confirm that version `0.1.0` agrees across
      CMake, `vcpkg.json`, the generated Home screen and the archive name.

## Automated gate

- [ ] Configure and build the pinned `ci` preset from a clean checkout.
- [ ] Run `ctest --preset ci` and require every registered test to pass.
- [ ] Configure, build and install the `release` preset.
- [ ] Create the portable archive with CPack, then run
      `python scripts/finalize_release.py --root . --build-dir build/release`.
- [ ] Create the Ubuntu package with CPack and retain the CI-validated `.deb`,
      `.deb.sha256` and `.spdx.json` files as one release set.
- [ ] Retain the validated ZIP, generated `.zip.sha256` and release-profile
      `.spdx.json` as one release set.
- [ ] Run the compliance test against the final source tree.

## Manual GPU gate

- [ ] Complete the Viewport Rendering, Viewport & Tool UX Polish and Part
      Design Stability recipes in `docs/testing.md` on the target Windows GPU.
- [ ] Check 100%, 125%, 150% and 200% display scaling.
- [ ] Exercise Apply, Cancel, invalid-input recovery and reopening each Part
      Design tool for at least 20 sequential tool sessions.
- [ ] Save, close, reopen, edit history, scrub the timeline and Undo without
      restarting the application.

## Clean-machine package gate

- [ ] Use a Windows x64 VM or machine without Qt, OCCT, Visual Studio or the
      repository checkout.
- [ ] Download the exact release ZIP, verify its published SHA-256 and extract
      it to a normal user-writable directory.
- [ ] Launch `bin/solidar.exe` directly from the extracted archive.
- [ ] Create and edit a Sketch -> Extrude -> Fillet/Chamfer model.
- [ ] Save it as `.solidar`, restart the application and continue editing it.
- [ ] Export STEP and STL and confirm that both files can be read independently.
- [ ] On an Ubuntu 24.04 x86_64 VM without Qt or OCCT, install the package with
      `sudo apt install ./solidarcad_0.1.0-1_amd64.deb`.
- [ ] Launch from the desktop menu and with `solidar`, then open a `.solidar`
      file from the file manager and repeat the modelling/export smoke test.

## Publication

- [ ] Publish the ZIP and Debian package with their SHA-256, SBOM and release
      notes together.
- [ ] Label the release **Windows x64 / Ubuntu 24.04 x64 MVP Preview** and state
      that it is unsigned and limited to single-part modelling.
- [ ] Include the known limitations from `CHANGELOG.md`.
- [ ] Keep the Debian package described as Ubuntu 24.04 x86_64-only until its
      manual GPU and clean-machine binary gates are completed.

Do not set the release date, create `v0.1.0`, or publish artifacts while either
manual gate above remains incomplete.
