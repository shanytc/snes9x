#!/usr/bin/env bash

# Build a shippable Snes9x GTK AppImage against the Ubuntu 22.04 baseline
# (glibc 2.35), the same floor as the Qt AppImage. An AppImage built directly
# on a newer host would refuse to run on older distros, so the ship build
# happens inside a container. Requires Docker.
#
#   gtk/scripts/makeappimage-docker.sh
#
# Output: gtk/build-appimage/super-snes9x-gtk-x86_64.AppImage

set -e

REPO=$(cd "$(dirname "$0")/../.." && pwd)

docker run --rm \
    -v "$REPO":/snes9x \
    -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    ubuntu:22.04 bash -ec '
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
    build-essential cmake ninja-build pkg-config git ca-certificates wget file \
    gettext \
    libgtkmm-3.0-dev libsdl2-dev libpng-dev zlib1g-dev libminizip-dev \
    libcurl4-openssl-dev libxrandr-dev libx11-dev libxext-dev libxv-dev \
    libgl-dev libegl-dev libpulse-dev libasound2-dev portaudio19-dev \
    libwayland-dev wayland-protocols libxkbcommon-dev

# Always build from scratch: reproducible, and sidesteps stale-state issues.
rm -rf /snes9x/gtk/build-appimage
mkdir -p /snes9x/gtk/build-appimage
cd /snes9x/gtk/build-appimage
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja
../scripts/makeappimage.sh
chown -R "$HOST_UID:$HOST_GID" /snes9x/gtk/build-appimage
'
