# Claude Code Project Sandbox

This file marks the directory containing it as the **project sandbox root** for Claude Code.

## Sandbox Boundaries

- The sandbox root is the directory containing this file.
- Claude Code (and any sub-agents) may freely read, edit, create, delete, build, and test files **inside** this directory.
- Claude Code must not intentionally modify files **outside** this directory.

## Git Policy

Local Git operations are allowed:

- `git status`, `git diff`, `git log`, `git branch`
- `git checkout`, `git switch`
- `git add`, `git restore`, `git stash`
- `git commit`, `git merge`, `git rebase`

Remote-publishing operations are forbidden unless the user explicitly requests them in a later turn:

- `git push`
- pushing tags
- creating pull requests

## Privilege Policy

If a command requires administrator privileges, stop and ask the user before running it.

## Configuration Layout

Project-local Claude configuration lives under `.claude/`:

- `.claude/agents/` — project-local sub-agent definitions
- `.claude/ProjectManagement/` — project planning artifacts
- `.claude/Knowledge/` — project knowledge notes

## Scope

This file defines the durable sandbox rules only. Project-specific workflows
(e.g. architect agent setup) live in their own files under `.claude/` and are
executed only when the user explicitly starts a session for them.
