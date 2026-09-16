# SolidarCAD AI Team v2

This package replaces the existing `.opencode/agents` files.

## Key policy changes

- Lead has a mandatory Scope Gate.
- Local Qwen agents perform exploration, Qt/theme audit, regression design, review and build diagnostics.
- DeepSeek is reserved for Lead, Implementer and specialist Sketcher/OCCT escalations.
- Independent product features must not be merged into one implementation contract.
- One implementation pass + at most one correction pass.
- Second blocked review stops the loop and reports to the user.
- Scope creep is a REVIEW BLOCKER.
- Final mechanical gate is `solidcad-local-verifier` only.
- `solidcad-rebuild-runner` is manual/standalone and is not called by Lead.
- No push/merge/rebase/reset/clean unless explicitly requested by the user.

## Model routing

DeepSeek / inherited parent model:
- solidcad-lead
- solidcad-implementer
- solidcad-sketcher-crash-hunter
- solidcad-occt-auditor

Local `ollama/qwen3-coder:30b`:
- solidcad-explorer
- solidcad-regression-designer
- solidcad-qt-auditor
- solidcad-theme-auditor
- solidcad-reviewer
- solidcad-build-engineer

Local `ollama/qwen3:8b`:
- solidcad-local-verifier
- solidcad-rebuild-runner

## Installation

Copy all `solidcad-*.md` files into:

`.opencode/agents/`

Replace the old files with the same names.

The new `solidcad-theme-auditor.md` is additional.

Restart OpenCode after replacement.
