#!/usr/bin/env python3
"""Pure packaging-policy checks; does not run ELF binaries or build an AppImage."""
from pathlib import Path
from tempfile import TemporaryDirectory
from build_appimage import host_library, validate_name


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


if __name__ == "__main__":
    main()
