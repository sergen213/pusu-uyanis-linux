#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build only retained libdbus; stage in a new directory, never install on the host."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile


ARCHIVE = "dbus-958bf9db2100553bcd2fe2a854e1ebb42e886054.tar.gz"
INPUT_HASHES = {
    ARCHIVE: "482fdd0b706a93d0fd7bd2cd192d9d264360de3d6d8d1b29de6f165195fed708",
    "0001-Arch-Linux-tweaks.patch": "df115a7dbd8701903259f8c73a31052d72b7ecd0fd1b723ff25b5073a1549166",
}
ORIGINAL_HASH = "accec52359b9195508051d2b4eda61a39659432c80d3590e8023bf5136de6f6c"
ELF = "libdbus-1.so.3.38.3"
OPTIONS = [
    "--backend=ninja", "--wrap-mode=nofallback", "--buildtype=debugoptimized",
    "--default-library=shared", "--prefix=/usr", "--libdir=lib", "--libexecdir=lib",
    "--bindir=bin", "--includedir=include", "--datadir=share",
    "--sysconfdir=/etc", "--localstatedir=/var", "-Db_lto=false", "-Db_ndebug=false",
    "-Dapparmor=disabled", "-Ddbus_user=dbus", "-Dkqueue=disabled",
    "-Dlaunchd=disabled", "-Drelocation=disabled", "-Dselinux=disabled",
    "-Dx11_autolaunch=disabled", "-Dsystemd=enabled", "-Depoll=enabled",
    "-Dinotify=enabled", "-Dlibaudit=auto", "-Dstats=true", "-Dchecks=true",
    "-Dasserts=false", "-Dverbose_mode=false", "-Dvalgrind=disabled",
    "-Dintrusive_tests=false", "-Dmodular_tests=disabled", "-Dinstalled_tests=false",
    "-Dmessage_bus=false", "-Dtools=false", "-Ddoxygen_docs=disabled",
    "-Dducktype_docs=disabled", "-Dqt_help=disabled", "-Dxml_docs=disabled",
    "-Dtraditional_activation=true", "-Duser_session=true", "-Druntime_dir=/run",
    "-Dsession_socket_dir=/tmp", "-Dsystem_socket=/run/dbus/system_bus_socket",
    "-Ddbus_session_bus_connect_address=autolaunch:",
    "-Ddbus_session_bus_listen_address=unix:tmpdir=/tmp",
]
HEADERS = """dbus-address.h dbus-bus.h dbus-connection.h dbus-errors.h dbus-macros.h
    dbus-memory.h dbus-message.h dbus-misc.h dbus-pending-call.h dbus-protocol.h
    dbus-server.h dbus-shared.h dbus-signature.h dbus-syntax.h dbus-threads.h
    dbus-types.h dbus.h""".split()


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", type=Path, required=True, help="New directory; parent must exist")
    parser.add_argument("--original-elf", type=Path, required=True, help="Read-only selected installed ELF")
    args = parser.parse_args()
    if sys.platform != "linux" or os.geteuid() == 0:
        parser.error("Run as a non-root user on Linux")
    package = Path(__file__).resolve().parent
    bundle = package.parent.parent
    inputs = {
        ARCHIVE: bundle / "sources/dbus-1.16.2" / ARCHIVE,
        "0001-Arch-Linux-tweaks.patch": package / "0001-Arch-Linux-tweaks.patch",
    }
    for name, path in inputs.items():
        if digest(path) != INPUT_HASHES[name]:
            parser.error(f"Retained input hash mismatch: {path}")
    original = args.original_elf.resolve(strict=True)
    if digest(original) != ORIGINAL_HASH:
        parser.error("Original ELF differs from the selected installed library")
    work = args.work_dir.resolve()
    if any(work.is_relative_to(Path(p)) for p in ("/usr", "/etc", "/var", "/run", "/bin", "/sbin", "/lib", "/lib64")):
        parser.error("Work directory must be outside host installation/runtime trees")
    work.mkdir()  # Fail closed for any existing directory; never clean or overwrite it.
    for name in ("inputs", "logs", "home", "tmp", "original", "DESTDIR"):
        (work / name).mkdir()
    env = {
        "PATH": "/usr/bin:/bin", "HOME": str(work / "home"), "TMPDIR": str(work / "tmp"),
        "LANG": "C.UTF-8", "LC_ALL": "C.UTF-8", "CC": "cc", "CFLAGS": "",
        "CPPFLAGS": "", "LDFLAGS": "", "PKG_CONFIG": "pkg-config", "DESTDIR": str(work / "DESTDIR"),
    }
    report = {"status": "running", "environment": env, "inputs": INPUT_HASHES,
              "script_sha256": digest(Path(__file__).resolve()),
              "original_elf": {"path": str(original), "sha256": ORIGINAL_HASH},
              "python": {"path": sys.executable, "version": sys.version},
              "commands": [], "tools": {}, "outputs": []}

    def save():
        (work / "execution.json").write_text(json.dumps(report, indent=2) + "\n")

    def run(argv, label, cwd=work):
        record = {"argv": [str(a) for a in argv], "cwd": str(cwd), "log": f"logs/{label}.log"}
        report["commands"].append(record)
        save()
        with (work / record["log"]).open("wb") as log:
            result = subprocess.run(record["argv"], cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT)
        record["returncode"] = result.returncode
        save()
        result.check_returncode()

    try:
        for name, path in inputs.items():
            shutil.copyfile(path, work / "inputs" / name)
            if digest(work / "inputs" / name) != INPUT_HASHES[name]:
                raise RuntimeError(f"Copied input hash mismatch: {name}")
        shutil.copyfile(original, work / "original" / ELF)
        if digest(work / "original" / ELF) != ORIGINAL_HASH:
            raise RuntimeError("Selected ELF changed while being retained")
        shutil.copyfile(Path(__file__).resolve(), work / "inputs/build-owned-client.py")
        for tool in ("cc", "meson", "ninja", "pkg-config", "patch", "readelf"):
            found = shutil.which(tool, path=env["PATH"])
            if found is None:
                raise RuntimeError(f"Missing prerequisite: {tool}")
            report["tools"][tool] = {"path": found, "sha256": digest(Path(found).resolve())}
            run([found, "--version"], f"version-{tool}")
        run(["pkg-config", "--modversion", "libsystemd"], "systemd-version")
        run(["pkg-config", "--cflags", "--libs", "libsystemd"], "systemd-build-flags")
        with tarfile.open(work / "inputs" / ARCHIVE) as archive:
            archive.extractall(work / "unpacked", filter="data")
        source = work / "source"
        (work / "unpacked" / "dbus-958bf9db2100553bcd2fe2a854e1ebb42e886054").rename(source)
        run(["patch", "--batch", "--forward", "--fuzz=0", "-Np1", "-i",
             work / "inputs/0001-Arch-Linux-tweaks.patch"], "prepare", cwd=source)
        prepared_date = datetime.now(timezone.utc).date().isoformat()
        report["preparation_modifications"] = {
            "date_utc": prepared_date,
            "paths": ["bus/meson.build", "bus/sysusers.d/dbus.conf.in"],
            "patch": "0001-Arch-Linux-tweaks.patch",
            "patch_author": "Jan Alexander Steffens (heftig)",
            "patch_date": "2024-12-17",
        }
        for name in report["preparation_modifications"]["paths"]:
            path = source / name
            path.write_text(
                f"# Modified during recipient preparation on {prepared_date}: retained Arch Linux tweaks\n"
                "# by Jan Alexander Steffens (heftig), patch dated 2024-12-17; see retained inputs/ patch.\n"
                + path.read_text()
            )
        (work / "prepared-source-hashes.json").write_text(json.dumps([
            {"path": str(path.relative_to(source)), "sha256": digest(path)}
            for path in sorted(source.rglob("*")) if path.is_file()
        ], indent=2) + "\n")
        build = work / "build"
        run(["meson", "setup", build, source, *OPTIONS], "configure")
        config = (build / "config.h").read_text()
        for feature in ("HAVE_UNIX_FD_PASSING", "HAVE_SYSTEMD", "DBUS_ENABLE_CHECKS", "DBUS_ENABLE_STATS", "DBUS_HAVE_LINUX_EPOLL"):
            if not re.search(r"^#define " + feature + r"(?:\s+1)?\s*$", config, re.MULTILINE):
                raise RuntimeError(f"Required Linux client configuration missing: {feature}")
        run(["meson", "introspect", "--buildoptions", build], "build-options")
        run(["meson", "introspect", "--dependencies", build], "dependencies")
        run(["meson", "introspect", "--targets", build], "targets")
        run(["ninja", "-C", build, "dbus/" + ELF], "compile-selected-library")
        built = build / "dbus" / ELF
        run(["readelf", "-h", "-d", "-Ws", built], "output-elf")
        prefix = work / "DESTDIR/usr"
        lib = prefix / "lib"
        lib.mkdir(parents=True)
        shutil.copy2(built, lib / ELF)
        (lib / "libdbus-1.so.3").symlink_to(ELF)
        (lib / "libdbus-1.so").symlink_to("libdbus-1.so.3")
        include = prefix / "include/dbus-1.0/dbus"
        include.mkdir(parents=True)
        for header in HEADERS:
            shutil.copy2(source / "dbus" / header, include / header)
        arch_include = lib / "dbus-1.0/include/dbus"
        arch_include.mkdir(parents=True)
        shutil.copy2(build / "dbus/dbus-arch-deps.h", arch_include / "dbus-arch-deps.h")
        (lib / "pkgconfig").mkdir()
        shutil.copy2(build / "meson-private/dbus-1.pc", lib / "pkgconfig/dbus-1.pc")
        notices = prefix / "share/licenses/dbus"
        notices.mkdir(parents=True)
        shutil.copytree(source / "LICENSES", notices / "LICENSES")
        for name in ("COPYING", "AUTHORS"):
            shutil.copy2(source / name, notices / name)
        shutil.copy2(package / "NOTICE.txt", notices / "NOTICE.txt")
        report["outputs"] = [
            {"path": str(path.relative_to(work)), "symlink": os.readlink(path)} if path.is_symlink()
            else {"path": str(path.relative_to(work)), "sha256": digest(path)}
            for path in sorted((work / "DESTDIR").rglob("*")) if path.is_file() or path.is_symlink()
        ]
        report["owned_elf"] = {"path": "DESTDIR/usr/lib/" + ELF, "sha256": digest(lib / ELF)}
        report["artifacts"] = [
            {"path": str(path.relative_to(work)), "sha256": digest(path)}
            for path in [work / "prepared-source-hashes.json", build / "config.h",
                         build / "dbus/dbus-arch-deps.h", build / "dbus/version_script",
                         build / "meson-private/dbus-1.pc", build / "build.ninja",
                         build / "compile_commands.json", *sorted((work / "logs").glob("*.log")),
                         *sorted((build / "meson-info").glob("*.json"))]
        ]
        report["status"] = "complete"
    except BaseException as error:
        report["status"] = "failed"
        report["error"] = str(error)
        raise
    finally:
        save()  # Keep full source, generated build files and all logs even on failure.


if __name__ == "__main__":
    main()
