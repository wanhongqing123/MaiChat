---
name: builtin-agent
description: Coordinate long tasks, local execution, questions and subagents without losing the active task.
---

# Agent workflow guidance

Use `tool_help({"arguments":["agent","-h"]})` and load actual tool schemas
through `tool_search`. This group can include questions, task coordination,
terminal sessions and subagent lifecycle tools. Not every platform provides
external-process or subagent execution.

For a running terminal command, distinguish command completion from the
session ID returned for a still-running command. Continue with the matching
stdin/session tool only when that tool is registered; do not rerun the command
just because its first response is incomplete. Read the exit status and returned
output and honor cancellation without claiming a canceled command completed.

Use a user question when a missing preference, path or constraint materially
changes the work. A later message generally steers the active task rather than
implicitly deleting it; preserve the original objective and integrate corrections.
Do not fabricate the user's answer from elapsed time.

If subagents are available and delegation is appropriate, give each one a clear,
nonoverlapping scope and the inputs it needs. Keep shared files and build lanes
under one writer at a time. Track agent IDs, wait/collect results through the
actual tools and inspect the resulting code/evidence before claiming success.
A subagent report is not an independent artifact verification.

Use the registered todo/task interface for sustained work when helpful. Keep
completed, pending and blocked work truthful. Before delivering a code change,
check the final diff, run relevant verification and state which platforms or
runtime paths remain unverified. No workflow Skill changes tool permissions,
approval rules or the user's authority to stop or redirect the work.
