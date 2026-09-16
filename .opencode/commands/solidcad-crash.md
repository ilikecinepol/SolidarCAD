---
description: Investigate and fix a SolidarCAD crash with parallel Sketcher/Qt/OCCT audits as relevant.
agent: solidcad-lead
---

Investigate and eliminate this SolidarCAD crash:

$ARGUMENTS

Prioritize crash safety. Launch `solidcad-explorer`, `solidcad-sketcher-crash-hunter`, and all relevant Qt/OCCT auditors in parallel, plus `solidcad-regression-designer`. Require a deterministic regression test before considering the fix complete. Then implement, review and run the local final gate.
