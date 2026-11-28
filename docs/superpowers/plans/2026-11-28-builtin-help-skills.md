# Builtin Help and Skills Implementation Plan

> For agentic workers: use superpowers:subagent-driven-development for the separate
> grouping task and review; Root integrates sequentially on main.

**Goal:** Give models real short grouped help and bundled on-demand workflow Skills.
**Architecture:** Metadata-only ordinary help core plus thin deferred adapter;
existing Skill manager reads shared compiled Markdown resources. FFmpeg stays
with its original engine/lock and reviewed writer plan.
**Tech Stack:** C++17, current registry/Skill manager, CMake-generated UTF-8 arrays.

- [x] Implement pure MaiToolHelp API/adapter with strict argv: [] or ["-h"],
  [group,"-h"]. Register without bootstrap changes. Add current summary routing.
  Tests in MaiToolHelpTests.cpp prove actual visibility, no schema/execution,
  ordinary/tool parity and invalid input handling. Normal wrapper must first
  fail the new behavior, then pass after implementation.
- [x] Add five self-contained SKILL.md files under MaiAgent/resources/skills,
  generate bounded immutable resource records at configure time in the core.
  Add opt-in includeBuiltinSkills and Builtin scope to MaiSkillManager; append
  after user discovery with the existing limits. read/readCurrent must validate
  catalog ownership first and return embedded bytes only for discovered entries.
  Test no opt-in, all builtin reads, UTF-8/body/count budget, foreign catalog,
  user duplicate-name ambiguity, no user-file mutation and real skill_read.
- [x] Enable builtins next to skillsEnabled in Desktop/MobileAdapter/CLI; Mac
  carries the first two narrow host hunks to avoid source writer conflicts.
  Normal shared CMake build includes identical resource bytes on every target.
- [x] Integrate Mac FFmpeg v2 narrow bytes only after checking before/post
  hashes and reviewed help argv list. Validate original real-engine -h RED to
  GREEN, topics/lists, long output, sequential failure/cancel cleanup, no
  cross-thread stdout capture. No real supplier calls.
- [x] Run required eight Core suites plus help/catalog/discovery/Skills/runtime
  and meaningful budget fixtures, normal Desktop build/PE/staging/ZIP smoke.
  Android arm64 JNI build validates shared resources and callback compilation.
  Bind exact commit/source/artifacts and inspect actual group/help tool output.
- [x] Independent scope/quality review; record diagnostics under docs/diagnostics;
  commit/push only authorized files. Give Mac final same-source consumers and
  report platform limits without claiming release or installation.

Final source0afd03e validated: Windows normalApp/Core/FFmpeg/packaging; Android normalJNI and owned arm64 native runtime; Mac exact-source7/7+App smoke; iOS Simulator shared consumer normal compile/link only (no booted device). See diagnostics for real RED/GREEN and context-window boundaries.
