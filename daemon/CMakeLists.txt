# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Scott Combs
# airprobed -- libairprobe's second consumer, and the reason s3 split the
# library out in the first place.
#
# Note what is NOT here: no find_package for BlueZ, no find_package for
# spdlog. Both arrive as PUBLIC usage requirements of airprobe::lib, exactly
# as they do for cli/. That is the split doing its job -- the failure it
# prevents is two consumers with two slightly drifted dependency blocks,
# presenting as "the daemon can't find BlueZ but the CLI can".
#
# GIO is the one dependency airprobed has that the CLI does not, so it is
# found HERE rather than at the root. The root finds what the LIBRARY needs;
# a dependency of exactly one consumer belongs to that consumer. Putting it at
# the root would make the CLI fail to configure on a machine without GLib
# development files, for a package it never links.

find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0)

add_executable(airprobed
  src/main.cpp
  src/pump.cpp
  src/bluez_watch.cpp
  src/dbus_service.cpp
  src/battery_provider.cpp
)

target_link_libraries(airprobed PRIVATE airprobe::lib PkgConfig::GIO)
