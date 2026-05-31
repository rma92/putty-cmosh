# AGENTS.md

## Current work

We are in the middle of implementing a complex new protocol feature in PuTTY.

A plan already exists. Do not restart planning from scratch unless explicitly asked. Preserve the current implementation direction, infer intent from existing changes, and continue from the current working tree state.

## Operating style

* Prefer small, localized changes.
* Do not reformat unrelated code.
* Do not rename files, move code, or reorganize modules unless required by the current task.
* Preserve PuTTY's existing C style and nearby idioms.
* Make minimal changes that fit the existing architecture.
* When uncertain, inspect nearby code and follow precedent.
* Avoid speculative abstractions.
* Do not remove compatibility behavior unless explicitly directed.

## Context discipline

Context is expensive. The user runs `/compact` often.

Before reading broadly, identify the smallest likely set of relevant files/functions.

Prefer this workflow:

1. Inspect current git status and changed files.
2. Read the existing plan or handoff notes if present.
3. Search narrowly for the specific symbol, packet type, state transition, or protocol concept.
4. Read only the top relevant files/functions.
5. Make the smallest safe change.
6. Summarize what changed and what remains.

Do not dump large command outputs into the conversation.

When searching, prefer commands that return filenames or limited matches:

```powershell
rg -l "pattern" --glob "*.c" --glob "*.h"
rg -n "pattern" --glob "*.c" --glob "*.h" -m 40
rg -n "pattern" path\to\likely-file.c -m 40
```

Avoid broad recursive PowerShell searches unless output is capped.

If using PowerShell `Select-String`, limit the output:

```powershell
Get-ChildItem -Recurse -Include *.c,*.h |
  Select-String -Pattern "pattern" |
  Select-Object -First 40
```

For diffs, prefer:

```powershell
git diff --stat
git diff --name-only
git diff -U3 -- path\to\file.c
```

Do not repeatedly print full repository-wide diffs.

## Before and after `/compact`

Because `/compact` is used frequently, maintain continuity.

At natural checkpoints, produce or update a compact handoff note containing:

* current goal
* files touched
* important design decisions
* protocol invariants
* commands/tests already run
* known failures or unfinished work
* exact next step

Keep handoff notes concise and factual.

After `/compact`, first recover context from:

1. current working tree
2. latest handoff note
3. existing plan
4. `git status`
5. small targeted diffs

Do not re-discover the whole codebase after each compact.

## PuTTY-specific expectations

* This is C code in the PuTTY codebase.
* Follow existing PuTTY naming, allocation, error-handling, logging, and cleanup patterns.
* Prefer existing helper functions and data structures.
* Keep protocol state explicit and easy to audit.
* Maintain clean failure paths.
* Be careful with packet parsing, bounds checks, ownership, and lifetime.
* Avoid introducing global state unless consistent with nearby code.
* Do not weaken existing security checks.
* Do not assume network input is trusted.
* Preserve existing behavior for protocols and platforms not touched by this feature.

## Implementation guidance

When adding protocol logic:

* Define constants close to related constants.
* Keep encode/decode/parsing logic close to similar protocol code.
* Validate lengths and enum values before use.
* Handle unsupported or malformed messages explicitly.
* Add comments only where they explain protocol rules, invariants, or non-obvious control flow.
* Prefer clear state-machine transitions over clever shortcuts.

When modifying existing logic:

* First understand the current call path.
* Check whether the same concept appears in another protocol path.
* Make the narrowest change that integrates with existing behavior.
* Avoid changing public interfaces unless required.
* If an interface must change, update all callers deliberately.

## Testing and verification

Run focused checks before broad ones.

Prefer tests or builds that are directly relevant to the changed files first.

When reporting results, summarize only:

* command run
* pass/fail
* important error text
* next action

Do not paste long build logs unless the relevant failure is not otherwise clear.

If a command produces too much output, rerun with filtering or summarize the important lines.

## Communication

Be concise.

When making a change, report:

* what changed
* why it was needed
* files touched
* tests/checks run
* remaining risks or TODOs

If blocked, state the specific missing fact or failing command, then propose the smallest next step.

Do not ask for confirmation for routine continuation of the existing plan. Continue with the best-supported next step from the current state.

