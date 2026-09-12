# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Scott Combs
cmake_minimum_required(VERSION 3.20)
project(airprobe LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------------------
# Dependencies are discovered ONCE, here, not per target.
#
# s3 split libairprobe out from the CLI, and s4 adds a second consumer
# (airprobed). Two consumers looking for the same two packages is two places
# for the search to drift apart -- and a drift between them would present as
# "the daemon can't find BlueZ but the CLI can", which is a confusing bug for
# a trivial cause. The library carries both as PUBLIC usage requirements, so
# anything that links airprobe::lib inherits them and no consumer repeats
# this block.
# ---------------------------------------------------------------------------

# --- BlueZ. Fedora: bluez-libs-devel | Debian/Ubuntu: libbluetooth-dev -----
#
# Prefer pkg-config (bluez-libs-devel ships bluez.pc); fall back to a plain
# library search for distros that don't. If BOTH miss, the package simply
# isn't installed -- say so, with the command, rather than emitting a bare
# find_library error.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(BLUEZ QUIET IMPORTED_TARGET bluez)
endif()

if(TARGET PkgConfig::BLUEZ)
  set(AIRPROBE_BLUEZ PkgConfig::BLUEZ)
  message(STATUS "BlueZ: found via pkg-config (${BLUEZ_VERSION})")
else()
  find_library(BLUETOOTH_LIB NAMES bluetooth)
  find_path(BLUETOOTH_INCLUDE_DIR NAMES bluetooth/bluetooth.h)

  if(NOT BLUETOOTH_LIB OR NOT BLUETOOTH_INCLUDE_DIR)
    message(FATAL_ERROR
      "\n"
      "BlueZ development files not found.\n"
      "\n"
      "  Fedora:        sudo dnf install bluez-libs-devel\n"
      "  Debian/Ubuntu: sudo apt install libbluetooth-dev\n"
      "  Arch:          sudo pacman -S bluez-libs\n"
      "\n"
      "Airprobe needs bluetooth/l2cap.h and libbluetooth to open the AAP\n"
      "channel. See build.sh for the full dependency list.\n")
  endif()

  add_library(airprobe_bluez INTERFACE)
  target_link_libraries(airprobe_bluez INTERFACE ${BLUETOOTH_LIB})
  target_include_directories(airprobe_bluez INTERFACE ${BLUETOOTH_INCLUDE_DIR})
  set(AIRPROBE_BLUEZ airprobe_bluez)
  message(STATUS "BlueZ: no bluez.pc, using ${BLUETOOTH_LIB} directly")
endif()

# --- spdlog. Fedora: spdlog-devel fmt-devel | Debian/Ubuntu: libspdlog-dev -
find_package(spdlog QUIET)
if(NOT spdlog_FOUND)
  message(FATAL_ERROR
    "\n"
    "spdlog not found.\n"
    "\n"
    "  Fedora:        sudo dnf install spdlog-devel fmt-devel\n"
    "  Debian/Ubuntu: sudo apt install libspdlog-dev\n"
    "  Arch:          sudo pacman -S spdlog\n")
endif()

# GIO is deliberately NOT found here. It is a dependency of airprobed alone,
# so daemon/CMakeLists.txt finds it -- a machine that wants only the CLI
# should not need GLib development files to configure. The root finds what the
# LIBRARY needs, and both consumers inherit those.
add_subdirectory(lib)
add_subdirectory(cli)
add_subdirectory(daemon)
