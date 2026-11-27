# Skill URL Installation Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development for the independent repository installer, filesystem publication and runtime refresh components.

**Goal:** A MaiChat request to install a skill from a GitHub URL can inspect the repository, locate the actual skill, safely install its files into the standard user/workspace directory, and use it on the next input in the same conversation.

**Architecture:** Add an ordinary C++ GitHub skill repository installer plus a thin skill_install tool. Reuse the existing skill metadata parser, native filesystem and embedded curl; no Python/Node runtime is required to install skill files. The model resolves semantic README links rather than hardcoding Playwright. Runtime discovery refreshes only at new user-input boundaries, preserving old prompts and selected-body snapshots.

**Tech Stack:** Existing C++17, libcurl/OpenSSL, JSON, MaiSkillManager and MaiFileSystem; no new third-party dependencies or MCP.

## Contract and ownership

- Default install scope is user `<home>/.agents/skills`; explicit workspace scope uses `<session-workspace>/.agents/skills`. These roots come from the host, never an arbitrary model destination path.
- `skill_install mode=discover` resolves an HTTPS GitHub repository/blob/tree URL, pins a commit, lists candidate SKILL.md folders and relevant README GitHub links. A framework root with several repository-maintenance skills must not be guessed as one browser skill.
- `mode=install` accepts a discovered repository, path and pinned ref. Download only that folder's files, validate through the existing skill parser, preserve resources/executable metadata and record source identity. Do not clone/install an entire framework or execute downloaded scripts.
- Existing directories are not overwritten. Stage outside the active skills tree on the same filesystem, then atomically publish the directory without replacement. Reject links/path traversal and unsupported source kinds; failures/cancellation clean only owned staging.
- Skill dependencies are separate from skill-file installation. Do not claim browser/CLI availability merely because SKILL.md was copied.
- New user input refreshes current metadata/discovery, while the original baseInstructions and historical per-input contexts remain immutable. Installation becomes discoverable on the next input, matching the explicit Codex installer workflow.

## Tasks

- [x] Add a safe no-replace directory publication and executable-bit primitive to MaiFileSystem; native failure/cancellation semantics and owned regression tests.
- [x] Implement bounded GitHub discovery/install using existing curl and the same metadata parser; injectable source fetch for deterministic tests, no public HTTP/JSON types.
- [x] Add skill_install tool, host-authoritative scope resolution and product prompt guidance. Read-only discovery and approving installation must remain distinct.
- [x] Refresh skills at unseen user-input boundaries; add same-session install/new-input/read tests and preserve all old prefix/pinning invariants.
- [x] Extend the actual desktop-host Playwright acceptance from preinstalled skill verification to URL discovery/install followed by subsequent-input execution. Use exact official source and private home/workspace.
- [x] Compare the same CLI workflow through current Codex and MaiChat; fix foreground/background process lifetime without losing owner cleanup.
- [x] Run normal builds, skill/file/process/runtime regressions, real owned browser/page/POST/screenshot checks and record boundaries. Never use existing browser profiles, user databases, real model keys, installs or global npm configuration for acceptance.

Evidence and platform/model boundaries: [acceptance record](../../diagnostics/2026-11-26-playwright-skill-install-acceptance.md).
