# SuperSnes9x
*SuperSnes9x - Portable Super Nintendo Entertainment System (TM) emulator*

This is the un-official source code fork for the Snes9x project.

Please check the official [Wiki](https://github.com/snes9xgit/snes9x/wiki) for additional information.

## SuperSnes9x Features:
- Super FX 3 (FX3) support for the 2026 LRG releases, with cycle-accurate GSU timing
- LRG SNES rumble dongle support (Ultimate Doom FX3, RT.SFC)
- Supports GameBoy, GameBoy Color, Super Game Boy, Super Game Boy 2, Super Game Boy Color (experimental)
- libretro core for SNES / SGB / GB / GBC
- Voicer-kun peripheral emulation (plays its audio CDs from cue/bin images, infrared transmitter/receiver)
- [Super Famicom Box](https://github.com/shanytc/snes9x/wiki/Super-Famicom-Box) (hotel version, with live English OSD translations)
- [Nintendo SuperDisc](https://github.com/shanytc/snes9x/wiki/SuperDisc)
- [Nintendo Super System](https://github.com/shanytc/snes9x/wiki/Nintendo-Super-System)
- Event carts - PowerFest '94, Nintendo Campus Challenge '92
- Kaillera Server/Client
- RetroAchievements (without Hardcore)
- Wide-SNES - Supports the widescreen SMW ROM hack by [Vitor Vilela](https://github.com/VitorVilela7/wide-snes)
- PPU Sprite/Tile/Tiles viewers (SNES, GB, GBC, SGB)
- Audio waveform viewer (Logic-style per-channel tracks, solo/mute, level meters)
- Enhanced Cheat Search / Cheat Editor (SNES, GB — incl. GB cheats in SGB BIOS mode)
- Multi-Bind-Controller support
- Run-Ahead (Input lag reduction)
- Color Correction Support / Native CRT Colors
- SDL Support
- Multi-Language Pack
- 100% Acid Tests support for gbc/gb/sgb (Emulator [shootout](https://tomek.rekawek.eu/GBEmulatorShootout/))


## SuperSnes9x libretro core

The SuperSnes9x libretro core (`supersnes9x_libretro.so` — SNES / SFC / SGB /
GB / GBC in one core) is built from this repository. Prebuilt cores for
Windows, Linux, macOS and Android are in the [nightly builds](#nightly-builds).
To build the portable Linux core locally with Docker:

```bash
cd libretro/linux
./build-portable.sh x86_64        # output: libretro/linux/dist/x86_64/
```

Then copy `supersnes9x_libretro.so` together with
`libretro/supersnes9x_libretro.info` into your RetroArch cores directory
(e.g. `~/.config/retroarch/cores/`). For authentic Super Game Boy mode,
place the SGB BIOS ROMs (`SGB1.sfc` / `SGB2.sfc`) in RetroArch's system
directory — without them, GB content runs on the built-in BIOS-less core.

For the original Game Boy / Game Boy Color power-on animation (the Nintendo
logo scrolling down, then the boot chime), drop a boot ROM in the same place:
`dmg_boot.bin` (256 bytes) for Game Boy and `cgb_boot.bin` (2304 bytes) for
Game Boy Color. The desktop builds also look next to the ROM and in the
configured BIOS directory — note that a *relative* BIOS directory in the config
resolves against the working directory the emulator was launched from, so an
absolute path is safer. None is bundled — with no matching file present, carts
start immediately exactly as before.

Emulation → BIOS then lists it alongside the Super Game Boy entries: **No BIOS**,
**Game Boy BIOS** (or **Game Boy Color BIOS**, matching the loaded cart),
**SGB1**, **SGB2**. Entries whose BIOS file is missing are greyed out, and the
checked entry is whatever is actually running. With both an SGB BIOS and a GB
boot ROM installed, SGB mode stays the default; picking the Game Boy entry runs
the cart on its own console instead, and picking an SGB entry hands it back.

Or you can build all versions: appimage, linux, android (requires docker).

```bash
cd libretro
./build-all.sh
```

## macOS

The Qt GUI and the libretro core both build on macOS, self-signed so they run
without a paid Apple Developer account:

```bash
# Qt GUI -> qt/build-macos/super-snes9x-qt.app
cmake -S qt -B qt/build-macos -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build qt/build-macos -j"$(sysctl -n hw.ncpu)"
qt/scripts/makeapp-macos.sh

# libretro core -> libretro/macos/dist/ (universal x86_64 + arm64)
libretro/macos/build-macos.sh
```

Or build both at once with `cd libretro && ./build-all.sh --osx`. That flag
selects the macOS artifacts *instead of* the Linux and Android ones — the
Linux AppImages are native builds and the macOS ones need Xcode, so a single
host can't produce both.

The macOS GUI uses the Qt Software or OpenGL display driver; Vulkan is not
available on the platform. Because the builds are ad-hoc signed rather than
notarized, a copy that arrives over the network needs its quarantine flag
cleared: `xattr -dr com.apple.quarantine <path>`.

See [qt/docs/README-macos.md](qt/docs/README-macos.md) for details.

## Nightly builds

[![Nightly builds](https://github.com/shanytc/snes9x/actions/workflows/nightly.yml/badge.svg?branch=master)](https://github.com/shanytc/snes9x/actions/workflows/nightly.yml)

Every night that `master` has new commits, GitHub Actions builds everything
below and publishes it as the rolling
[nightly pre-release](https://github.com/shanytc/snes9x/releases/tag/nightly).
The links always point at the newest nightly. These are untested snapshots
and may be broken. For stable builds use the
[latest release](https://github.com/shanytc/snes9x/releases/latest).

| Platform       | GUI                                            | libretro core                                   |
|----------------|------------------------------------------------|-------------------------------------------------|
| Windows 64-bit | [super-snes9x (x64)][win64]                    | [core (x64)][core-win64]                        |
| Windows 32-bit | [super-snes9x (x86)][win32]                    | [core (x86)][core-win32]                        |
| Linux x86_64   | [Qt AppImage][qt-x64] · [GTK AppImage][gtk-x64] | [core (x86_64)][core-linux64]                   |
| Linux x86      | [Qt AppImage][qt-x86] · [GTK AppImage][gtk-x86] | [core (x86)][core-linux32]                      |
| macOS          | [Qt app (Apple Silicon)][mac-qt]               | [core (universal x86_64 + arm64)][core-mac]     |
| Android        | —                                              | [arm64-v8a][core-android64] · [armeabi-v7a][core-android32] |

Windows: unzip and run. Linux: `chmod +x` the AppImage. macOS: the app is
self-signed, so after copying it run
`xattr -dr com.apple.quarantine super-snes9x-qt.app`. Cores: unzip the core
and its `.info` file into RetroArch's cores directory.

[win64]: https://github.com/shanytc/snes9x/releases/download/nightly/super-snes9x-nightly-win32-x64.zip
[win32]: https://github.com/shanytc/snes9x/releases/download/nightly/super-snes9x-nightly-win32.zip
[qt-x64]: https://github.com/shanytc/snes9x/releases/download/nightly/super-snes9x-nightly-qt-x86_64.AppImage
[gtk-x64]: https://github.com/shanytc/snes9x/releases/download/nightly/super-snes9x-nightly-gtk-x86_64.AppImage
[qt-x86]: https://github.com/shanytc/snes9x/releases/download/nightly/super-snes9x-nightly-qt-x86.AppImage
[gtk-x86]: https://github.com/shanytc/snes9x/releases/download/nightly/super-snes9x-nightly-gtk-x86.AppImage
[mac-qt]: https://github.com/shanytc/snes9x/releases/download/nightly/super-snes9x-nightly-qt-macos-arm64.zip
[core-win64]: https://github.com/shanytc/snes9x/releases/download/nightly/supersnes9x_libretro-nightly-win32-x64.zip
[core-win32]: https://github.com/shanytc/snes9x/releases/download/nightly/supersnes9x_libretro-nightly-win32.zip
[core-linux64]: https://github.com/shanytc/snes9x/releases/download/nightly/supersnes9x_libretro-nightly-linux-x64.zip
[core-linux32]: https://github.com/shanytc/snes9x/releases/download/nightly/supersnes9x_libretro-nightly-linux.zip
[core-mac]: https://github.com/shanytc/snes9x/releases/download/nightly/supersnes9x_libretro-nightly-osx-universal.zip
[core-android64]: https://github.com/shanytc/snes9x/releases/download/nightly/supersnes9x_libretro-nightly-android-arm64.zip
[core-android32]: https://github.com/shanytc/snes9x/releases/download/nightly/supersnes9x_libretro-nightly-android.zip
