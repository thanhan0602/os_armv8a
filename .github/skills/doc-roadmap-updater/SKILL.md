---
name: doc-roadmap-updater
description: "Workflow for updating project documentation and roadmaps after implementing a feature or fixing a bug. Use when a technical task is completed to ensure documentation parity."
user-invocable: true
---

# Documentation & Roadmap Updater Skill

This skill ensures that the project's documentation reflects the current state of the codebase.

## Guidelines

1. **Phase-Based Updates**:
   - **Pre-implementation**: Create or update the High-Level Design (HLD) doc and the Plan. Ensure the user agrees on the architecture before writing code.
   - **Post-implementation**: Create or update the Low-Level Design (LLD) or specific handoff docs to reflect the final implementation details.
2. **Identify Target Files**: Check `roadmap.md`, `handoff.md`, and specific subsystem docs in `document/` (e.g., `handoff_mmu.md`).
3. **Review Implementation**: Extract key technical details, new structures, or fixed bugs from the task just completed.
4. **Update Roadmap**:
    - Mark completed tasks as `[x]`.
    - Add new technical findings or next steps if discovered.
5. **Update Handoff/Docs (LLD)**:
    - Record new architectural decisions (HLD part).
    - Document new assembly-kernel interfaces, syscall changes, and specific C logic (LLD part).
    - Update "Known Issues" or "Post-mortems" if applicable.

## Execution Pattern

When a feature implementation is confirmed:
1.  **Analyze**: "What changed in the logic/ABI?"
2.  **Locate Docs**: Find relevant files in `document/` or root.
3.  **Perform Edits**:
    - `roadmap.md`: Update progress.
    - `handoff.md`: Add a summary of the current state.
    - `document/handoff_<subsystem>.md`: Update technical details.
4.  **Notify**: Inform the user which documents were updated.

## Safety
- Ensure technical terms match the code exactly (e.g., specific register names like `x16`, `x30`).
- Do not remove historical context unless it is explicitly superseded.
