#!/usr/bin/env python3
"""Package the export templates built from this tree into a .tpz archive.

A `.tpz` is what "Editor > Manage Export Templates > Install from File"
expects. It is a plain zip archive that contains a `version.txt` and, next to
it, the template files under the names the editor looks them up by (see
`editor/export/export_template_manager.cpp`, `ExportTemplateManager::_tpz_file_selected()`):

- The archive must contain a `version.txt` whose stripped content has at least
  three dot separated components (`major.minor[.patch].status`) - for example
  `4.7.2.stable`. Its content decides the directory the files are installed
  into (`<data>/godot/export_templates/<version>/`).
- Entries are extracted relative to the directory that contains `version.txt`,
  which is why the files are stored under `templates/` here (the official
  layout). A flat archive with `version.txt` at the root works too.
- Unix permissions stored in the zip are restored, so the templates have to be
  added as executable (0755); otherwise an exported Linux game cannot run.
- Directory entries and `__MACOSX` entries are ignored.

Only the names matter, not the source names: a build produces
`godot.linuxbsd.template_release.x86_64` while the editor looks up
`linux_release.x86_64`. The mapping below is the same one used by
`ExportTemplateManager::_initialize_template_data()`.

Usage (from anywhere):

    python3 misc/scripts/make_export_templates_tpz.py

Produces `bin/Godot_v<version>_export_templates.tpz` with every template found
in `bin/`. Select platforms with `--platforms linux,web`, point at another
build directory with `--bin-dir`, or add a build suffix (for example
`--suffix nohaxe`, `--suffix mono`, `--suffix haxe_mono.mono`) when the
templates were built with `extra_suffix=`/`module_mono_enabled=yes`.
"""

import argparse
import sys
import zipfile
from pathlib import Path

if __name__ != "__main__":
    raise ImportError(f"{__name__} should not be used as a module.")

REPO_ROOT = Path(__file__).resolve().parents[2]

# platform, name in the templates directory, file name produced by a build.
# `executable` is the unix mode the editor restores when it extracts the file.
TEMPLATES = [
    ("linux", "linux_debug.x86_64", "godot.linuxbsd.template_debug.x86_64", True),
    ("linux", "linux_release.x86_64", "godot.linuxbsd.template_release.x86_64", True),
    ("linux", "linux_debug.x86_32", "godot.linuxbsd.template_debug.x86_32", True),
    ("linux", "linux_release.x86_32", "godot.linuxbsd.template_release.x86_32", True),
    ("linux", "linux_debug.arm32", "godot.linuxbsd.template_debug.arm32", True),
    ("linux", "linux_release.arm32", "godot.linuxbsd.template_release.arm32", True),
    ("linux", "linux_debug.arm64", "godot.linuxbsd.template_debug.arm64", True),
    ("linux", "linux_release.arm64", "godot.linuxbsd.template_release.arm64", True),
    ("web", "web_debug.zip", "godot.web.template_debug.wasm32.zip", False),
    ("web", "web_release.zip", "godot.web.template_release.wasm32.zip", False),
    ("web", "web_nothreads_debug.zip", "godot.web.template_debug.wasm32.nothreads.zip", False),
    ("web", "web_nothreads_release.zip", "godot.web.template_release.wasm32.nothreads.zip", False),
    ("web", "web_dlink_debug.zip", "godot.web.template_debug.wasm32.dlink.zip", False),
    ("web", "web_dlink_release.zip", "godot.web.template_release.wasm32.dlink.zip", False),
    ("web", "web_dlink_nothreads_debug.zip", "godot.web.template_debug.wasm32.nothreads.dlink.zip", False),
    ("web", "web_dlink_nothreads_release.zip", "godot.web.template_release.wasm32.nothreads.dlink.zip", False),
    ("android", "android_debug.apk", "android_debug.apk", False),
    ("android", "android_release.apk", "android_release.apk", False),
    ("android", "android_source.zip", "android_source.zip", False),
    ("macos", "macos.zip", "godot.macos.template_release.universal.zip", False),
    ("ios", "ios.zip", "godot.ios.template_release.universal.zip", False),
    ("windows", "windows_debug_x86_32.exe", "godot.windows.template_debug.x86_32.exe", True),
    ("windows", "windows_release_x86_32.exe", "godot.windows.template_release.x86_32.exe", True),
    ("windows", "windows_debug_x86_64.exe", "godot.windows.template_debug.x86_64.exe", True),
    ("windows", "windows_release_x86_64.exe", "godot.windows.template_release.x86_64.exe", True),
    ("windows", "windows_debug_arm64.exe", "godot.windows.template_debug.arm64.exe", True),
    ("windows", "windows_release_arm64.exe", "godot.windows.template_release.arm64.exe", True),
    # Console wrappers. The editor's Windows template sets list a wrapper for
    # both targets (`ExportTemplateManager::_initialize_template_data()`), and
    # SCons builds one next to each template when `windows_subsystem=gui`.
    (
        "windows",
        "windows_debug_x86_32_console.exe",
        "godot.windows.template_debug.x86_32.console.exe",
        True,
    ),
    (
        "windows",
        "windows_debug_x86_64_console.exe",
        "godot.windows.template_debug.x86_64.console.exe",
        True,
    ),
    (
        "windows",
        "windows_debug_arm64_console.exe",
        "godot.windows.template_debug.arm64.console.exe",
        True,
    ),
    (
        "windows",
        "windows_release_x86_32_console.exe",
        "godot.windows.template_release.x86_32.console.exe",
        True,
    ),
    (
        "windows",
        "windows_release_x86_64_console.exe",
        "godot.windows.template_release.x86_64.console.exe",
        True,
    ),
    (
        "windows",
        "windows_release_arm64_console.exe",
        "godot.windows.template_release.arm64.console.exe",
        True,
    ),
]

# The marker string every binary built with modules/haxe contains (the
# `HaxeScript` class name). A template without it cannot load Haxe scripts, so
# it is reported: Linux and Windows templates as plain files, Android APKs
# through the engine library they carry.
HAXE_MARKER = b"HaxeScript"


def read_version():
    """major.minor.patch.status[.module_config] from version.py."""
    namespace = {}
    with open(REPO_ROOT / "version.py", "r", encoding="utf-8") as file:
        exec(file.read(), namespace)  # noqa: S102 - trusted, in-tree file.
    version = "{}.{}.{}.{}".format(namespace["major"], namespace["minor"], namespace["patch"], namespace["status"])
    module_config = namespace.get("module_config", "")
    if module_config:
        version += f".{module_config}"
    return version


def build_name(base, suffix):
    """`godot.linuxbsd.template_release.x86_64` + build suffixes."""
    if not suffix:
        return base
    return f"{base}.{suffix.lstrip('.')}"


def collect(bin_dir, platforms, suffix):
    found = []
    missing = []
    for platform, lookup_name, source_name, executable in TEMPLATES:
        if platform not in platforms:
            continue
        source = bin_dir / build_name(source_name, suffix)
        if source.is_file():
            found.append((lookup_name, source, executable))
        else:
            missing.append((platform, source))
    return found, missing


def contains_marker(path, marker, chunk_size=1 << 20):
    overlap = len(marker) - 1
    tail = b""
    with open(path, "rb") as file:
        while True:
            chunk = file.read(chunk_size)
            if not chunk:
                return False
            if marker in tail + chunk:
                return True
            tail = (tail + chunk)[-overlap:] if overlap else b""


def apk_contains_marker(path, marker):
    """Whether an APK's engine libraries contain `marker`.

    APKs store their native libraries deflated (and may carry a 64-bit and a
    32-bit engine), so the search has to look inside each `.so` entry rather
    than at the file bytes.
    """
    with zipfile.ZipFile(path) as archive:
        for name in archive.namelist():
            if name.startswith("lib/") and name.endswith(".so"):
                if marker in archive.read(name):
                    return True
    return False


def template_contains_marker(lookup_name, source):
    # The console wrapper is a small launcher that re-executes the real
    # template next to it, so it carries no engine code of its own.
    if lookup_name.endswith("_console.exe"):
        return True
    if lookup_name.startswith(("linux_", "windows_")):
        return contains_marker(source, HAXE_MARKER)
    if lookup_name.startswith("android_") and lookup_name.endswith(".apk"):
        return apk_contains_marker(source, HAXE_MARKER)
    return True


def write_tpz(output, version, entries, prefix):
    output.parent.mkdir(parents=True, exist_ok=True)
    contents_dir = prefix.strip("/") if prefix else ""
    version_entry = f"{contents_dir}/version.txt" if contents_dir else "version.txt"

    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        info = zipfile.ZipInfo(version_entry, date_time=(1980, 1, 1, 0, 0, 0))
        info.external_attr = 0o644 << 16
        info.compress_type = zipfile.ZIP_DEFLATED
        archive.writestr(info, f"{version}\n")

        for lookup_name, source, executable in entries:
            arcname = f"{contents_dir}/{lookup_name}" if contents_dir else lookup_name
            # Templates that are archives themselves are already compressed.
            compressed = not lookup_name.endswith(".zip")
            info = zipfile.ZipInfo(arcname, date_time=(1980, 1, 1, 0, 0, 0))
            info.external_attr = (0o755 if executable else 0o644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED if compressed else zipfile.ZIP_STORED
            with open(source, "rb") as file:
                archive.writestr(info, file.read(), compresslevel=9 if compressed else None)


def human_size(size):
    for unit in ("B", "KiB", "MiB", "GiB"):
        if size < 1024 or unit == "GiB":
            return f"{size:.1f} {unit}" if unit != "B" else f"{size} B"
        size /= 1024
    return f"{size:.1f} GiB"


def main():
    parser = argparse.ArgumentParser(
        description="Package built export templates into a .tpz archive.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "--bin-dir",
        type=Path,
        default=REPO_ROOT / "bin",
        help="directory with the built templates (default: <tree>/bin)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="archive to write (default: <bin-dir>/Godot_v<version>_export_templates.tpz)",
    )
    parser.add_argument(
        "--version",
        default=None,
        help="version.txt content (default: read from version.py)",
    )
    parser.add_argument(
        "--platforms",
        default=None,
        help="comma separated platforms to include, e.g. 'linux,web' (default: every platform with a built template)",
    )
    parser.add_argument(
        "--suffix",
        default="",
        help="build suffix after the architecture, e.g. 'nohaxe', 'mono', 'haxe_mono.mono'",
    )
    parser.add_argument(
        "--prefix",
        default="templates",
        help="directory inside the archive that holds version.txt (default: templates, empty for a flat archive)",
    )
    parser.add_argument(
        "--no-haxe-check",
        action="store_true",
        help="do not warn about templates without the Haxe marker",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        dest="list_only",
        help="print what would be packaged and exit",
    )
    args = parser.parse_args()

    version = args.version or read_version()
    if version.count(".") < 2:
        parser.error(f"invalid version '{version}': need at least major.minor.status")

    if args.platforms is None:
        # Every platform that has at least one built template. Nothing to warn
        # about: a tree that only builds Linux and web is normal.
        platforms = {platform for platform, _name, _source, _exec in TEMPLATES}
        explicit_platforms = False
    else:
        platforms = {name.strip() for name in args.platforms.split(",") if name.strip()}
        explicit_platforms = True

    entries, missing = collect(args.bin_dir, platforms, args.suffix)
    if not entries:
        print(f"No export templates found in {args.bin_dir}.", file=sys.stderr)
        if missing:
            print("Expected one of:", file=sys.stderr)
            for _platform, source in missing[:4]:
                print(f"  {source}", file=sys.stderr)
        return 1

    if not args.no_haxe_check:
        for lookup_name, source, _executable in entries:
            if not template_contains_marker(lookup_name, source):
                print(
                    f"warning: {source.name} does not contain the Haxe module; "
                    "a Haxe project cannot use it as an export template.",
                    file=sys.stderr,
                )

    total = sum(source.stat().st_size for _lookup, source, _exec in entries)
    print(f"Version: {version}")
    for lookup_name, source, executable in entries:
        mode = "0755" if executable else "0644"
        print(f"  {lookup_name:<40} <- {source.name} ({human_size(source.stat().st_size)}, {mode})")
    if explicit_platforms:
        for platform, source in missing:
            print(f"  (skipped {platform}: {source.name} not built)", file=sys.stderr)

    if args.list_only:
        print(f"Total: {human_size(total)}")
        return 0

    output = args.output or args.bin_dir / f"Godot_v{version}_export_templates.tpz"
    write_tpz(output, version, entries, args.prefix)

    print(f"Wrote {output} ({human_size(output.stat().st_size)} from {human_size(total)} of templates).")
    print("Install it with Editor > Manage Export Templates > Install from File,")
    print("or extract it into the versioned templates directory by hand:")
    print("  unzip -j -o {} -d ~/.local/share/godot/export_templates/{}".format(output, version))
    return 0


if __name__ == "__main__":
    sys.exit(main())
