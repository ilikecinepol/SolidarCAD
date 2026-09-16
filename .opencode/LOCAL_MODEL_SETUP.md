# Local free model setup

The final verifier is configured for:

`ollama/qwen3:8b`

Why a small local model is used here:
- its job is mechanical, not architectural;
- compile/test waiting should not consume paid DeepSeek API usage;
- it is denied edit permission, so it cannot "repair" code after a failed build.

## Install

1. Install Ollama for Windows.
2. In PowerShell:

```powershell
ollama pull qwen3:8b
```

3. Start/keep Ollama running.
4. Open OpenCode and check `/models`.
5. Confirm `ollama/qwen3:8b` is visible.
6. Run `/solidcad-verify`.

If you already use another free local tool-capable model, replace the single `model:` line in:

`.opencode/agents/solidcad-local-verifier.md`

Example:

`model: ollama/<your-model-id>`

The build script itself does not depend on the model. The model only invokes the script and summarizes PASS/FAIL.
