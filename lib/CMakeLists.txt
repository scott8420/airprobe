# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Scott Combs
# libairprobe -- everything that knows about AirPods, and nothing that knows
# about a terminal.
#
# The seam this target defines: anything above it (CLI now, airprobed at s4,
# a test harness later) talks to the pods only through these headers. Nothing
# in here writes to stdout, parses argv, or decides how to present a battery
# reading. If a change to this library requires a change to how output looks,
# the change is on the wrong side of the seam.
#
# STATIC, deliberately. Airprobe is a personal tool with two consumers in the
# same build tree; a shared object would buy versioned installs we will never
# do and cost an rpath to get wrong.

add_library(airprobe_lib STATIC
  src/aap_socket.cpp
  src/aap_protocol.cpp
  src/aap_battery.cpp
  src/session.cpp
)

# libairprobe.a, not libairprobe_lib.a. The CMake target name has the suffix
# only because `airprobe` is already the CLI executable.
set_target_properties(airprobe_lib PROPERTIES OUTPUT_NAME airprobe)

# PUBLIC: consumers include <airprobe/aap_battery.h> by the same path the
# library's own sources use, so a header that compiles here compiles there.
target_include_directories(airprobe_lib PUBLIC include)

# PUBLIC on both, and the reason matters:
#   - spdlog appears in no public header, but it IS the project's diagnostic
#     channel and every consumer configures it (see RULES, log mode). Making
#     consumers re-find it would be ceremony.
#   - BlueZ is needed at link time by anyone linking this archive, since a
#     static library carries no dependencies of its own.
target_link_libraries(airprobe_lib PUBLIC ${AIRPROBE_BLUEZ} spdlog::spdlog)

# An alias so consumers read as airprobe::lib rather than a bare global name.
add_library(airprobe::lib ALIAS airprobe_lib)
