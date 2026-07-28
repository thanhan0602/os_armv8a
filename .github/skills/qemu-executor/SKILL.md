---
name: qemu-executor
description: "Workflow for running QEMU for a fixed duration to avoid infinite hangs. Use when the user asks to 'run qemu', 'test in qemu', or 'verify on hardware'."
user-invocable: true
---

# QEMU Executor Skill

This skill ensures that QEMU runs are managed with a timeout to prevent the agent from hanging indefinitely.

## Guidelines

1. **Always use mode='async'** for QEMU commands when using `run_in_terminal`.
2. **Set a reasonable timeout** (typically 5000-10000ms) for the initial output.
3. **Capture output** and then use `kill_terminal` or `terminal_last_command` to manage the lifecycle.
4. **Prefer Scripts**: Use the existing `scripts/run_qemu.sh` but wrap it in a timeout logic if possible.

## Execution Pattern

When asked to run QEMU:
- Use `run_in_terminal` with `mode: 'async'` and a `timeout: 10000`.
- If the command times out, report the captured output to the user.
- Remind the user that QEMU is still running in the background and suggest `kill_terminal` if it's no longer needed.
- If specific log patterns are needed (e.g., "completed", "exited"), use `grep` or `tail` on the terminal output via `get_terminal_output`.

## Troubleshooting
- If QEMU hangs the terminal, use `kill_terminal` with the ID returned by the async call.
- If the output is truncated, use `get_terminal_output` to fetch more.
