# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Scott Combs
# airprobe -- the probe CLI. libairprobe's first consumer.
#
# This is the same program s1 and s2 ran; it moved from daemon/ to cli/
# because it never was a daemon. `airprobed` arrives at s4 as a sibling
# directory and links the same library.
#
# Everything here is presentation and experiment control: argv, the
# --caps/--notify/--sequence knobs, the .cap transcript, hexdumps, the
# end-of-run summary. None of it belongs in the library.

add_executable(airprobe
  src/main.cpp
)

# One link line, and no mention of BlueZ or spdlog -- they arrive as PUBLIC
# usage requirements of the library. That is the split working.
target_link_libraries(airprobe PRIVATE airprobe::lib)
