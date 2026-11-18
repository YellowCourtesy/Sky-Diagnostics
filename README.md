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
| Mod Organizer 2 plugin (file to mod map) | Written, not tested, not used by the tool yet |
| Engine limits, memory attribution by DLL, HTML report | Not started |
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
- The data window is at the top of the AbyssusLib stack. The warnings box has
  its own place (plugin name `SkyrimDiagnostics Warnings`, place 5000): under
  SkyGPS and the other mods, directly above the notifications. Both are in
  the group Diag. `F11` (in AbyssusLib) cycles the configurations: Diag / GPS / Notif,
  GPS / Notif, GPS, Notif, Off. "Diag" is this tool. The `View` and
  `ToggleKey` settings and the `F10` key are removed; an old settings file
  with them still loads.
- Each new warning is also sent one time to the AbyssusLib notification
  queue (10 seconds). The Plugins warning is different: it stays in the queue
  while it shows (always when over the limit).
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
