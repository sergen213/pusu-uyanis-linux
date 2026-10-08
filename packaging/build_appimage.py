#!/usr/bin/env python3
"""Build-time only: package native ELF binaries, their dependency closure and notices."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile

from license_bundle import bundle_licenses

HERE = Path(__file__).resolve().parent
BINARIES = ("pusu-installer", "pusu-game", "pusu-launcher")
# Keep the host dynamic loader, libc ABI and hardware-specific driver stack together.
HOST_LIBRARIES = re.compile(
    r"^(?:ld-linux.*|lib(?:c|m|mvec|BrokenLocale|c_malloc_debug|thread_db|dl|pthread|rt|resolv|util|anl|nss_[^.]+)\.so(?:\..*)?"
    r"|lib(?:GLX_mesa|EGL_mesa|gbm|drm[^.]*|vulkan_(?:radeon|intel[^.]*|lvp|nouveau))\.so(?:\..*)?"
    r"|lib(?:nvidia[^.]*|cuda|nvcuvid)\.so(?:\..*)?)$"
)


def run(*args):
    return subprocess.check_output(args, text=True, env={**os.environ, "LC_ALL": "C"}).strip()


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def tool(name):
    found = shutil.which(name)
    if not found:
        raise ValueError(f"Required build-time tool is missing: {name}")
    return found


def elf_arch(path):
    with path.open("rb") as stream:
        header = stream.read(20)
    if len(header) != 20 or header[:4] != b"\x7fELF" or header[4:6] != b"\x02\x01":
        raise ValueError(f"Expected a native little-endian 64-bit ELF: {path}")
    machine = struct.unpack_from("<H", header, 18)[0]
    if machine != 62:
        raise ValueError(f"This AppImage recipe targets x86_64, not ELF machine {machine}: {path}")


def validate_name(name):
    if not name or "/" in name or name in (".", "..") or "\x00" in name:
        raise ValueError(f"Unsafe ELF dependency/SONAME: {name!r}")
    return name


def host_library(name, path=None):
    if HOST_LIBRARIES.fullmatch(name):
        return True
    if path is None:
        return False
    # --module and dependency search roots can expose drivers through aliases.
    resolved = path.resolve()
    return bool(HOST_LIBRARIES.fullmatch(resolved.name)) or any(
        part in ("dri", "vdpau") for part in (*path.parts, *resolved.parts)
    )


def ld_cache():
    result = {}
    for line in run(tool("ldconfig"), "-p").splitlines():
        match = re.match(r"\s*(\S+)\s+\([^)]*x86-64[^)]*\)\s+=>\s+(\S+)", line)
        if match:
            result.setdefault(match[1], Path(match[2]))
    return result


def library_paths(binary, patchelf):
    paths = []
    for entry in run(patchelf, "--print-rpath", str(binary)).split(":"):
        if not entry:
            continue
        entry = entry.replace("${ORIGIN}", str(binary.parent)).replace("$ORIGIN", str(binary.parent))
        # Never reinterpret another loader token or a cwd-relative path.
        if "$" not in entry and Path(entry).is_absolute():
            paths.append(Path(entry))
    return paths


def copy_closure(binaries, library_dirs, appdir, patchelf, modules):
    cache = ld_cache()
    library_destination = appdir / "usr/lib"
    library_destination.mkdir(parents=True)
    queue = []
    records = []
    copied = {}
    names = {}
    aliases = {}
    external = set()

    def copy_library(source, alias=None):
        source = source.resolve(strict=True)
        elf_arch(source)
        checksum = digest(source)
        soname = run(patchelf, "--print-soname", str(source))
        filename = validate_name(soname or source.name)
        for name in (filename, alias):
            if name is None:
                continue
            validate_name(name)
            if name in names and names[name] != checksum:
                raise ValueError(f"Different libraries claim {name}: {source}")
            names[name] = checksum
        if filename not in copied:
            target = library_destination / filename
            shutil.copy2(source, target)
            copied[filename] = checksum
            records.append({"source": str(source), "packaged": str(target.relative_to(appdir)), "sha256": checksum})
            queue.append((source, target))
        if alias and alias != filename:
            aliases[alias] = filename
        return library_destination / filename

    for source in binaries:
        source = source.resolve(strict=True)
        elf_arch(source)
        target = appdir / "usr/bin" / source.name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        target.chmod(target.stat().st_mode | 0o111)
        queue.append((source, target))
        records.append({"source": str(source), "packaged": str(target.relative_to(appdir)), "sha256": digest(source)})
    for module in modules:
        if host_library(module.name, module):
            raise ValueError(f"Host graphics driver must not be bundled: {module}")
        copy_library(module)
    visited = set()
    while queue:
        source, target = queue.pop()
        if source in visited:
            continue
        visited.add(source)
        search = [*library_dirs, *library_paths(source, patchelf)]
        needed = run(patchelf, "--print-needed", str(source)).splitlines()
        # Original OGG music needs SDL_mixer's runtime-loaded Vorbis decoder.
        if "SDL2_mixer" in source.name and "libvorbisfile.so.3" not in needed:
            needed.append("libvorbisfile.so.3")
        # CachyOS SDL2 is sdl2-compat and dlopens the literal SDL3 SONAME.
        if source.name.startswith("libSDL2-2.0") and b"libSDL3.so.0\0" in source.read_bytes():
            needed.append("libSDL3.so.0")
        for name in needed:
            validate_name(name)
            if host_library(name):
                external.add(name)
                continue
            dependency = next((directory / name for directory in search if (directory / name).is_file()), cache.get(name))
            if dependency is None or not dependency.is_file():
                raise ValueError(f"Unresolved dependency {name} required by {source}; supply --library-dir")
            if host_library(name, dependency):
                external.add(name)
                continue
            copy_library(dependency, name)
        rpath = "$ORIGIN/../lib" if target.parent.name == "bin" else "$ORIGIN"
        subprocess.run([patchelf, "--set-rpath", rpath, str(target)], check=True)
    for alias, filename in aliases.items():
        # Regular copies preserve importer's no-symlinks safety contract.
        shutil.copy2(library_destination / filename, library_destination / alias)
        next(record for record in records if record["packaged"] == f"usr/lib/{filename}").setdefault("aliases", []).append(f"usr/lib/{alias}")
    return records, sorted(external)


def copy_art(source, target):
    # GUI artwork is decoded directly through native libpng, not Glycin helpers.
    with source.open("rb") as stream:
        if stream.read(8) != b"\x89PNG\r\n\x1a\n":
            raise ValueError(f"Artwork must be PNG: {source}")
    shutil.copy2(source, target)


def package_schemas(appdir):
    schema_dir = appdir / "usr/share/glib-2.0/schemas"
    schema_dir.mkdir(parents=True)
    records = []
    for name in ("ColorChooser", "Debug", "EmojiChooser", "FileChooser"):
        source = Path("/usr/share/glib-2.0/schemas") / f"org.gtk.Settings.{name}.gschema.xml"
        target = schema_dir / source.name
        shutil.copy2(source, target)
        records.append({"source": str(source), "packaged": str(target.relative_to(appdir)), "sha256": digest(source)})
    subprocess.run([tool("glib-compile-schemas"), "--strict", str(schema_dir)], check=True)
    return records


def package(args):
    patchelf = str(args.patchelf.resolve(strict=True)) if args.patchelf else tool("patchelf")
    appimagetool = tool("appimagetool")
    binaries = [args.build_dir.resolve() / name for name in BINARIES]
    missing = [str(path) for path in binaries if not path.is_file()]
    if missing:
        raise ValueError("Native binaries are not built: " + ", ".join(missing))
    output = args.output.resolve()
    if output.exists():
        raise ValueError(f"Refusing to overwrite existing output: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    runtime = args.runtime_file.resolve(strict=True)
    elf_arch(runtime)
    runtime_record = {"source": str(runtime), "packaged": "AppImage/runtime", "sha256": digest(runtime)}
    art_record = json.loads(args.art_provenance.read_text(encoding="utf-8"))
    for field, path in (("installer", args.art), ("icon", args.icon)):
        entry = art_record.get(field, {})
        if entry.get("sha256") != digest(path) or not entry.get("source") or not entry.get("permission"):
            raise ValueError(f"Unresolved artwork provenance or checksum mismatch: {field}")
    with tempfile.TemporaryDirectory(prefix="pusu-appimage-", dir=output.parent) as directory:
        appdir = Path(directory) / "Pusu.AppDir"
        share = appdir / "usr/share/pusu"
        share.mkdir(parents=True)
        records, external = copy_closure(binaries, [path.resolve(strict=True) for path in args.library_dir], appdir, patchelf, args.module)
        notices = bundle_licenses([*records, runtime_record], args.provenance.resolve(strict=True),
                                 appdir / "usr/share/licenses", strict=args.redistributable)
        schemas = package_schemas(appdir)
        copy_art(args.art, share / "installer.png")
        copy_art(args.icon, share / "pusu.png")
        shutil.copy2(HERE / "art/NOTICE.txt", share / "art-NOTICE.txt")
        shutil.copy2(HERE / "pusu.desktop", appdir / "pusu.desktop")
        (appdir / "AppRun").symlink_to("usr/bin/pusu-installer")
        (appdir / "pusu.png").symlink_to("usr/share/pusu/pusu.png")
        (appdir / ".DirIcon").symlink_to("pusu.png")
        (share / "art-provenance.json").write_text(json.dumps(art_record, indent=2) + "\n", encoding="utf-8")
        manifest = {
            "format": 1, "architecture": "x86_64", "entrypoint": "usr/bin/pusu-installer",
            "apprun": {"kind": "native-elf-symlink", "target": "usr/bin/pusu-installer"},
            "distribution_policy": "strict-dependency-provenance" if args.redistributable else "private-local-use",
            "public_release_notice": "No public release or new project/artwork licence is granted by this build. Review recorded redistribution obligations before redistribution.",
            "libraries": records, "host_libraries": external, "packages": notices,
            "appimage_runtime": runtime_record,
            "gtk_schemas": schemas,
            "artwork": art_record, "game_assets": "Not bundled; extracted from the user's own disc image.",
            "build_tools": {
                "patchelf": {"path": str(Path(patchelf).resolve()), "version": run(patchelf, "--version"),
                             "sha256": digest(Path(patchelf))},
                "appimagetool": {"path": str(Path(appimagetool).resolve()), "sha256": digest(Path(appimagetool))},
            },
        }
        for record in records:
            record["packaged_sha256"] = digest(appdir / record["packaged"])
        (share / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        image = Path(directory) / "Pusu-x86_64.AppImage"
        # This appimagetool leaves C locale uninitialized; GLib STRING options
        # need its charset override to accept UTF-8 --runtime-file arguments.
        subprocess.run([appimagetool, "--no-appstream", "--runtime-file", str(runtime), str(appdir), str(image)], check=True,
                       env={**os.environ, "LC_ALL": "C.UTF-8", "LANG": "C.UTF-8", "CHARSET": "UTF-8",
                            "ARCH": "x86_64", "APPIMAGE_EXTRACT_AND_RUN": "1"})
        # Atomic publication refuses a racing output instead of replacing user data.
        os.link(image, output)
    print(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--patchelf", type=Path,
                        help="Explicit verified build-time patchelf executable; avoids colon-sensitive PATH entries")
    parser.add_argument("--runtime-file", required=True, type=Path,
                        help="Exact x86_64 AppImage type-2 runtime, covered by the provenance manifest")
    parser.add_argument("--library-dir", action="append", default=[], type=Path,
                        help="Additional native library search root, in order (e.g. private libmsi root)")
    parser.add_argument("--module", action="append", default=[], type=Path,
                        help="Additional runtime-dlopen ELF plugin; package its actual closure too")
    parser.add_argument("--provenance", type=Path, default=HERE / "provenance.json")
    parser.add_argument("--redistributable", action="store_true",
                        help="Require complete dependency copyright/source evidence; does not grant project or artwork redistribution rights")
    parser.add_argument("--art", type=Path, default=HERE / "art/installer.png")
    parser.add_argument("--icon", type=Path, default=HERE / "art/pusu.png")
    parser.add_argument("--art-provenance", type=Path, default=HERE / "art/provenance.json")
    args = parser.parse_args()
    try:
        package(args)
    except (OSError, ValueError, subprocess.CalledProcessError, json.JSONDecodeError) as error:
        parser.exit(1, f"Packaging failed: {error}\n")


if __name__ == "__main__":
    main()
