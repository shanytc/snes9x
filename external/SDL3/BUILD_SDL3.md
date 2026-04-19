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
build_sdl3.bat                # builds x64 + x86, Debug + Release
build_sdl3.bat x64            # x64 only
build_sdl3.bat x86            # x86 only
build_sdl3.bat clean          # wipe CMake build trees first, then build
build_sdl3.bat syms           # embed /Z7 debug info in the Debug lib for
                              # stepping into SDL source from VS - produces
                              # a fat SDL3-staticd.lib. DO NOT commit it.
                              # Rerun without "syms" when done to restore
                              # the lean committed artifact.
```

Flags combine: `build_sdl3.bat x64 clean syms` etc.

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

## Reverting to vanilla upstream SDL

Two reversible paths, in order of invasiveness. Use Level 1 first unless you
specifically need to prove a bug isn't in our patch.

### Level 1 — Runtime toggle (no rebuild)

The async worker is gated by the hint `SDL_JOYSTICK_DIRECTINPUT_ASYNC`
(default on). Set it to `"0"` before `SDL_Init(...)` in snes9x to skip
starting the worker and fall through to the original synchronous
`IDirectInput8::EnumDevices` path. The patched `.lib` stays linked but
behaves like upstream SDL.

In `win32/SDLInput.cpp` (near the other `SDL_SetHint` calls in
`SDLInput_Init`):

```c
SDL_SetHint("SDL_JOYSTICK_DIRECTINPUT_ASYNC", "0");
```

Rebuild snes9x. Remove the line to re-enable the worker — no SDL rebuild
needed either way.

### Level 2 — Replace the `.lib` artifacts with a vanilla upstream build

Use this when you want the committed binaries themselves to come from
unmodified `libsdl-org/SDL`, e.g. for a release where you want zero
downstream patches in the shipping artifacts.

Point the submodule at upstream and pin the same release tag:

```
# Edit .gitmodules: set url back to libsdl-org, drop the `branch` line
cd external/SDL3-src
git remote set-url origin https://github.com/libsdl-org/SDL.git
git fetch origin --tags
git checkout release-3.2.14
cd ../..
git submodule sync external/SDL3-src
```

Rebuild the libs against the clean upstream source:

```
cd external/SDL3
build_sdl3.bat clean
cd ../..
```

Commit both the submodule wiring and the rebuilt artifacts together so the
repo stays internally consistent:

```
git add .gitmodules external/SDL3-src
git add -f external/SDL3/lib/x64/SDL3-static.lib  external/SDL3/lib/x64/SDL3-staticd.lib
git add -f external/SDL3/lib/x86/SDL3-static.lib  external/SDL3/lib/x86/SDL3-staticd.lib
git commit -m "sdl3: revert to vanilla libsdl-org/SDL"
```

Rebuild snes9x. The async worker is gone — DirectInput enumeration is
back on the event-pump thread (the original stall reappears on the HID
drivers that triggered it).

To re-apply the patch later: undo `.gitmodules` to point back at
`shanytc/SDL` branch `snes9x-3.2.14`, `git submodule sync`, rebuild the
libs, commit.
