---
name: build-and-run
description: Final local Windows configure/build/test/launch procedure for SolidarCAD using repository CMake presets.
---

# Final local verification

Use the repository-local PowerShell script:

`powershell -NoProfile -ExecutionPolicy Bypass -File .opencode/scripts/verify-and-run.ps1`

The script:
1. configures the `dev` CMake preset;
2. builds the `dev` preset;
3. runs the `dev` CTest preset;
4. finds `build/dev/src/solidar.exe`;
5. launches the GUI;
6. checks that the process remains alive for a short smoke window.

The verifier must never edit code after failure. It reports the failed gate to the lead agent.
