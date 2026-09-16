---
name: safe-change
description: Small-diff and worktree-safety rules for AI changes in SolidarCAD.
---

# Safe change policy

- Inspect `git status` before editing.
- Preserve unrelated user changes.
- Do not use destructive cleanup/reset commands.
- Do not rewrite large neighboring modules to solve a local bug.
- Separate required behavior changes from optional cleanup.
- Keep the diff explainable.
- Do not push unless the user explicitly requests it.
- Never report success based only on code inspection: at minimum run relevant automated checks.
- A final task completed through the AI-team pipeline requires independent review plus the local build/test/launch gate.
