# SolidarCAD AI Team for OpenCode

Project-local OpenCode configuration for AI-assisted development of SolidarCAD.

## Core idea

- The **primary session** should use your strong coding model (for example DeepSeek V4 Pro).
- `solidcad-lead` is the primary orchestrator. It does not edit source files itself.
- Independent audits are delegated to read-only subagents and should be started in parallel.
- Only `solidcad-implementer` is allowed to change project files.
- `solidcad-reviewer` independently reviews the resulting diff.
- `solidcad-local-verifier` uses a local Ollama model and performs the final configure/build/test/GUI launch.
- The local verifier must never edit source code or "fix" a failed build.

This structure is intentionally conservative: multiple agents may inspect the same code in parallel, but multiple write-capable agents must not edit overlapping code at the same time.

## Agents

| Agent | Responsibility | Writes code? | Suggested model |
|---|---|---:|---|
| `solidcad-lead` | Decompose work, launch parallel audits, merge findings, delegate implementation and final verification | No | Current primary model / DeepSeek V4 Pro |
| `solidcad-explorer` | Map affected modules, call graph, ownership and dependencies | No | Inherits DeepSeek |
| `solidcad-sketcher-crash-hunter` | Find Sketcher lifetime/index/constraint/undo crash risks | No | Inherits DeepSeek |
| `solidcad-qt-auditor` | QObject lifetime, signals/slots, UI state, event-loop and widget risks | No | Inherits DeepSeek |
| `solidcad-occt-auditor` | OCCT shapes, handles, selection, topology references and geometry rebuild risks | No | Inherits DeepSeek |
| `solidcad-regression-designer` | Design focused regression coverage before implementation | No | Inherits DeepSeek |
| `solidcad-implementer` | Make the smallest correct source/test changes | Yes | Inherits DeepSeek |
| `solidcad-reviewer` | Independent post-change diff review | No | Inherits DeepSeek |
| `solidcad-local-verifier` | Final configure, rebuild, tests and application launch | No | `ollama/qwen3:8b` |

## Recommended workflow

Select `solidcad-lead` as the primary agent and choose DeepSeek V4 Pro as the session model.

For a feature:
`/solidcad-feature <task>`

For a crash:
`/solidcad-crash <scenario>`

For a bug:
`/solidcad-fix <bug>`

The lead should:

1. Establish the current repository state.
2. Decompose the problem.
3. Launch all relevant read-only audits as background subagents before waiting for them.
4. Merge findings into one implementation contract.
5. Invoke exactly one write-capable `solidcad-implementer`.
6. Invoke `solidcad-reviewer`.
7. If review finds a blocker, return to the implementer for one focused correction.
8. Invoke `solidcad-local-verifier`.
9. Only report completion when the final verifier reports build + tests + GUI launch success.

## Local verifier setup

OpenCode V2 automatically discovers Ollama on its default local endpoint.

Install Ollama, then pull the local model:

```powershell
ollama pull qwen3:8b
```

Verify that OpenCode can see it using `/models`.

If your local model has another ID, edit:

`.opencode/agents/solidcad-local-verifier.md`

and replace:

`model: ollama/qwen3:8b`

with the model ID displayed by OpenCode.

The final verifier runs:

`.opencode/scripts/verify-and-run.ps1`

The script uses the repository's `dev` CMake preset, builds the project, executes the `dev` CTest preset, then launches `build/dev/src/solidar.exe`.

## Important safety rule

The local verifier is deliberately prohibited from editing files. If compilation or tests fail, it reports the failed stage and relevant output. It does not change code. The lead decides whether another DeepSeek implementation pass is needed.
