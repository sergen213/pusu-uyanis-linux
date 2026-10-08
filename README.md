# Pusu: Uyanış — native Linux

A reverse-engineered native Linux implementation of Pusu: Uyanış, with an original-media importer, GTK installer and settings launcher, OpenGL rendering, and Vulkan rendering with optional hardware ray queries. It runs the native engine, not the Windows executable; Wine is not required.

You need your own original game media to play. Disc images, Windows executables, the unofficial Windows patch, imported game data, private analysis captures and built binaries are not included in this repository.

## Status and release limits

This is an in-development engine, not a claim of complete campaign compatibility. Native checks and selected OpenGL, Vulkan and installation probes have passed in the development environment; a user-reported flicker/noise issue is still unidentified and unresolved. Full campaign completion, cross-distribution builds and older-CPU compatibility are not established.

**There is no public-release-cleared AppImage yet.** The existing local AppImage is classified for private local use. Dependency corresponding-source/build mapping, runtime static-dependency/relinking evidence and original-artwork distribution rights remain unresolved. Source publication and the new project license do not retroactively clear that binary.

## Build requirements

Install development headers/libraries and tools matching `CMakeLists.txt`. Distribution package names vary; the identifiers below are authoritative, not a tested cross-distro package-install recipe.

- CMake **3.20+**, **Ninja**, a **C++20** compiler, `pkg-config`, and a working Linux development toolchain.
- CMake packages: `OpenSSL` (component `Crypto`), `ZLIB`, `PNG`, `Threads`, `OpenGL`, `GLEW`, `Vulkan`.
- pkg-config modules: `libarchive`, `gio-2.0`, `gtk+-3.0`, `json-c`, `sdl2`, `SDL2_image`, `SDL2_mixer`, `libavformat`, `libavcodec`, `libavutil`, `libswresample`, `libswscale`.
- `glslangValidator` or `glslang` for build-time Vulkan shader compilation.
- libsquish: `squish.h` and the `squish` library.
- Native libmsi: `libmsi.h` and `msi-1.0`; native libgcab: `libgcab.h` and `gcab-1.0`.
- Python 3 interpreter when `BUILD_TESTING=ON` (the default).

Both renderer stacks and the GTK/importer dependencies are required by the current build, even if you intend to use only OpenGL. Compiling does not require the original game asset library; the two retained installer/launcher PNGs are build inputs.

### Configure, build and check

The CMake default for `PUSU_MSI_ROOT` points into the unpublished `.work/disc/tools/usr` tree. **Override it with your actual libmsi/libgcab development prefix.** For dependencies installed under `/usr`:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPUSU_MSI_ROOT=/usr -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

If the headers/libraries use a different layout, pass all four explicit CMake cache definitions instead: `PUSU_MSI_INCLUDE` (directory containing `libmsi.h`), `PUSU_MSI_LIBRARY` (actual library file), `PUSU_GCAB_INCLUDE` (directory containing `libgcab.h`) and `PUSU_GCAB_LIBRARY` (actual library file).

Use Ninja for checkout paths containing `:`: CMake's Unix Makefiles generator emits invalid Make dependencies for those paths.

The optional prefix-copy block currently names `libmsi-1.0.so.0.0.0` and `libgcab-1.0.so.0.3.0` under the prefix's `lib/`; other layouts/versioned filenames are not validated here. The commands above describe the source configuration, not a guarantee of a clean-clone build on every distribution.

Executables are produced in `build/bin/`. CTest registers nine checks. `build/bin/check_interface` is also built, but is not registered with CTest and requires an original asset root:

```sh
build/bin/check_interface "$ASSET_ROOT"
```

An optional native install is:

```sh
cmake --install build --prefix "$HOME/.local/opt/pusu-native"
```

This installs executables and the two UI images; it does **not** import media or create a game shortcut. The native media installer performs those steps.

## Import and play

A working graphical session is required for the installer, launcher and game. Start the installer GUI, select your own original CUE/BIN/ISO media and choose an installation destination:

```sh
build/bin/pusu-installer
```

The installer imports game assets without executing Windows programs and creates native launcher shortcuts. Direct MSI extraction is also supported. CUE support is limited to one sibling quoted `BINARY` file with `TRACK 01 MODE1/2352` and `INDEX 01 00:00:00`; it is not a general multi-track disc reader. Extraction requires an absent destination.

For extraction only, without a runtime installation or shortcuts:

```sh
build/bin/pusu-installer --extract-only "$OWN_CUE" --destination "$ASSET_ROOT"
```

Set `OWN_CUE` to your media path and `ASSET_ROOT` to the new extraction directory. Launch the built native binaries against that imported asset root:

```sh
build/bin/pusu-launcher --data "$ASSET_ROOT"
build/bin/pusu-game --data "$ASSET_ROOT" --renderer opengl
# Alternative backend:
build/bin/pusu-game --data "$ASSET_ROOT" --renderer vulkan
```

Use `--settings "$SETTINGS_FILE"` on the launcher or game for an explicit settings file. Default data is `$XDG_DATA_HOME/pusu/data` (or `$HOME/.local/share/pusu/data`); default settings are `$XDG_CONFIG_HOME/pusu/settings.ini` (or `$HOME/.config/pusu/settings.ini`). XDG directory values must be absolute. The launcher exposes graphics/audio settings and starts the native game.

### Graphics requirements

- **OpenGL:** a driver providing **OpenGL 3.3 core**.
- **Vulkan:** a presentable **Vulkan 1.2** device with descriptor indexing, buffer-device-address support, and the feature/descriptor limits checked by the renderer.
- **Vulkan ray queries:** additionally require acceleration-structure, ray-query and deferred-host-operation extensions. On a non-RT Vulkan device, disable ray tracing in the launcher/settings (`ray_tracing=0`) before playing; unsupported saved quality requests fail rather than silently becoming supported.

Inspect the current machine with:

```sh
build/bin/pusu-game --list-renderers
build/bin/pusu-game --capabilities
```

HDR tonemapping is SDR output, not an HDR10 display-output claim. No minimum CPU generation or performance guarantee has been established for a public binary.

## Packaging source

`packaging/build_appimage.py`, `license_bundle.py`, `check_packaging.py` and `pusu.desktop` are retained as authored packaging sources. The AppImage recipe consumes the directory containing the three executables (`build/bin`, not `build`) and accepts an explicit provenance manifest. It also requires separately supplied runtime, dependency and license/source evidence.

Machine-specific `packaging/provenance.json`, bulk `packaging/licenses/` and `packaging/sources/` are intentionally excluded from Git, as are generated packages. The checkout is therefore **not** the complete evidence bundle for the existing private AppImage and does not establish reproducible or redistributable AppImage production. Do not bypass evidence checks or treat `--redistributable` as a license grant.

By default, validated source archives, recipes and build/relink materials remain embedded in the AppImage. Adding `--source-prefix /path/to/release/Pusu-sources` to the existing build command instead creates `Pusu-sources-001.tar`, `Pusu-sources-002.tar`, etc., beside `--output` (the prefix and binary must share a directory). These uncompressed, independently extractable companions preserve every delivered material path under `usr/share/licenses/<package>/source/`; identical bytes use earlier same-part hardlinks, never links into another part. Each complete tar is strictly below 1,900,000,000 bytes including headers/padding; an oversized material/alias group fails rather than dropping bytes. Notices and source-availability directions remain inside the full-feature AppImage, and its `usr/share/pusu/manifest.json` records companion filenames, SHA256 checksums, byte sizes and each material's archive/member location.

**Publish every numbered companion beside the matching binary**, with equivalent download access and no additional charge, not merely source URLs. Recipients must verify the recorded checksums/sizes and run `tar -xf <companion-file>` for every part into the same empty directory; package `NOTICE.txt` files identify the extracted source/recipe/build/relink paths and subsequent unpacking. The builder stages all assets privately and publishes them without replacing existing paths only after AppImage assembly succeeds. This delivery option supports the recorded GNU/MPL source-access obligations; it does not establish redistribution rights, clear unresolved provenance/artwork issues, or declare a public release ready.

The strict license bundler checks conditional delivery, not legal clearance: genuine notices suffice for supported notice-only grants, while optional source URLs/pinning and unverified optional correspondence remain audit diagnostics. GNU delivery requires hash-verified local source archives, original-ELF mapping and build materials, not necessarily a remote immutable URL. MPL-1.1/2.0 require mapped covered-source archives plus a hash-bound `source.availability_notice` identifying the MPL source and the delivered archive filenames (or the package `NOTICE.txt` archive list); MPL-1.1 additionally needs its covered build/install scripts in `source.recipe` or `source.covered_build_scripts`. MPL-2.0 does not impose a GNU whole-work recipe. Evidence-backed `licenses` overrides must resolve elected/component scope; no permissive OR branch or WITH source waiver is inferred. False `copyright_complete` audit flags are preserved, explicit unresolved duties still block strict delivery, and invalid claimed evidence remains fatal in either mode.

Nonstandard component grants can be recorded explicitly as `license_scopes: [{license, scope, delivery, terms: {path, sha256}}]`: `license` matches the exact declared component ID, `scope` identifies the included code and applicable grant, and `delivery` is `notice-only` or `source-required`. Hash-bound genuine grant/waiver terms are copied as notices; these reviewed scope attestations never silently relabel custom grants as MIT or waive GNU/MPL source duties. `source-required` retains mapped local archives and build recipes. Genuine software public-domain dedication/release notices do not require an invented copyright author.

Explicit custom optional-attribution grants retain their original author credits and hash-bound permission text. The automatic copyright-year check is skipped only when every declared component grant is genuinely public-domain or explicitly optional-attribution; a mixed MIT/BSD/GNU/MPL component does not inherit that exemption.

An explicitly evidence-backed Artistic-1.0 section 4(a) Standard Version election can use `delivery: "standard-version"` with actual source archives and hash-bound `source.availability_notice` recipient acquisition directions. This does not assert downstream corresponding-source identity or impose GNU whole-work build requirements; absent directions/source and invalid claimed evidence still fail.

## License and original material

The newly authored native implementation and project-authored build, check and packaging code are licensed under **GNU GPL version 3 only** (`GPL-3.0-only`); see [LICENSE](LICENSE) for the full text. This grant does not relicense original game artwork, recovered retail test fixtures/interoperability data, or upstream third-party material. Those materials retain their existing rights and applicable terms.

The installer illustration and logo are derived from original game artwork and are retained to preserve the intended UI. Their origin and transformations are recorded in [packaging/art/provenance.json](packaging/art/provenance.json), with the separate [artwork notice](packaging/art/NOTICE.txt). That notice records project-user approval, not a rights-holder public sublicensing grant. The project GPL does not establish permission to redistribute those images or the rest of the original game.

Original media must be obtained separately and used under its applicable terms. Linking or distributing third-party libraries introduces their own license obligations; including this source license does not satisfy dependency source, notices or relinking requirements for a binary release.
