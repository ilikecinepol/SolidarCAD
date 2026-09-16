# AGENTS.md

SolidarCAD — cross-platform parametric CAD (C++20, Qt 6, OpenCASCADE 8.0.1, CMake + vcpkg). Russian UI and Russian team communication. Deep safety rules live in `.opencode/skills/*` (load them for the relevant subsystem) — this file is the quick-ramp + gotchas.

## Build & test (exact, non-obvious)

The reproducible gate is the **`ci` preset** (Release, tests, pinned vcpkg OCCT, Qt 6.8.3). It needs the MSVC developer environment and vcpkg variables first:

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set VCPKG_ROOT=<repo>\vcpkg
set VCPKG_DEFAULT_TRIPLET=ci-x64-windows
set CMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64
cmake --preset ci
cmake --build --preset ci
ctest --preset ci
```

- `dev` preset = Debug + tests, **no** vcpkg toolchain (uses a local OCCT build-tree fallback). `.opencode/scripts/verify-and-run.ps1` runs configure→build→ctest→launch-smoke on `dev`; the verifier agent only reports failures, never edits code.
- Single test: `ctest --test-dir build/ci -R <name> -V`, or run `build/ci/tests/<name>.exe` directly with Qt `bin` + OCCT `bin` on `PATH` and `QT_QPA_PLATFORM=offscreen`.
- Focused runs by label: `ctest --preset ci -L unit|regression|ui-smoke|compliance|topology`.

## Gotchas that will waste your time

- **After editing a header, rebuild with `--clean-first`.** Ninja sometimes does not recompile dependent test objects whose header changed → ABI mismatch → tests crash with exit `0xc0000409` (stack buffer overrun). If tests crash spuriously after a header edit: `cmake --build --preset ci --clean-first`.
- **Tests use the repo's `CHECK` macro, not `assert`.** The `ci` preset is Release (`NDEBUG`); `tests/CMakeLists.txt` adds `/UNDEBUG`, but new tests must use `CHECK` (returns non-zero + prints) so they stay effective regardless.
- **OCCT via vcpkg is slow (~25 min) first build**, then restored from the binary cache.
- **`download.qt.io` is unreachable from this network**; Qt was installed via `aqtinstall` + a mirror and already lives at `C:\Qt\6.8.3\msvc2022_64`.

## Architecture (things not obvious from filenames)

- `solidar_model` is the UI-free parametric core — `Document`/`Body`/`Feature` history, Sketch + solver. It **does link OCCT** (B-Rep); the README/`docs/architecture.md` claim "no OCCT dependency" is **stale**. It is Qt-free.
- B-Rep is rebuilt by `Document::recompute()` and **never serialized**; `.solidar` stores only IDs, parameters, placements and topology references.
- Module map: `solidar_model` (core) → `solidar_project` (`.solidar` v1/v2 + STL) → `solidar_sketch_ui`/`solidar_drawing_ui`/`solidar_viewport3d` → `solidar_editor` (MainWindow shell) + `solidar_home`; `solidar_theme` (AppSettings + ThemeManager + SettingsWidget); `solidar` (exe) imports only Home + Editor.
- Part Design tools use a session/controller layer (`ToolSession` + `PartDesignToolController`); teardown order (controller `cancelActive()` + each session `cancel()` **before** replacing `Document`) is load-bearing.
- `Viewport` is one large `QOpenGLWidget` (`paintGL` monolith). `ViewCube` and `WorldGrid` are separate components — do not grow `paintGL` further.

## Theme / styling (do not hardcode)

- No per-widget hex `setStyleSheet`. Colors flow from `ThemeColors` (light/dark) through `ThemeManager` (Fusion base style + `QPalette` + generated global QSS). For special cases use semantic `uiRole`/`stateRole` properties, not color literals.
- `AppSettings` (QSettings, key `ui/theme`, values `system|light|dark`, single instance) is the source of truth; `ThemeManager` is presentation. Settings UI uses Apply/Cancel — the combo is a temporary selection, not the saved value.

## Git

- Commit and push only when explicitly asked.
- Branch naming: `main`, `codex/*`, `feature/*`.
- `build/`, `vcpkg/`, `vcpkg_installed/` are gitignored — never commit them.
