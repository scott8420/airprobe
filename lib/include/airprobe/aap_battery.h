// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Scott Combs
#pragma once

// Battery report parsing (opcode 0x0004).
//
// Wire format, from librepods `AAP Definitions.md` line 51 and CONFIRMED
// byte-for-byte against the A3056 in s2:
//
//   04 00 04 00 04 00 [count] ( [component] 01 [level] [status] 01 ) x count
//
// The doc's example is AirPods Pro 2 with three components in Left, Right,
// Case order. The A3056 sent exactly that shape, six times. We still do not
// assume either the count or the ordering -- the pods tell us both, and
// trusting our own expectation over the packet is how a parser starts being
// quietly wrong on the one device that differs.
//
// THE FIELD THAT LIES. `level` is only meaningful when `status` is not
// Disconnected. In the s2 capture the case reported level 0x00, 0xFF and 0x64
// in three packets spanning one second, while its status was Disconnected in
// two of the three. Read unconditionally, that renders as a case battery that
// swung 0% -> 255% -> 100%, and the 0% reading -- being the common one --
// would sit in the UI looking perfectly reasonable indefinitely. So `level`
// is an optional here, empty unless the reading is one we have reason to
// believe. That is the whole reason this type is not three uint8_t.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace airprobe {

enum class Component : uint8_t {
    Case  = 0x08,   // AAP_Definitions.md line 51 table
    Left  = 0x04,
    Right = 0x02,
};

enum class ChargeStatus : uint8_t {
    Unknown      = 0x00,
    Charging     = 0x01,
    Discharging  = 0x02,
    Disconnected = 0x04,
};

const char* component_name(Component c);
const char* status_name(ChargeStatus s);

struct BatteryComponent {
    Component    component{};
    ChargeStatus status{};

    // Percentage, 0-100. EMPTY when the pods gave us no usable reading --
    // status Disconnected, or a raw byte outside 0-100. Callers must handle
    // the empty case as "no reading", never as zero.
    std::optional<uint8_t> level;

    // The raw byte, always, even when `level` is empty. Kept so a capture can
    // be re-analysed later: the 0xFF the case sent is evidence about the
    // protocol even though it is not a battery percentage.
    uint8_t raw_level = 0;

    // True when the two bytes the doc says are fixed at 0x01 were not. We do
    // not refuse the packet over it -- we record it, because a structural
    // surprise is a finding and silently dropping it would hide the very
    // divergence this project exists to discover.
    bool framing_anomaly = false;

    std::string to_string() const;
};

struct BatteryReport {
    std::vector<BatteryComponent> components;

    // Convenience lookups. Empty when that component was absent from the
    // packet, which is different from present-but-unreadable.
    std::optional<BatteryComponent> find(Component c) const;

    // One line, for logs and for eyeballing against the iPhone.
    std::string to_string() const;
};

// Parses a battery packet. Returns empty when `packet` is not a well-formed
// opcode-0x0004 report -- wrong header, wrong opcode, or a length that does
// not match the count byte's own claim.
//
// A length mismatch is a hard refusal rather than a best-effort parse. The
// count byte and the packet length are two independent statements about the
// same thing; when they disagree we do not know which to believe, and a
// parser that picks one is guessing.
std::optional<BatteryReport> parse_battery(std::span<const uint8_t> packet);

} // namespace airprobe
