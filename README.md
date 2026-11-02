# Pusu: Uyanış — native Linux

A native Linux engine, installer and settings launcher for the Turkish game **Pusu: Uyanış**, which runs your own original game media without Wine.

## What it is

This is a reverse-engineered reimplementation of the game engine. It imports the data from your original disc image and runs it natively; the Windows executable is never run. It includes:

- **`pusu-installer`**: a GTK installer that imports your own CUE, BIN or ISO disc image and creates application-menu and optional desktop shortcuts.
- **`pusu-launcher`**: a settings launcher for graphics and audio that starts the game.
- **`pusu-game`**: the engine, with an OpenGL renderer and a Vulkan renderer with optional hardware ray tracing.

**Status:** in development. Native checks and selected OpenGL, Vulkan and installation tests have passed, but full campaign compatibility is not established. A user-reported flicker/noise problem is still unexplained and unfixed. Shutting the game down can take a while.

## You need the original game

No game data is included: no disc images, Windows executables, the unofficial Windows patch or imported game files. You need your own original Pusu: Uyanış media. The game and its media belong to their respective owners. This project is not affiliated with or endorsed by them.

## Download

Get the AppImage from [GitHub Releases](https://github.com/sergen213/pusu-uyanis-linux/releases). Each release has:

- `Pusu-Uyanis-Linux-x86_64.AppImage` — the installer, launcher and game.
- `Pusu-sources-001.tar`, `Pusu-sources-002.tar`, … — the corresponding sources, notices and rebuild/relink materials of the bundled components. Keep all of them with the AppImage if you pass it on.
- `public-provenance.json` and `SHA256SUMS`.

```sh
sha256sum --check --ignore-missing SHA256SUMS
chmod +x Pusu-Uyanis-Linux-x86_64.AppImage
./Pusu-Uyanis-Linux-x86_64.AppImage
```

The AppImage opens the installer. Select your own CUE, BIN or ISO image and an installation folder. To keep an existing installation untouched, install a new version into a fresh folder.

## Requirements

- x86-64 Linux with an **x86-64-v3** CPU and **glibc 2.44 or newer** for the released AppImage. Older distributions need a source build against their own libraries.
- A graphical desktop session.
- **OpenGL:** a driver with OpenGL 3.3 core.
- **Vulkan:** a Vulkan 1.2 device with descriptor indexing and buffer device addresses.
- **Vulkan ray tracing:** additionally acceleration-structure, ray-query and deferred-host-operation support. On a GPU without it, turn ray tracing off in the launcher (`ray_tracing=0`) before playing; the game stops with an error instead of silently falling back.

HDR tonemapping produces SDR output. No minimum CPU speed is established.

## Usage

Start the game from the shortcut the installer created; it opens the launcher. You can also run the programs directly against an imported data folder (paths below are from a source build; installed copies take the same options):

```sh
build/bin/pusu-launcher --data "$ASSET_ROOT"
build/bin/pusu-game --data "$ASSET_ROOT" --renderer opengl
build/bin/pusu-game --data "$ASSET_ROOT" --renderer vulkan

# What this machine supports
build/bin/pusu-game --list-renderers
build/bin/pusu-game --capabilities
```

`--settings FILE` selects a settings file. Default data is `$XDG_DATA_HOME/pusu/data` (`~/.local/share/pusu/data`); default settings are `$XDG_CONFIG_HOME/pusu/settings.ini` (`~/.config/pusu/settings.ini`). XDG directory values must be absolute.

## Build from source

Install the dependencies listed in [Development](docs/DEVELOPMENT.md#build-requirements) (CMake 3.20+, Ninja, a C++20 compiler, GTK 3, SDL2, FFmpeg, GLEW, Vulkan, libarchive, json-c, libsquish, libmsi and libgcab, among others). Then:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPUSU_MSI_ROOT=/usr -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
build/bin/pusu-installer
```

Set `PUSU_MSI_ROOT` to the prefix of your libmsi/libgcab development files. The programs are built in `build/bin/`. Details, extraction-only mode and AppImage packaging: [Development](docs/DEVELOPMENT.md).

## Documentation

- [Development](docs/DEVELOPMENT.md) — build requirements and options, tests, extraction-only import, v0.1.1 rendering fixes, AppImage packaging and source companions.
- [Artwork notice](packaging/art/NOTICE.txt) and [artwork provenance](packaging/art/provenance.json) — origin of the installer illustration and logo.

## Contributing

Changes are accepted only as pull requests to this repository, reviewed and merged by the maintainer. See [CONTRIBUTING.md](CONTRIBUTING.md).

## License and original material

The newly authored native implementation and project-authored build, check and packaging code are © sergen213, all rights reserved, and source-available under [LICENSE](LICENSE); contributions via pull request only ([CONTRIBUTING.md](CONTRIBUTING.md)). Source and AppImages published before this change remain under **GNU GPL version 3 only** (`GPL-3.0-only`). This license does not cover original game artwork, recovered retail test fixtures/interoperability data, or upstream third-party material. Those materials retain their existing rights and applicable terms.

The installer illustration and logo are derived from original game artwork and are retained to preserve the intended UI. Their origin and transformations are recorded in [packaging/art/provenance.json](packaging/art/provenance.json), with the separate [artwork notice](packaging/art/NOTICE.txt). That notice records project-user approval, not a rights-holder public sublicensing grant. The project license does not establish permission to redistribute those images or the rest of the original game.

Original media must be obtained separately and used under its applicable terms. Linking or distributing third-party libraries introduces their own license obligations; including this source license does not satisfy dependency source, notices or relinking requirements for a binary release.
