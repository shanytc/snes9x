# Building SDL3 from source

snes9x statically links `SDL3-static.lib` (Release) and `SDL3-staticd.lib`
(Debug) from `external/SDL3/lib/{x64,x86}/`. Those `.lib` files are kept in
the repo so a fresh clone can build snes9x without a CMake step. Only rebuild
them when:

1. Bumping the pinned SDL3 version (`external/SDL3-src` submodule), or
2. Modifying files inside `external/SDL3-src/`.

## Prerequisites

- Visual Studio 2019 or 2022 with the **Desktop development with C++**
  workload, including the C++ CMake Tools component.
- `cmake.exe` on `PATH`, or the copy shipped with VS
  (`Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` — the
  script finds it automatically if not on `PATH`).
- The `external/SDL3-src` submodule checked out:

  ```
  git submodule update --init --recursive external/SDL3-src
  ```

## Bump the pinned version

```
cd external/SDL3-src
git fetch --tags
git checkout release-X.Y.Z       # e.g. release-3.2.14
cd ../..
git add external/SDL3-src
```

Then rebuild the libs (next section) and commit both the submodule pointer
bump *and* the new `.lib` files together.

## Rebuild the static libs

From `external/SDL3/`:

```
build_sdl3.bat            # builds x64 + x86, Debug + Release
build_sdl3.bat x64        # x64 only
build_sdl3.bat x86        # x86 only
build_sdl3.bat clean      # wipe CMake build trees first, then build
```

The script:

1. Locates Visual Studio via `vswhere`.
2. Runs CMake with
   `-DSDL_STATIC=ON -DSDL_SHARED=OFF -DCMAKE_DEBUG_POSTFIX=d` for each arch
   (output tree: `external/SDL3-src/build/{x64,x86}/`).
3. Builds the `SDL3-static` target in both `Debug` and `Release`.
4. Copies the resulting `SDL3-static.lib` / `SDL3-staticd.lib` into
   `external/SDL3/lib/{x64,x86}/`.

snes9xw then picks them up on its next rebuild — no project file changes
needed.

## Local SDL patches

The submodule points at **`shanytc/SDL`** (a fork of `libsdl-org/SDL`),
branch **`snes9x-3.2.14`**. That branch is `release-3.2.14` upstream plus
one local commit that adds an async DirectInput enumeration worker
(`src/joystick/windows/SDL_dinputjoystick.c`) to prevent USB hotplug from
stalling the SDL event pump.

`git submodule update --init --recursive` on a fresh snes9x clone pulls
the patched SDL automatically — nothing extra to do.

### Updating the patch

```
cd external/SDL3-src
# ... edit SDL source ...
git add -u
git commit -m "your message"
git push origin snes9x-3.2.14
cd ../..
git add external/SDL3-src           # bumps the pinned SHA
```

### Bumping to a new upstream SDL release

```
cd external/SDL3-src
git fetch upstream --tags
git rebase release-X.Y.Z             # e.g. release-3.2.15
# resolve any conflicts in SDL_dinputjoystick.c
git push --force-with-lease origin snes9x-3.2.14
cd ../..
# Rename the branch in .gitmodules if you want the name to match the tag:
# branch = snes9x-3.2.15
git add .gitmodules external/SDL3-src
```

The async worker is gated by `SDL_JOYSTICK_DIRECTINPUT_ASYNC` (default on).
Set it to `"0"` to fall back to the original synchronous enumeration at
runtime:

```c
SDL_SetHint("SDL_JOYSTICK_DIRECTINPUT_ASYNC", "0");
```
