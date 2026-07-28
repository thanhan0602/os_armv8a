---
name: auto-commit
description: "Workflow for committing and pushing code changes after a feature or fix is verified. Use when the user says 'ok', 'đã xong', 'commit đi', or confirms the program works as expected."
user-invocable: true
---

# Auto-Commit Skill

This skill automates the Git workflow to ensure changes are safely recorded after verification.

## Guidelines

1. **Verify Status**: Run `git status` to see what files are modified.
2. **Review Changes**: Use `git diff --cached` (or `git diff` if not staged) to summarize changes for the commit message.
3. **Stage Files**: Stage all relevant changes using `git add .` or specify files if the user requested a partial commit.
4. **Draft Message**: Create a concise, descriptive commit message in English (following Conventional Commits if possible, e.g., `feat: ...`, `fix: ...`).
5. **Commit & Push**: Execute the commit and ask the user if they want to push to the remote repository.

## Execution Pattern

When the user confirms "ok" or "commit":
1.  **Check Diff**:
    - `git status`
    - `git diff`
2.  **Propose Message**:
    - Summarize based on the session's work (e.g., "Implement lazy binding and add QEMU safety skill").
3.  **Execute**:
    ```bash
    git add .
    git commit -m "<message>"
    ```
4.  **Confirm**: Report the commit hash to the user.

## Safety
- Never commit without confirming the diff summary with the user if the changes are large or complex.
- Check for any temporary test files or `LD_DEBUG=1` hacks that should be reverted before committing.
