// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Scott Combs
#include "airprobe/aap_battery.h"
#include "airprobe/aap_protocol.h"

#include <cstdio>

namespace airprobe {

namespace {

// Layout of one component block, offsets relative to the block start.
// 04 00 04 00 04 00 [count] then blocks of:
//   +0 component  +1 spacer(0x01)  +2 level  +3 status  +4 end(0x01)
constexpr size_t kBlockSize   = 5;
constexpr size_t kCountOffset = 6;
constexpr size_t kFirstBlock  = 7;

constexpr size_t kOffComponent = 0;
constexpr size_t kOffSpacer    = 1;
constexpr size_t kOffLevel     = 2;
constexpr size_t kOffStatus    = 3;
constexpr size_t kOffEnd       = 4;

bool known_component(uint8_t b)
{
    return b == static_cast<uint8_t>(Component::Case)
        || b == static_cast<uint8_t>(Component::Left)
        || b == static_cast<uint8_t>(Component::Right);
}

} // namespace

const char* component_name(Component c)
{
    switch (c) {
    case Component::Case:  return "case";
    case Component::Left:  return "left";
    case Component::Right: return "right";
    }
    return "?";
}

const char* status_name(ChargeStatus s)
{
    switch (s) {
    case ChargeStatus::Unknown:      return "unknown";
    case ChargeStatus::Charging:     return "charging";
    case ChargeStatus::Discharging:  return "discharging";
    case ChargeStatus::Disconnected: return "disconnected";
    }
    return "?";
}

std::string BatteryComponent::to_string() const
{
    char buf[96];
    if (level)
        std::snprintf(buf, sizeof(buf), "%s %u%% (%s)",
                      component_name(component), static_cast<unsigned>(*level),
                      status_name(status));
    else
        std::snprintf(buf, sizeof(buf), "%s --%% (%s, raw 0x%02X)",
                      component_name(component), status_name(status), raw_level);

    std::string out = buf;
    if (framing_anomaly) out += " [FRAMING ANOMALY]";
    return out;
}

std::optional<BatteryComponent> BatteryReport::find(Component c) const
{
    for (const auto& comp : components)
        if (comp.component == c) return comp;
    return std::nullopt;
}

std::string BatteryReport::to_string() const
{
    std::string out;
    for (size_t i = 0; i < components.size(); ++i) {
        if (i) out += "   ";
        out += components[i].to_string();
    }
    return out.empty() ? "(no components)" : out;
}

std::optional<BatteryReport> parse_battery(std::span<const uint8_t> packet)
{
    if (!proto::has_std_header(packet)) return std::nullopt;
    if (proto::opcode_of(packet) != proto::kOpBattery) return std::nullopt;
    if (packet.size() <= kCountOffset) return std::nullopt;

    const size_t count = packet[kCountOffset];

    // The count byte and the packet length are two independent claims about
    // the same structure. If they disagree, we do not get to pick a winner.
    if (packet.size() != kFirstBlock + count * kBlockSize) return std::nullopt;

    BatteryReport report;
    report.components.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        const size_t base = kFirstBlock + i * kBlockSize;

        const uint8_t comp_byte   = packet[base + kOffComponent];
        const uint8_t spacer      = packet[base + kOffSpacer];
        const uint8_t level_byte  = packet[base + kOffLevel];
        const uint8_t status_byte = packet[base + kOffStatus];
        const uint8_t end_byte    = packet[base + kOffEnd];

        // An unrecognised component id means the packet is not the shape we
        // think it is. Refuse the whole report rather than return a partial
        // one -- a caller that gets two of three components back has no way
        // to tell that from a device that only has two.
        if (!known_component(comp_byte)) return std::nullopt;

        BatteryComponent c;
        c.component = static_cast<Component>(comp_byte);
        c.status    = static_cast<ChargeStatus>(status_byte);
        c.raw_level = level_byte;
        c.framing_anomaly = (spacer != 0x01 || end_byte != 0x01);

        // The level is believable only when the component is actually
        // reporting. See the header comment -- the A3056's case sent 0x00,
        // 0xFF and 0x64 within one second while disconnected.
        const bool reporting = (c.status != ChargeStatus::Disconnected);
        if (reporting && level_byte <= 100) c.level = level_byte;

        report.components.push_back(c);
    }

    return report;
}

} // namespace airprobe
