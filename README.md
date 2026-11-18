# SkyrimDiagnostics

Performance and crash diagnostics for Skyrim. `SkyrimDiagnostics` is a
working name.

**Game versions:** built with CommonLibSSE-NG for SE, AE and VR in one DLL.
Tested only on AE 1.7.x (with Proton). Other versions are not tested. "Tested
in the game" in the table below means AE 1.7.x.

The tool reports problems. It does not change game behavior and it does not
fix problems.

## Status

| Function | State |
|---|---|
| Crash report with probable cause, minidump, game data (forms, plugin files) | Works, tested in the game (Proton) |
| Freeze detection and stall report | Works, tested in the game (Proton) |
| Unclean exit report (kill, out of memory, fail fast) | Works, tested in the game (Proton) |
| Overlay: frame data, frame rate limit, input latency estimate, memory, video memory | Works, tested in the game (Proton). Now drawn by AbyssusLib: not tested |
| Profiler: processor time by thread, module and mod DLL | Works, tested in the game (Proton) |
| Papyrus sampler: running and waiting script stacks by script | Works, tested in the game (Proton) |
| Live warnings with the most probable cause | Works, tested in the game (Proton) with the fault test plugin |
| Settings file, load time reports | Works, tested in the game (Proton) |
| Session report (HTML) and comparison with the last 10 sessions | In test |
| Mod Organizer 2 plugin (file to mod map, profile) | Works: the game read its data in the test of 0.2.0 (158 mods) |
| Known crash signs from Phostwood's Crash Log Analyzer data | Written, not tested |
| Monitor process (rundll32.exe with this DLL): dumps from outside, reports when the game stops with no report, opens the report | Written, not tested |
| Crash report: quick answer at the top, access violation type, hooks in the game code, caught errors, crash history, force close | Written, not tested |
| Crash report: all plugins of an object, leveled actors, MO2 profile, save, PDB function names, Crash Logger log | Written, not tested |
| Stall report: wait analysis (dialog windows, wait chain, graphics driver); infinite loading screen report | Written, not tested |
| Load times: SKSE plugins, plugin files, save loads | Written, not tested |
| Papyrus: live stacks, function totals, script time, latency test | Written, not tested |
| GPU time of each frame and render pass | Written, not tested |
| Engine limits (references, plugins, animations), heap memory by mod DLL | Works, in the overlay and the session report |
| SE 1.5.97, VR, other AE versions | Built, not tested |

All crash, stall and unclean exit reports go to `SkyrimDiagnostics/Reports`
in the SKSE log folder. Each one ends with a PROBABLE CAUSE section.

Each game session also makes a report in
`SkyrimDiagnostics/Sessions/<character name>` (`No character` until a save
is loaded): `session-<time>.html` with graphs (FPS, slowest frame, memory,
video memory, script stacks, draw calls, animations), warnings, loading
screens, processor time tables (mod DLLs, Papyrus scripts, threads),
suggestions and all events. Open it in a web browser. The tool keeps the
last 10 sessions of each character. Ten minutes into a session, the tool
compares its first 10 minutes with the average of the earlier sessions of
the same character of 10 minutes or more, and shows a note if the frame rate
values are 10 % better or worse.

The report is written one time each minute and at the game exit. Its event
lists keep 5,000 menu events and 20,000 other events; if a session has more,
the report says how many are not listed. The graphs and loading screens are
always complete. A write that fails (for example, a different program has the
file open) is in the log and in the events, with the cause.

The Suggestions section of the report comes from the `[[Suggestions]]` part
of the configuration file.

## Monitor process

When the game starts, the tool starts a second process: `rundll32.exe` from
Windows runs the function `SkyDiagMonitor` of `SkyrimDiagnostics.dll`. There
is no extra program file, and it starts with SKSE (also from Mod Organizer
2). It stops when the game stops. Its log is
`SkyrimDiagnostics/Monitor.log` in the SKSE log folder. Setting:
`[Monitor] Monitor`.

- It writes the minidump of a crash, a stall or an infinite loading screen
  from outside the game. This also works when the game process is too
  damaged to write it.
- When the game stops with an error code and no crash report (fail fast,
  stack overflow, heap corruption), it writes an unclean exit report with the
  exit code at once.
- When all of the game stops (also the watchdog thread of the tool), it
  writes `freeze-outside-*.log`: the wait chains of the threads and the
  dialog windows of the game.
- After a crash, when the game closed: it adds the function names from PDB
  files of mod DLLs (a `.pdb` file next to the DLL), adds the Crash Logger log
  of the same crash (if Crash Logger is installed), copies the MO2 mod list
  next to the report (`crash-*-mods.tsv`), and opens the report
  (`[Monitor] OpenReport`). A crash while the game closes is not opened.

Some antivirus programs do not like `rundll32.exe` running a DLL from a game
folder. If yours stops it, add an exception, or set `Monitor = 0`.

## Crash report

- **QUICK ANSWER** at the top: what happened in plain words, the suspect and
  how likely it is, one thing to try, and how often a crash at the same place
  happened before (`crash-history.txt` in the report folder).
- Access violations in plain words: null pointer, an object that was already
  deleted (free memory), a bad index (reserved memory), read-only memory, the
  end of a thread stack, a jump into freed code.
- Hooks in the game code: the tool compares the game code in memory with the
  game file and records each jump of a DLL out of the game code. The report
  names the hooks near the crash place, the hook that went to freed code, and
  the number of hooks of each DLL.
- Errors that code caught before the crash (access violations that did not
  end the game), DLLs that hide crashes (`crashguard`), and other crash
  handlers.
- Force close: Alt+F4, the X button and Quit are recorded. A crash after them
  is a crash while the game closes: it is not counted in the history and the
  report does not open. A long wait after Quit is a slow shutdown, not a stall.
- Objects near the crash: all plugins that change them; for an actor from a
  leveled list, also the forms it was made from.
- The lines `Save:` (the save that is loading or was loaded last) and
  `Mod setup:` (the MO2 profile and the number of active mods; the MO2 plugin
  writes `profile.txt`).

## Stall and loading screen reports

- The stall report has a wait analysis of the game update thread: a dialog
  window (an error message of a DLL), the wait chain (the lock and the thread
  that holds it, with its mod DLL), the graphics driver, or Direct3D.
- A loading screen that is open longer than `LoadingScreenSeconds` (120),
  with no data read from the disk in the last 30 seconds, gives an infinite
  loading screen report (`loading-*.log`).

## Load times

The session report has three tables:

- **Game start: SKSE plugins**: the time of the load function of each SKSE
  plugin, and of its functions for the SKSE messages of the game start. On
  Skyrim AE all plugins are measured; on SE and VR only the plugins that load
  after this tool.
- **Game start: plugin files**: the approximate time of each plugin file
  (from its open to the open of the next file).
- **Save loads: SKSE plugins**: the co-save load and revert functions and the
  PreLoadGame and PostLoadGame functions of each plugin.

A loading screen over 30 seconds, or a game start over 120 seconds, adds the
`load-slow` suggestions (loose files, BSA archives).

## Papyrus

- Overlay part `livestacks`: the script stacks that are longest in their
  state (running, waits for a latent function, and the like).
- Session table **Papyrus functions in this session**: total time, calls,
  average and largest call of each script function (`PapyrusTime`).
- Script time each second of all script functions: overlay value and graph.
- Script latency (`PapyrusLatency`): each 5 seconds, one call of
  `Utility.GetCurrentRealTime` through the queue of the script engine; the
  time until it ran. The values are not the same as those of Elephant's
  Script Latency Tester, which measures update events.

## GPU time

`GpuPasses`: the GPU time of each frame and of each render pass, with
timestamp queries. A render pass is the work between two changes of the
render targets; it is named by its render target (size and format), because
the names of the game's passes are not known. Overlay part `gpupasses` and
session table **GPU time by render pass**.

## Known crash signs (Phostwood's data)

The PROBABLE CAUSE section of a crash report also lists known crash signs.
They come from the data of
[Phostwood's Crash Log Analyzer](https://github.com/Phostwood/crash-analyzer)
(by Phostwood, GPL-3.0). Only its data tables are used, not the web page or its
code:

- Groups of names that are in a crash log when a known problem is the cause
  (for example graphics drivers, meshes, textures, animations, NavMesh and AI
  pathing, memory). A group is "probable" if a name is in the exception, the
  call stack or the registers, and "possible" if a name is only in the stack.
- Known crash addresses in the game executable.
- Short explanations of the files on the call stack.
- Antivirus and overlay programs that have code in the game process.
- DLL versions that do not work with newer game versions. The tool checks the
  file version of these DLLs at the game start: the result is in the log, the
  session events and each crash report. The file version of a DLL is not always
  the version of the mod, so this check can be wrong for some mods.

The data is in `data/phostwood.json`. Each build first gets the latest data of
the analyzer (`tools/phostwood.py import`; the script reads the data files as
text and runs no code of the analyzer). If the download or the import fails,
the build uses the file in the repository. To update the file in the
repository by hand:

```bash
git clone --depth 1 https://github.com/Phostwood/crash-analyzer.git analyzer
```

```bash
python tools/phostwood.py import analyzer data/phostwood.json
```

`analyzer` is the folder that the first command makes.

## Configuration

All settings are in one file: `Data/SKSE/Plugins/SkyrimDiagnostics.ini`. It
has three parts: `[[Settings]]`, `[[Layout]]` (what the overlay shows, and in
which order) and `[[Suggestions]]` (the mod suggestions of the session
report). The file has the instructions. If the file is not there, the tool
makes it when the game starts.

The default text of this file is `SkyrimDiagnostics.ini` at the top level of
this repository. The build puts that file into the DLL. To change the
defaults, change that file and build again.

The file is not in the mod package: the tool makes it on the first start, so
an update of the mod does not replace your file. Its second line is the
version of the tool that made or updated it (`# Config version: 0.1.0`). When
a different version starts, it adds to the file, in place, what the file does
not have: settings (with their comments, at the end of their `[Section]`),
layout parts (at the end of their section) and suggestion sections. Your
values, comments and order stay. Then the version line changes, and a message
box at the main menu lists what was added. Settings that a new version does
not use any more stay in the file; the tool ignores them.

In the default file, the version line has the text `@VERSION@`; the tool puts
its version there.

## Use

- The overlay needs AbyssusLib 2 or later (a separate SKSE plugin). AbyssusLib
  draws all windows and notes of the tool. Without AbyssusLib, the overlay
  does not show, a message box tells this at the start, and the reports work.
- The data window is at the top of the AbyssusLib stack (group Diag). `F11` (in AbyssusLib) cycles the configurations: Diag / GPS / Notif,
  GPS / Notif, GPS, Notif, Off. "Diag" is this tool. The `View` and
  `ToggleKey` settings and the `F10` key are removed; an old settings file
  with them still loads.
- The warnings are notifications in the AbyssusLib notification queue (there
  is no warnings box). A warning stays in the queue while its condition is
  true, and for 30 seconds (`WarningHoldSeconds`) after it ended, dim and with
  "ended N s ago". Colors: critical red, warning yellow, note grey. The Plugins
  warning: over the limit it stays always; near the limit until 1 minute after
  the first cell is loaded.
- The log file is `SkyrimDiagnostics/SkyrimDiagnostics.log` in the SKSE log folder.
- Settings are in `Data/SKSE/Plugins/SkyrimDiagnostics.ini` (warning
  limits, sampler switches). The file has a comment for
  each setting.
- `MO2-plugin/skyrim_diagnostics_export.py` in the package is a Mod
  Organizer 2 plugin. Copy it to the MO2 `plugins` folder. It writes the mod
  list and the file-to-mod map when the game starts.

## Warnings

| Warning | Condition | What it says |
|---|---|---|
| FPS | Below the limit in the settings (57) | The cause: GPU, frame cap or VSync, or CPU with the most probable source |
| Stutter | Slowest frame is 50 ms or more and 3 times the average | The frame time and what the busiest thread did in that frame |
| Input latency | Estimate is 60 ms or more | The cause: GPU queue or frame time |
| Memory | The game can get less than 3 GB more (critical below 1 GB) | A crash is probable at the critical level |
| Video memory | 90 % of the budget (critical at 100 %) | Stutter and low FPS are expected when full |
| Memory growth | 30 MB each minute for 5 minutes and in the last 2 minutes, with no loading screen | Possible leak, and the time until memory is full |
| Papyrus | 500 script stacks or more | Scripts are slow, stack dumps are possible |
| Object references | 80 % of the 1,048,576 reference handles in use (critical at 95 %) | The game can crash when all are in use |
| Plugins | 90 % of 254 full plugins or of 4,096 light plugins: shows until 1 minute after the first cell is loaded (the first loading screen after the main menu). Above the maximum: critical, shows always. In the notification queue, this warning stays while it shows | The count is near or over the limit |

In the main menu, and from the game start until the main menu closes, only
the Plugins warning and the critical Memory and Video memory warnings (memory
full) show. All other warnings are off there, also in the session report.

Frame, stutter, latency and Papyrus warnings are off during a loading
screen and for 10 seconds after it.

## Mod Organizer 2 data

Copy `MO2-plugin/skyrim_diagnostics_export.py` from the build artifact to
the `plugins` folder of Mod Organizer 2. When the game starts from MO2, the
plugin writes `mods.tsv`, `files.tsv` and `owners.tsv` to
`SKSE/Plugins/SkyrimDiagnostics` in the overwrite folder. The tool reads
`owners.tsv` and shows the mod of a DLL in the crash, stall and session
reports, and the mod of a script (loose `.pex` file) in the session report.
State: not tested in MO2.

## Other measurements

- **Heap memory by mod DLL** (`MemoryByDll` in the settings file): the tool
  changes the import tables of the mod DLLs and counts their heap
  allocations. Not included: memory from the game's own memory manager,
  VirtualAlloc, and memory of the graphics driver.
- **Papyrus function time** (`PapyrusTime` in the settings file): the tool
  measures the time that the script engine uses in the code of each script
  function. For this, it changes one entry in the function table of the
  script function class. Native functions are not included.
- **Input latency**: measured from the time the game reads the input to the
  end of the Present call of that frame, plus the frames in the GPU queue.
  The display is not included. If the input time is not available, the
  overlay shows an estimate and says so.
- **Overlay target**: AbyssusLib draws the overlay on the render target of the
  game UI, so that Community Shaders (upscaling, frame generation, HDR)
  handles it as it handles the game UI. The `OverlayTarget` setting is
  removed; an old settings file with it still loads.

## Tests in the game

- `SkyrimDiagnosticsFaultTest.dll` (second build artifact) is a separate
  SKSE plugin that makes faults on purpose. It makes an empty `inject.txt`
  in the `SkyrimDiagnostics` folder (SKSE log folder) and reads it every
  2 seconds, also while the game runs:
  - `cpu 15`: uses the processor for 15 ms in each frame
  - `sleep 15`: waits for 15 ms in each frame
  - `leak 600`: allocates 600 MB each minute and keeps it
  - `crash`: crashes the game one time
  - `freeze`: stops a game update thread
  - an empty file stops the `cpu`, `sleep` and `leak` faults
  Before a crash or a freeze, the plugin makes the file empty.

## Phases

1. Foundation: build system, CI build, plugin loads. (done)
2. Crash report, freeze detection, session record, early start. (done)
3. Frame and input latency telemetry, in-game overlay.
4. Sampling profiler for SKSE DLLs, Papyrus profiler, Papyrus probe.
5. RAM, VRAM, leak detection, memory attribution, process health.
6. Load time analysis, asset attribution, MO2 plugin.
7. Engine limits.
8. Live warnings and cause ranking.
9. HTML session report, baselines, crash grouping, rule file.
10. VR support.

## Install

1. Install [SKSE64](https://skse.silverlock.org/),
   [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
   and AbyssusLib (for the overlay).
2. Download the `SkyrimDiagnostics` artifact from the latest run of the
   Build workflow (Actions tab). The build starts only by hand ("Run
   workflow"). It first checks that `src/Abyssus/API.h` is the file of its
   AbyssusLib version.
3. Install the archive as a mod in MO2.

The log file is `SkyrimDiagnostics.log` in the SKSE log folder
(`Documents/My Games/Skyrim Special Edition/SKSE`).

## Build

Requirements: Visual Studio 2022 (Desktop development with C++), CMake 3.24
or later, Ninja, vcpkg with the `VCPKG_ROOT` environment variable set.

Run these commands in an "x64 Native Tools Command Prompt for VS 2022":

```
git clone --recursive https://github.com/YellowCourtesy/Sky-Diagnostics.git
cd Sky-Diagnostics
cmake --preset release
cmake --build --preset release
cmake --install build/release
```

The mod folder is `build/package`. CommonLibSSE-NG is a git submodule in
`lib/commonlibsse-ng` (v16.4.0, the same version as AbyssusLib and SkyGPS). In
a clone without `--recursive`, run `git submodule update --init --recursive`.

## License

GPL-3.0-or-later. This is a requirement of
[CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG), which the
plugin links statically.
