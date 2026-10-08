#!/usr/bin/env python3
"""Pure packaging-policy checks; does not run ELF binaries or build an AppImage."""
import hashlib
import io
import json
import tarfile
from pathlib import Path
from tempfile import TemporaryDirectory
from build_appimage import build_source_companions, digest, host_library, publish_outputs, validate_name
from license_bundle import bundle_licenses


def check_source_companions():
    with TemporaryDirectory(prefix="pusu: source companions ") as directory:
        root = Path(directory)
        binary = root / "libfixture.so"
        binary.write_bytes(b"original library fixture")
        grant = root / "COPYING"
        grant.write_text("Copyright 2026 Fixture author.\nPermission is hereby granted to use this fixture under MIT.\n")
        source = root / "covered.tar"
        with tarfile.open(source, "w") as archive:
            payload = tarfile.TarInfo("fixture.c")
            payload.size = 8192
            archive.addfile(payload, io.BytesIO(b"A" * payload.size))
        duplicate = root / "alias.tar"
        duplicate.write_bytes(source.read_bytes())
        material = root / "build-evidence.bin"
        material.write_bytes(b"B" * 16384)
        recipe = root / "build.sh"
        recipe.write_bytes(b"cc fixture.c -shared -o libfixture.so\n")
        hashed = lambda path: {"path": path.name, "sha256": digest(path)}
        provenance = root / "provenance.json"
        provenance.write_text(json.dumps({"version": 1, "packages": [{
            "files": [binary.name], "package": "fixture", "version": "1", "licenses": ["MIT"],
            "origin": "temporary consumer fixture", "notices": [hashed(grant)],
            "source": {"archives": [hashed(source), hashed(duplicate)],
                       "corresponding_to": [digest(binary)], "recipe": hashed(recipe),
                       "build_materials": [hashed(material)]},
        }]}))

        def delivery(name):
            appdir = root / name
            licenses = appdir / "usr/share/licenses"
            records = bundle_licenses(
                [{"source": str(binary.resolve()), "packaged": "usr/lib/libfixture.so",
                  "sha256": digest(binary)}], provenance, licenses)
            stage = root / (name + "-stage")
            stage.mkdir()
            return appdir, records, stage

        appdir, records, stage = delivery("good")
        notice_files = {item["file"]: (appdir / "usr/share/licenses" / item["file"]).read_bytes()
                        for item in records[0]["files"] if item["kind"] not in {"source", "recipe", "build-material"}}
        expected = {item["file"]: (appdir / "usr/share/licenses" / item["file"]).read_bytes()
                    for item in records[0]["files"] if item["kind"] in {"source", "recipe", "build-material"}}
        companions = build_source_companions(appdir, records, stage, root / "Pusu-sources", max_bytes=30000)
        assert len(companions) == 2, companions
        good_stage = stage
        assert records[0]["source"]["companions"] == companions
        extracted = root / "extracted"
        all_members = {}
        for companion in companions:
            path = stage / companion["file"]
            assert path.stat().st_size == companion["size"] < 30000
            assert digest(path) == companion["sha256"]
            with tarfile.open(path) as archive:
                seen = {}
                regular_hashes = set()
                for member in archive:
                    content = archive.extractfile(member).read()
                    checksum = hashlib.sha256(content).hexdigest()
                    if member.islnk():
                        assert member.linkname in seen and seen[member.linkname] == checksum
                    else:
                        assert checksum not in regular_hashes, "Duplicate payload stored twice"
                        regular_hashes.add(checksum)
                    seen[member.name] = checksum
                    assert member.name not in all_members
                    all_members[member.name] = companion["file"]
                archive.extractall(extracted, filter="data")
        assert set(all_members) == {"usr/share/licenses/" + name for name in expected}
        for item in records[0]["files"]:
            original = appdir / "usr/share/licenses" / item["file"]
            if item["file"] in expected:
                location = item["location"]
                assert location["kind"] == "source-companion"
                assert all_members[location["member"]] == location["file"]
                assert (extracted / location["member"]).read_bytes() == expected[item["file"]]
                assert not original.exists()
            else:
                assert original.read_bytes() == notice_files[item["file"]]
        notice = appdir / "usr/share/licenses" / records[0]["notice"]["file"]
        assert digest(notice) == records[0]["notice"]["sha256"]
        assert records[0]["source"]["notice"] == records[0]["notice"]
        assert all(companion["file"] in notice.read_text() for companion in companions)

        for failure in ("missing", "tampered", "oversized", "collision"):
            appdir, records, stage = delivery(failure)
            first = next(item for item in records[0]["files"] if item["kind"] == "source")
            path = appdir / "usr/share/licenses" / first["file"]
            prefix = root / failure
            if failure == "missing":
                path.unlink()
            elif failure == "tampered":
                path.write_bytes(b"modified")
            elif failure == "collision":
                (root / (failure + "-001.tar")).write_bytes(b"user data")
            try:
                build_source_companions(appdir, records, stage, prefix,
                                        max_bytes=10000 if failure == "oversized" else 30000)
            except (OSError, ValueError):
                pass
            else:
                raise AssertionError(f"Companion {failure} accepted")
            assert not list(stage.iterdir())
            assert (appdir / "usr/share/licenses" / records[0]["notice"]["file"]).is_file()
            if failure == "collision":
                assert (root / (failure + "-001.tar")).read_bytes() == b"user data"

        image = good_stage / "built.AppImage"
        image.write_bytes(b"completed image")
        output = root / "final.AppImage"
        sentinel = root / companions[0]["file"]
        sentinel.write_bytes(b"user data")
        try:
            publish_outputs([(image, output), (good_stage / companions[0]["file"], sentinel)])
        except ValueError:
            pass
        else:
            raise AssertionError("Final asset collision accepted")
        assert not output.exists() and sentinel.read_bytes() == b"user data"
        sentinel.unlink()
        try:
            publish_outputs([(image, output), (image, output)])
        except FileExistsError:
            pass
        else:
            raise AssertionError("Racing/duplicate final asset accepted")
        assert not output.exists(), "Owned publication link was not rolled back"
        assets = [(good_stage / item["file"], root / item["file"]) for item in companions]
        publish_outputs([*assets, (image, output)])
        assert output.read_bytes() == b"completed image"
        for item in companions:
            assert digest(root / item["file"]) == item["sha256"]


def main():
    for name in ("libc.so.6", "libmvec.so.1", "libthread_db.so.1", "libpthread.so.0", "ld-linux-x86-64.so.2", "libGLX_mesa.so.0", "libdrm_amdgpu.so.1", "libvulkan_radeon.so", "libnvidia-glcore.so.590.1"):
        assert host_library(name), name
    for name in ("libGL.so.1", "libGLX.so.0", "libEGL.so.1", "libvulkan.so.1", "libmsi.so.0", "libstdc++.so.6", "libSDL2-2.0.so.0"):
        assert not host_library(name), name
    assert host_library("radeonsi_dri.so", Path("/usr/lib/dri/radeonsi_dri.so"))
    with TemporaryDirectory(prefix="pusu: packaging ") as directory:
        root = Path(directory)
        driver = root / "libvulkan_radeon.so"
        driver.touch()
        alias = root / "libplugin.so"
        alias.symlink_to(driver)
        assert host_library(alias.name, alias), "Driver alias escaped host exclusion"
        dri = root / "dri"
        dri.mkdir()
        driver = dri / "vendor.so"
        driver.touch()
        alias.unlink()
        alias.symlink_to(driver)
        assert host_library(alias.name, alias), "Driver directory alias escaped host exclusion"
        loader = root / "libvulkan.so.1"
        loader.touch()
        alias.unlink()
        alias.symlink_to(loader)
        assert not host_library(alias.name, alias), "Portable Vulkan loader was excluded"
    assert validate_name("libmsi.so.0") == "libmsi.so.0"
    for name in ("", ".", "..", "../libc.so.6", "/tmp/libbad.so", "libbad\x00.so"):
        try:
            validate_name(name)
        except ValueError:
            pass
        else:
            raise AssertionError(f"Unsafe ELF dependency accepted: {name!r}")
    check_source_companions()


if __name__ == "__main__":
    main()
