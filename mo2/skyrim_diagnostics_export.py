"""
Mod Organizer 2 plugin for SkyrimDiagnostics.

When the game starts from MO2, this plugin writes two text files that tell
SkyrimDiagnostics which mod each file comes from:

  <overwrite>/SKSE/Plugins/SkyrimDiagnostics/mods.tsv
      one line for each active mod, in priority order (lowest first):
      priority <tab> mod name <tab> version

  <overwrite>/SKSE/Plugins/SkyrimDiagnostics/files.tsv
      one line for each file in the virtual Data folder:
      lower-case path relative to Data <tab> winning mod <tab> number of mods that have the file

  <overwrite>/SKSE/Plugins/SkyrimDiagnostics/owners.tsv
      the lines of files.tsv for the file types that SkyrimDiagnostics
      reads now: DLL files, compiled scripts (.pex), plugins (.esp, .esm,
      .esl) and archives (.bsa). This file is small, so the game reads it
      fast.

The files are in the MO2 overwrite folder, so the game sees them at
Data/SKSE/Plugins/SkyrimDiagnostics/.

Install: copy this file to the "plugins" folder of Mod Organizer 2.

How to see if it operates: after you start the game from MO2, the overwrite
folder has SKSE/Plugins/SkyrimDiagnostics/export-status.txt. The file has
"OK" and the counts, or "ERROR" and the cause.

State: written from the MO2 type stubs (mobase-stubs 2.5.2).
"""

import datetime
import os
import traceback
from typing import List, Sequence

import mobase


class SkyrimDiagnosticsExport(mobase.IPlugin):
    _organizer: mobase.IOrganizer

    def __init__(self):
        super().__init__()

    def init(self, organizer: mobase.IOrganizer) -> bool:
        self._organizer = organizer
        organizer.onAboutToRun(self._on_about_to_run)
        # A first status line, so that the file shows that MO2 loaded the
        # plugin also when no program was started yet.
        try:
            self._status("The plugin is loaded. No program was started from MO2 after that.")
        except Exception:
            print("SkyrimDiagnostics Export: could not write the first status\n" + traceback.format_exc())
        return True

    def name(self) -> str:
        return "SkyrimDiagnostics Export"

    def author(self) -> str:
        return "YellowCourtesy"

    def description(self) -> str:
        return "Writes the mod list and the file-to-mod map for SkyrimDiagnostics when the game starts."

    def version(self) -> mobase.VersionInfo:
        return mobase.VersionInfo("0.1.0")

    # No settings. An earlier version had a setting with the name
    # "enabled". MO2 uses that name for its own on/off switch of each
    # plugin, so the setting of this plugin could read as "off".
    def settings(self) -> Sequence[mobase.PluginSetting]:
        return []

    # Called by MO2 before it starts a program. Must return True, or MO2
    # does not start the program.
    def _on_about_to_run(self, executable: str) -> bool:
        try:
            mods, files, owners = self._export()
            self._status(f"OK. {mods} active mods, {files} files, {owners} files in owners.tsv. Program: {executable}")
        except Exception:  # never stop the game start
            self._status("ERROR. The export did not complete.\n" + traceback.format_exc())
        return True

    # Writes the result of the last export to export-status.txt, next to the
    # export files, and to the MO2 log. Use this file to see if the plugin
    # operates.
    def _status(self, text: str) -> None:
        line = f"{datetime.datetime.now().isoformat(timespec='seconds')} SkyrimDiagnostics Export: {text}"
        print(line)
        try:
            out_dir = os.path.join(self._organizer.overwritePath(), "SKSE", "Plugins", "SkyrimDiagnostics")
            os.makedirs(out_dir, exist_ok=True)
            with open(os.path.join(out_dir, "export-status.txt"), "w", encoding="utf-8", newline="\n") as out:
                out.write(line + "\n")
        except Exception:
            print("SkyrimDiagnostics Export: could not write export-status.txt\n" + traceback.format_exc())

    def _export(self):
        mod_list = self._organizer.modList()
        out_dir = os.path.join(self._organizer.overwritePath(), "SKSE", "Plugins", "SkyrimDiagnostics")
        os.makedirs(out_dir, exist_ok=True)

        # Active mods, lowest priority first. A later mod wins a file conflict.
        active: List[str] = []
        for name in mod_list.allModsByProfilePriority():
            if not (mod_list.state(name) & mobase.ModState.ACTIVE):
                continue
            mod = mod_list.getMod(name)
            if mod is None or mod.isSeparator() or mod.isBackup() or mod.isOverwrite():
                continue
            active.append(name)

        winners = {}  # lower-case relative path -> [winning mod, count]

        def scan(mod_name: str, root: str) -> None:
            if not os.path.isdir(root):
                return
            for folder, _dirs, files in os.walk(root):
                relative_folder = os.path.relpath(folder, root)
                for file_name in files:
                    relative = file_name if relative_folder == "." else os.path.join(relative_folder, file_name)
                    key = relative.replace("/", "\\").lower()
                    entry = winners.get(key)
                    if entry is None:
                        winners[key] = [mod_name, 1]
                    else:
                        entry[0] = mod_name
                        entry[1] += 1

        with open(os.path.join(out_dir, "mods.tsv"), "w", encoding="utf-8", newline="\n") as out:
            for priority, name in enumerate(active):
                mod = mod_list.getMod(name)
                try:
                    version = mod.version().canonicalString()
                except Exception:
                    version = ""
                out.write(f"{priority}\t{name}\t{version}\n")
                scan(name, mod.absolutePath())

        # The overwrite folder wins over all mods.
        scan("<overwrite>", self._organizer.overwritePath())

        owner_types = (".dll", ".pex", ".esp", ".esm", ".esl", ".bsa")
        owner_count = 0
        with open(os.path.join(out_dir, "files.tsv"), "w", encoding="utf-8", newline="\n") as out, open(
            os.path.join(out_dir, "owners.tsv"), "w", encoding="utf-8", newline="\n"
        ) as owners:
            for key in sorted(winners):
                mod_name, count = winners[key]
                # The export files themselves are not mod files.
                if key.startswith("skse\\plugins\\skyrimdiagnostics\\"):
                    continue
                line = f"{key}\t{mod_name}\t{count}\n"
                out.write(line)
                if key.endswith(owner_types):
                    owners.write(line)
                    owner_count += 1
        return len(active), len(winners), owner_count


def createPlugin() -> mobase.IPlugin:
    return SkyrimDiagnosticsExport()
