# Rules for AI agents

The source of this file is `CLAUDE.md` in the AbyssusLib repository. Each
repository that depends on AbyssusLib has a copy, which the workflow
`.github/workflows/sync-rules.yml` of that repository updates automatically.
Change the rules only in AbyssusLib; do not change a copy.

## 1. Build only when the user asks

Build only when the user asks for a build explicitly, and never in any other
case. This includes local builds, compile tests and runs of a build workflow.
The build workflows start only by hand (`workflow_dispatch`); do not add a
trigger that starts a build automatically.

Exception: when a build that the user asked for fails, fix the cause and start
the build again yourself, without asking. Do this only while the user has not
added a request or interrupted the work since the failed build. If the user
added a request or interrupted, ask before the next build.

## 2. AbyssusLib is its own mod

AbyssusLib must not be included into any other mod. It is its own, independent
mod with its own DLL and its own build.

- A different mod has only a copy of `include/Abyssus/API.h` (in
  `src/Abyssus/API.h`). The header finds `AbyssusLib.dll` when the game runs;
  nothing is linked.
- Do not change that copy. Its first line, `// From AbyssusLib x.y.z`, tells
  its version; the build of the mod compares it with that version of
  AbyssusLib.
- Never add AbyssusLib (its source, its DLL or its repository) to the build,
  the package or the repository of a different mod: no submodule, no
  FetchContent, no package dependency.

## 3. This rules file

The `CLAUDE.md` of AbyssusLib is the source of these rules. They go to each
repository that depends on AbyssusLib automatically.

- A repository that depends on AbyssusLib has a copy of this file as
  `CLAUDE.md` and the workflow `.github/workflows/sync-rules.yml`, which copies
  the file from AbyssusLib each day. That workflow does not build.
- When you make a new repository that depends on AbyssusLib, or add the
  AbyssusLib header to a repository, add both files to it.
