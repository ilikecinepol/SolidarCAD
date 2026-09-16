---
name: git-hygiene
description: Git safety rules for AI-assisted SolidarCAD development.
---

# Git hygiene

Allowed by default:
- status;
- diff;
- log;
- branch inspection;
- rev-parse.

Do not:
- force push;
- reset --hard;
- clean untracked files;
- restore unrelated user changes;
- rebase or merge unless explicitly requested;
- commit unless the user asks for a commit.

When reporting a task, include the resulting changed-file list and whether the worktree contained unrelated pre-existing changes.
