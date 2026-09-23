"""Make the Epic Games Launcher list this project, so a Fab / vault asset can be
added to it from the Library's "Add to project" dropdown.

WHY IT IS MISSING
-----------------
The launcher builds its project list in
FDesktopPlatformBase::EnumerateProjectsFromEngine
(Engine/Source/Developer/DesktopPlatform/Private/DesktopPlatformBase.cpp:1801-1877)
from exactly three sources, all of them inside one file:

    <EngineDir>/Saved/Config/WindowsEditor/EditorSettings.ini
      [/Script/UnrealEd.EditorSettings]
        CreatedProjectPaths=<dir>        # a PARENT directory, scanned 1 level deep
        RecentlyOpenedProjectFiles=...   # taken as a literal .uproject path

Two things keep this project out of that list, and they are independent:

1. No CreatedProjectPaths covers it. UE writes that key only when a project is
   created through the editor's New Project dialog. This project was made by
   hand, so the key does not exist at all for 5.7 - the section is there, the
   key is not.

2. Its RecentlyOpenedProjectFiles entry is unusable. UE 5.7 stores that setting
   as TArray<FRecentProjectFile>, so the ini value is a struct literal, but
   DesktopPlatform still reads the key into a TArray<FString> and pushes
   whatever it finds into the project list as if it were a path. From
   EpicGamesLauncher.log, the only entry in the whole list shaped like that:

     Found project "(ProjectName="<project>/PursuitAI.uproject",
     LastOpenTime=2026.09.19-13.48.23)" in recently opened files

   Every other entry is a bare path. That one normalises to a filename with a
   trailing ")", so it is never a real project, and the real path never enters
   the list. This is a version mismatch between the editor (struct) and the
   launcher (string), not something this project did wrong.

THE SCAN IS EXACTLY ONE LEVEL DEEP
----------------------------------
    for each <dir> in CreatedProjectPaths:
        for each subdirectory <sub> of <dir>:
            collect <dir>/<sub>/*.uproject

So the value has to be the project's PARENT directory. E:\\Project, not
E:\\Project\\UE5+ai: the latter would search E:\\Project\\UE5+ai\\<sub>\\*.uproject
and find nothing, silently. This is empirically confirmed by the working sample
in UE 4.27's own config, where CreatedProjectPaths=E:\\Project\\Test yields
<unrelated project>.uproject.

WHAT THIS DOES
--------------
Adds CreatedProjectPaths=E:\\Project under the same section, idempotently.
It does not touch RecentlyOpenedProjectFiles: that key drives the editor's own
recent-projects list, and rewriting it as a bare path would fix the launcher at
the cost of breaking the editor's list. CreatedProjectPaths is the structural
fix and the editor only ever appends to it.

The edit refuses to run while the editor is open. UnrealEditor loads this file
once at startup and writes it back on shutdown, so a line added underneath a
running editor can be erased when it exits. The launcher only needs a restart -
it does not write this file.

    python tools/fix_fab_project_list.py --check     # what the launcher sees now
    python tools/fix_fab_project_list.py --dry-run   # show the diff, write nothing
    python tools/fix_fab_project_list.py             # apply, with a backup
"""

import argparse
import ctypes
import datetime
import os
import shutil
import sys
from ctypes import wintypes

# ---------------------------------------------------------------------------
# What we are fixing
# ---------------------------------------------------------------------------

# The directory the launcher has to scan. Its immediate subdirectory has to be
# the project folder - that is the one-level rule above.
SEARCH_ROOT = r"E:\Project"

UPROJECT = r"<project>\\PursuitAI.uproject"

# The game-agnostic editor config for the 5.7 engine. This is the path the
# launcher logged as "Looking for directories to scan from", so it is the one
# that matters. (4.27 has its own copy under .../4.27/Saved/Config/Windows and
# is only consulted when the launcher enumerates 4.27 projects.)
CONFIG = os.path.join(
    os.environ.get("LOCALAPPDATA", r"%USERPROFILE%\AppData\Local"),
    "UnrealEngine", "5.7", "Saved", "Config", "WindowsEditor", "EditorSettings.ini",
)

SECTION = "[/Script/UnrealEd.EditorSettings]"
KEY = "CreatedProjectPaths"
RECENT_KEY = "RecentlyOpenedProjectFiles"

# Writes must not race the editor. Listed rather than guessed: these are the
# binaries that hold the config in memory.
EDITOR_PROCESSES = ("UnrealEditor.exe", "UnrealEditor-Cmd.exe")

# Only needs a restart - the launcher reads the file at startup and never
# writes it. Reported so the user knows the change is not live yet.
RESTART_PROCESSES = ("EpicGamesLauncher.exe",)


# ---------------------------------------------------------------------------
# Process lookup, without psutil
# ---------------------------------------------------------------------------

TH32CS_SNAPPROCESS = 0x00000002
MAX_PATH = 260


class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("th32DefaultHeapID", ctypes.c_void_p),
        ("th32ModuleID", wintypes.DWORD),
        ("cntThreads", wintypes.DWORD),
        ("th32ParentProcessID", wintypes.DWORD),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", wintypes.DWORD),
        ("szExeFile", ctypes.c_char * MAX_PATH),
    ]


def running_processes():
    """Return {lowercase exe name: [pid, ...]} for every live process.

    ctypes rather than psutil: this script has to run with a bare interpreter,
    and pulling a dependency in to answer one question is not worth it.
    """
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    k32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    k32.Process32First.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32)]
    k32.Process32Next.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32)]

    snapshot = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snapshot == wintypes.HANDLE(-1).value or not snapshot:
        return {}

    found = {}
    try:
        entry = PROCESSENTRY32()
        entry.dwSize = ctypes.sizeof(PROCESSENTRY32)
        ok = k32.Process32First(snapshot, ctypes.byref(entry))
        while ok:
            name = entry.szExeFile.decode("mbcs", "replace").lower()
            found.setdefault(name, []).append(int(entry.th32ProcessID))
            ok = k32.Process32Next(snapshot, ctypes.byref(entry))
    finally:
        k32.CloseHandle(snapshot)
    return found


def matches(processes, names):
    hits = []
    for name in names:
        for pid in processes.get(name.lower(), []):
            hits.append("%s (PID %d)" % (name, pid))
    return hits


# ---------------------------------------------------------------------------
# Config file
# ---------------------------------------------------------------------------

def read_config(path):
    """Return the raw text. utf-8, not utf-8-sig, so a byte-order mark survives
    a read/write round trip instead of being silently dropped."""
    with open(path, "rb") as handle:
        return handle.read().decode("utf-8")


def section_lines(lines, section):
    """Index range [header+1, end) of a section, or None if it is absent."""
    header = None
    for index, line in enumerate(lines):
        if line.strip().lower() == section.lower():
            header = index
            break
    if header is None:
        return None

    end = len(lines)
    for index in range(header + 1, len(lines)):
        if lines[index].lstrip().startswith("["):
            end = index
            break
    return header, end


def split_kv(line):
    if "=" not in line:
        return None, None
    key, value = line.split("=", 1)
    return key.strip(), value.strip()


def values_in_section(lines, span, key):
    """Every value for `key` inside a section, with the line numbers."""
    start, end = span
    out = []
    for index in range(start + 1, end):
        name, value = split_kv(lines[index])
        if name and name.lower() == key.lower():
            out.append((index, value))
    return out


def normalise(path):
    return path.replace("\\", "/").rstrip("/").lower()


def find_terminators(text):
    """Split into lines keeping their line endings, so CRLF is preserved."""
    lines = text.splitlines(keepends=True)
    return lines


def launcher_scan(search_dirs):
    """Reproduce the launcher's own scan, so the result can be shown before it
    is trusted. Mirrors DesktopPlatformBase.cpp:1830 - one level deep only."""
    found = []
    for directory in search_dirs:
        if not os.path.isdir(directory):
            continue
        try:
            subdirs = sorted(os.listdir(directory))
        except OSError:
            continue
        for sub in subdirs:
            candidate = os.path.join(directory, sub)
            if not os.path.isdir(candidate):
                continue
            try:
                names = os.listdir(candidate)
            except OSError:
                continue
            for name in names:
                if name.lower().endswith(".uproject"):
                    found.append(os.path.join(candidate, name))
    return found


def report(text):
    """Print what the launcher would do with the file as it stands.

    Returns the search directories it would scan, and the .uproject files that
    scan would produce.
    """
    lines = find_terminators(text)
    span = section_lines(lines, SECTION)
    if span is None:
        print("  section %s is MISSING from the file" % SECTION)
        return [], []

    # (line number, value) pairs for printing, plain values for the scan.
    search_entries = values_in_section(lines, span, KEY)
    recent = values_in_section(lines, span, RECENT_KEY)

    print("  section found, %d line(s) from %d to %d"
          % (len(lines), span[0] + 1, span[1]))

    if search_entries:
        print("  CreatedProjectPaths (%d):" % len(search_entries))
        for index, value in search_entries:
            print("      line %d: %s" % (index + 1, value))
    else:
        print("  CreatedProjectPaths: NONE - nothing is scanned for this engine")

    if recent:
        unusable = [(i, v) for i, v in recent if not v.lower().endswith(".uproject")]
        print("  RecentlyOpenedProjectFiles (%d, %d unusable):"
              % (len(recent), len(unusable)))
        for index, value in recent:
            flag = "  <-- not a path" if (index, value) in unusable else ""
            shown = value if len(value) <= 96 else value[:93] + "..."
            print("      line %d: %s%s" % (index + 1, shown, flag))

    search_dirs = [value for _, value in search_entries]
    found = launcher_scan(search_dirs)
    print("  launcher will therefore see:")
    if not found:
        print("      (nothing)")
    for item in found:
        print("      %s" % item)
    return search_dirs, found


def apply_fix(config_path, search_root, dry_run):
    text = read_config(config_path)
    lines = find_terminators(text)
    span = section_lines(lines, SECTION)
    if span is None:
        raise SystemExit("refusing to edit: section %s not found in %s"
                         % (SECTION, config_path))

    existing = values_in_section(lines, span, KEY)
    if any(normalise(value) == normalise(search_root) for _, value in existing):
        print("already present - nothing to do")
        return False

    # Backslashes, matching the shipment in 4.27's editor config, which the
    # launcher demonstrably reads (it logs the same path with forward slashes
    # after normalising). A trailing separator is trimmed on purpose.
    newline = "\r\n" if "\r\n" in text else "\n"
    entry = "%s=%s%s" % (KEY, search_root.rstrip("\\/"), newline)

    # After the last existing CreatedProjectPaths if there is one, otherwise
    # directly under the header, so the file keeps its shape.
    if existing:
        insert_at = existing[-1][0] + 1
    else:
        insert_at = span[0] + 1

    updated = "".join(lines[:insert_at]) + entry + "".join(lines[insert_at:])

    if dry_run:
        print("dry run - would insert at line %d:" % (insert_at + 1))
        print("    %s" % entry.strip())
        return False

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    backup = "%s.bak-%s" % (config_path, stamp)
    shutil.copy2(config_path, backup)
    print("backup: %s" % backup)

    with open(config_path, "wb") as handle:
        handle.write(updated.encode("utf-8"))
    print("inserted at line %d: %s" % (insert_at + 1, entry.strip()))
    return True


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Add this project to the Epic Games Launcher's project list.")
    parser.add_argument("--check", action="store_true",
                        help="report only, never write")
    parser.add_argument("--dry-run", action="store_true",
                        help="show the edit that would be made, write nothing")
    parser.add_argument("--force", action="store_true",
                        help="write even if the editor is running")
    parser.add_argument("--config", default=CONFIG,
                        help="override the EditorSettings.ini path")
    args = parser.parse_args()

    print("config : %s" % args.config)
    print("project: %s" % UPROJECT)
    print("scan   : %s  (parent dir - the scan is one level deep)" % SEARCH_ROOT)
    print()

    if not os.path.isfile(args.config):
        raise SystemExit("config not found: %s" % args.config)

    processes = running_processes()
    editors = matches(processes, EDITOR_PROCESSES)
    launchers = matches(processes, RESTART_PROCESSES)
    if editors:
        print("editor running : %s" % ", ".join(editors))
    if launchers:
        print("launcher running: %s" % ", ".join(launchers))
    print()

    text = read_config(args.config)
    print("BEFORE")
    report(text)
    print()

    if args.check:
        return 0

    if editors and not args.force:
        print("REFUSING TO WRITE")
        print("  UnrealEditor loads this file at startup and writes it back on")
        print("  shutdown, so a line added underneath it can be erased when it")
        print("  exits. Close the editor and run this again.")
        for name in EDITOR_PROCESSES:
            print("      %s" % name)
        print("  (--force overrides, at the risk of the edit being reverted)")
        return 2

    changed = apply_fix(args.config, SEARCH_ROOT, args.dry_run)
    print()

    if changed:
        text = read_config(args.config)
        print("AFTER")
        search_dirs, _ = report(text)
        print()

        # Restate the result as a pass/fail on the thing that actually matters:
        # does the launcher's own scan, replayed from the file we just wrote,
        # produce this project's .uproject? Not "did the line get written" -
        # that would still pass if the path were at the wrong depth.
        seen = [normalise(path) for path in launcher_scan(search_dirs)]
        print("VERDICT")
        if normalise(UPROJECT) in seen:
            print("  PASS - %s is now in the launcher's project list" % UPROJECT)
        else:
            print("  FAIL - the scan still does not produce %s" % UPROJECT)

        if launchers:
            print("  restart the Epic Games Launcher for it to take effect")
            print("  (it caches the project list at startup; it never writes this file)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
