#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Scott Combs
# Airprobe build. Out-of-tree, Debug by default.
#
# Layout as of s4:
#   lib/     libairprobe.a  -- the pods; no terminal, no argv
#   cli/     airprobe       -- the probe CLI, first consumer of the library
#   daemon/  airprobed      -- session-bus daemon, second consumer
#
# Not built here: shell-extension/ is GJS and the shell loads it from source.
# It has its own script, deliberately -- this one compiles into build/ and
# touches nothing else, and installing into a live GNOME session is a
# different verb:
#
#   ./install.sh install | uninstall | status
#
# Fedora 44 dependencies:
#   sudo dnf install gcc-c++ cmake bluez-libs-devel spdlog-devel fmt-devel \
#                    glib2-devel
#
# glib2-devel is new at s4 and is airprobed's only extra dependency (gio-2.0
# and gio-unix-2.0). The CLI does not link it.
#
# If cmake reports BlueZ or spdlog missing, that dnf line is the fix.
#
# Permissions -- the L2CAP socket may be refused without one of these:
#   sudo usermod -aG bluetooth "$USER"        # then log out and back in
# or, on the built binary:
#   sudo setcap cap_net_raw,cap_net_admin+eip build/cli/airprobe

set -euo pipefail
cd "$(dirname "$0")"

BUILD_TYPE="${1:-Debug}"

cmake -B build -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build build -j"$(nproc)"

echo
echo "built: build/cli/airprobe      (library: build/lib/libairprobe.a)"
echo "       build/daemon/airprobed"
echo
echo "probe:  ./build/cli/airprobe <MAC> --verbose"
echo "daemon: ./build/daemon/airprobed <MAC> --verbose"
echo
echo "inspect the daemon while it runs:"
echo "  gdbus introspect --session -d io.github.scott8420.Airprobe \\"
echo "                             -o /io/github/scott8420/Airprobe"
echo
echo "no hardware handy? ./build/daemon/airprobed --self-test"
echo "find MAC with: bluetoothctl devices"

echo
echo "Built into build/.  Installing is separate:"
echo "  ./install.sh install      (or: status, uninstall)"
