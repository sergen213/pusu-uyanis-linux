#!/usr/bin/env python3
"""Run against a built pusu-game; these checks never require game data or a display."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def check(binary: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="pusu-cli-") as directory:
        root = Path(directory)
        environment = dict(os.environ, HOME=str(root), XDG_DATA_HOME=str(root / "data"),
                           XDG_CONFIG_HOME=str(root / "config"),
                           SDL_VIDEODRIVER="pusu-deliberately-unavailable-driver")

        def run(*arguments: str) -> subprocess.CompletedProcess[str]:
            return subprocess.run([str(binary), *arguments], env=environment,
                                  text=True, capture_output=True, timeout=15)

        result = run("--list-renderers")
        assert result.returncode == 0, result.stderr
        backends = result.stdout.splitlines()
        assert backends and len(backends) == len(set(backends)), result.stdout
        assert set(backends) == {"opengl", "vulkan"}, backends
        assert all(name and not any(char.isspace() for char in name) for name in backends)
        assert not result.stderr, result.stderr
        assert run("--help").returncode == 0
        for backend in backends:
            assert run("--renderer", backend, "--help").returncode == 0
        for arguments in (("--renderer", "does-not-exist-native-renderer"),
                          ("--data",), ("--settings",), ("--unknown",),
                          ("--list-renderers", "--capabilities")):
            rejected = run(*arguments)
            assert rejected.returncode != 0, arguments
            assert rejected.stderr, arguments
            assert not rejected.stdout, (arguments, rejected.stdout)

        settings = root / "settings.ini"
        settings.write_text("width=not-a-number\n", encoding="utf-8")
        for backend in backends:
            rejected = run("--settings", str(settings), "--renderer", backend)
            assert rejected.returncode != 0
            assert rejected.stderr
            assert settings.read_text(encoding="utf-8") == "width=not-a-number\n"
        # Both CLI overrides preserve all persisted preferences, including Vk-only floats.
        saved = ("renderer=vulkan\nray_tracing=false\nhdr_tonemapping=true\n"
                 "exposure_ev=1.23456788\nvirtual_light_enabled=false\n"
                 "mouse_sensitivity=-37.123455\n")
        settings.write_text(saved, encoding="utf-8")
        for backend in backends:
            rejected = run("--settings", str(settings), "--renderer", backend,
                           "--data", str(root / "absent-original-data"))
            assert rejected.returncode != 0 and rejected.stderr
            assert settings.read_text(encoding="utf-8") == saved
        missing = root / "missing-settings.ini"
        for backend in backends:
            rejected = run("--settings", str(missing), "--renderer", backend,
                           "--data", str(root / "absent-original-data"))
            assert rejected.returncode != 0 and rejected.stderr
            assert not missing.exists()

        result = run("--capabilities")
        assert result.returncode == 0, result.stderr
        capabilities = json.loads(result.stdout)
        renderers = capabilities["renderers"]
        assert {renderer["name"] for renderer in renderers} == set(backends)
        # Neither real backend can initialize with the deliberately unavailable SDL driver.
        features = {"hardware_ray_tracing", "msaa", "anisotropy", "bloom", "gamma", "vsync",
                    "ray_shadows", "ray_reflections", "hdr_tonemapping"}
        for renderer in renderers:
            assert renderer["available"] is False
            assert isinstance(renderer["error"], str) and renderer["error"]
            assert all(type(renderer["features"][name]) is bool and not renderer["features"][name]
                       for name in features)
            hardware = renderer["hardware"]
            assert type(hardware["max_msaa"]) is int and hardware["max_msaa"] >= 0
            assert type(hardware["max_anisotropy"]) is int and hardware["max_anisotropy"] >= 1
            assert all(isinstance(hardware[name], str) for name in ("vendor", "renderer", "version"))
            assert hardware["msaa_values"] == []
            assert hardware["anisotropy_values"] == []
            assert hardware["present_modes"] == []


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: checks/game_cli.py /path/to/pusu-game")
    check(Path(sys.argv[1]).resolve(strict=True))
    print("Native CLI consumer and unavailable-display boundaries passed")
