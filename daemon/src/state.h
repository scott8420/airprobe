// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Scott Combs
#pragma once

// State -- the snapshot airprobed publishes, and the only type the D-Bus
// layer and the pump both know about.
//
// WHY A SNAPSHOT TYPE AT ALL, rather than handing DbusService a Session&.
// Three reasons, in order of how much they matter:
//
//   1. The pump and the bus must be separable. DbusService has no business
//      knowing what an L2CAP socket is, and Daemon has no business knowing
//      what a GVariant is. A plain struct between them is the smallest thing
//      that keeps both halves testable and readable.
//
//   2. optional<uint8_t> does not survive a wire format by accident. The
//      library is careful that "absent from the packet", "present but
//      unreadable" and "0%" are three different answers (see aap_battery.h,
//      THE FIELD THAT LIES). Flattening that on the way to D-Bus is exactly
//      the bug that care was protecting against, so the flattening gets to
//      happen once, here, where it is visible -- not incidentally inside a
//      property getter.
//
//   3. Change detection needs two of these to compare. PropertiesChanged is
//      supposed to fire when something changed, and "changed" is a statement
//      about two snapshots.
//
// NOT IN HERE: battery age. It is derived from the wall clock rather than
// from anything that happens, so it is different every time you look and
// including it would make every comparison unequal and every tick a
// PropertiesChanged. It is fetched live in the property getter instead, and
// the interface annotates it EmitsChangedSignal=false. See dbus_service.cpp.

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace airprobe::daemon {

// One battery component as it will appear on the bus.
struct ComponentView {
    std::string status;                  // "charging" / "discharging" /
                                         // "disconnected" / "unknown"

    // EMPTY means the pods gave us no usable reading -- status disconnected,
    // or a raw byte outside 0-100. A consumer must render that as "no
    // reading", never as 0%. The D-Bus dict simply omits the Level key in
    // that case, which is the closest thing the wire format has to nullopt.
    std::optional<uint8_t> level;

    // Always the byte that actually arrived, readable or not. Kept on the bus
    // so a misbehaving pod can be diagnosed without re-running the CLI.
    uint8_t raw_level = 0;

    // The library saw one of the two supposedly-fixed 0x01 framing bytes come
    // through as something else. Not fatal, but a structural surprise, and a
    // consumer showing a number derived from that packet should know.
    bool framing_anomaly = false;

    bool operator==(const ComponentView&) const = default;
};

struct State {
    // The classic paired MAC this daemon is bound to. Fixed for the process
    // lifetime at M1; when the daemon learns devices from BlueZ it will still
    // serve exactly one.
    std::string address;

    // Session::phase() by name: closed / wait-handshake-ack /
    // wait-features-ack / listening. A consumer deciding whether to show a
    // spinner or a number needs "not ready yet" and "ready but silent" to be
    // distinguishable, and they are not the same as have_battery.
    std::string phase = "closed";

    // Is the AAP channel up right now. False with have_battery true is the
    // ordinary case after the pods go back to the phone: the reading we have
    // is real, it is simply no longer being refreshed.
    bool channel_open = false;

    // Last thing the channel did, in words, for a consumer that wants to say
    // WHY there is nothing rather than just showing nothing. Carries the
    // connect diagnosis on failure -- which stage, which errno, what it
    // usually means -- because "it didn't connect" is not actionable.
    std::string status = "starting";

    bool have_battery = false;

    // Count of opcode-0x0004 packets whose shape the parser refused. Nonzero
    // means something structural changed and the cached reading should be
    // distrusted rather than quietly served. Published because the consumer
    // is the one deciding whether to show a number.
    int parse_refusals = 0;

    // Keyed "left" / "right" / "case". A missing key means that component was
    // not in the packet at all, which is a different statement from present
    // and unreadable.
    std::map<std::string, ComponentView> components;

    bool operator==(const State&) const = default;
};

} // namespace airprobe::daemon
