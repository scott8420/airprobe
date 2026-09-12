// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Scott Combs
#include "airprobe/aap_protocol.h"

#include <cctype>
#include <cstdio>

namespace airprobe::proto {

namespace {

constexpr std::array<uint8_t, 4> kStdHeader = { 0x04, 0x00, 0x04, 0x00 };
constexpr std::array<uint8_t, 4> kAckHeader = { 0x01, 0x00, 0x04, 0x00 };

bool starts_with(std::span<const uint8_t> p, const std::array<uint8_t, 4>& h)
{
    if (p.size() < h.size()) return false;
    for (size_t i = 0; i < h.size(); ++i)
        if (p[i] != h[i]) return false;
    return true;
}

} // namespace

bool has_std_header(std::span<const uint8_t> p) { return starts_with(p, kStdHeader); }
bool is_handshake_ack(std::span<const uint8_t> p) { return starts_with(p, kAckHeader); }

uint16_t opcode_of(std::span<const uint8_t> p)
{
    if (!has_std_header(p) || p.size() < 6) return 0xFFFF;
    return static_cast<uint16_t>(p[4] | (p[5] << 8));
}

// Source: librepods docs/opcodes.md. "Destination" in that table is from the
// pods' point of view -- Host means the pods send it to us, Accessory means we
// send it to them. Kept in the names below because knowing which direction an
// opcode travels is half of knowing what a packet means.
const char* opcode_name(uint16_t op)
{
    switch (op) {
    case 0x0001: return "unknown-0001";
    case 0x0004: return "battery";
    case 0x0006: return "ear-detection";
    case 0x0009: return "control";
    case 0x000D: return "audio-source-req";
    case 0x000E: return "audio-source-resp";
    case 0x000F: return "notification-register";
    case 0x0010: return "smart-routing-relay";
    case 0x0011: return "smart-routing-response";
    case 0x0014: return "connected-device-mac";
    case 0x0017: return "multi-purpose-0017";
    case 0x0019: return "stem-press";
    case 0x001B: return "timestamp";
    case 0x001D: return "device-info";
    case 0x001E: return "rename-device";
    case 0x0022: return "unknown-0022";
    case 0x0029: return "host-capabilities-alt";
    case 0x002B: return "features-ack/paired-devices";
    case 0x002D: return "connected-devices-req";
    case 0x002E: return "connected-devices-list";
    case 0x0030: return "ble-keys-req";
    case 0x0031: return "ble-keys-response";
    case 0x004B: return "conversation-awareness";
    case 0x004D: return "host-capabilities";
    case 0x004F: return "information-req/res";
    case 0x0053: return "eq-data";
    default:     return nullptr;
    }
}

// Source: librepods docs/control_commands.md. Upstream extracted these from
// the iOS 19.1 Beta (23B5044l) Bluetooth stack, which is why the list runs so
// far past what the older AAP Definitions.md documents.
//
// s1 saw eleven identifiers it could not name: 0x18 0x17 0x25 0x23 0x1F 0x24
// 0x26 0x2E 0x1B 0x35 0x3E. Every one of them is in this table.
const char* control_name(uint8_t id)
{
    switch (id) {
    case 0x01: return "mic-mode";
    case 0x05: return "button-send-mode";
    case 0x06: return "owns-connection";
    case 0x0A: return "ear-detection";
    case 0x0D: return "listening-mode";
    case 0x12: return "voice-trigger-siri";
    case 0x14: return "single-click-mode";
    case 0x15: return "double-click-mode";
    case 0x16: return "click-hold-mode";
    case 0x17: return "double-click-interval";
    case 0x18: return "click-hold-interval";
    case 0x1A: return "listening-mode-configs";
    case 0x1B: return "one-bud-anc-mode";
    case 0x1C: return "crown-rotation-direction";
    case 0x1E: return "auto-answer-mode";
    case 0x1F: return "chime-volume";
    case 0x20: return "connect-automatically";
    case 0x23: return "volume-swipe-interval";
    case 0x24: return "call-management-config";
    case 0x25: return "volume-swipe-mode";
    case 0x26: return "adaptive-volume-config";
    case 0x27: return "software-mute-config";
    case 0x28: return "conversation-detect";
    case 0x29: return "ssl";
    case 0x2C: return "hearing-aid-enrolled/enabled";
    case 0x2E: return "auto-anc-strength";
    case 0x2F: return "hps-gain-swipe";
    case 0x30: return "hrm-state";
    case 0x31: return "in-case-tone-config";
    case 0x32: return "siri-multitone-config";
    case 0x33: return "hearing-assist-config";
    case 0x34: return "allow-off-listening-mode";
    case 0x35: return "sleep-detection-config";
    case 0x36: return "allow-auto-connect";
    case 0x37: return "ppe-toggle-config";
    case 0x38: return "ppe-cap-level-config";
    case 0x39: return "raw-gestures-config";
    case 0x3A: return "temporary-pairing-config";
    case 0x3B: return "dynamic-end-of-charge";
    case 0x3C: return "system-siri-message-config";
    case 0x3D: return "hearing-aid-generic-config";
    case 0x3E: return "uplink-eq-bud-config";
    case 0x3F: return "uplink-eq-source-config";
    case 0x40: return "in-case-tone-volume";
    case 0x41: return "disable-button-input";
    default:   return nullptr;
    }
}

std::string opcode_label(std::span<const uint8_t> packet)
{
    char buf[96];

    // The handshake ack uses its own header. Observed on the A3056, s1 #1:
    //   01 00 04 00 00 00 01 00 03 00 ...
    if (is_handshake_ack(packet)) return "handshake-ack";

    if (!has_std_header(packet)) return "non-standard-header";
    if (packet.size() < 6) return "truncated";

    const uint16_t op = opcode_of(packet);

    // Opcode 0x09 is a generic CONTROL carrier, not "noise control". Byte 6
    // is the identifier, byte 7 the value. Established in s1 when the A3056
    // dumped fourteen distinct 0x09 packets on connect and only one of them
    // was noise control; the identifier names arrived in s2 from upstream's
    // control_commands.md.
    if (op == kOpControl) {
        if (packet.size() < 8) return "control (truncated)";
        const char* name = control_name(packet[6]);
        if (name)
            std::snprintf(buf, sizeof(buf), "control: %s = 0x%02X", name, packet[7]);
        else
            std::snprintf(buf, sizeof(buf), "control: id 0x%02X ? = 0x%02X", packet[6], packet[7]);
        return buf;
    }

    // Battery is the one we are hunting, so say more about it than the label
    // strictly needs to -- the component count is the first thing worth
    // knowing and it costs one byte to read. This is NOT a parse: it reports
    // the packet's own claim about its shape, which is exactly the thing
    // milestone 2 has to check against the iPhone.
    if (op == kOpBattery && packet.size() >= 7) {
        std::snprintf(buf, sizeof(buf), "*** BATTERY *** (claims %u component%s, %zu bytes)",
                      packet[6], packet[6] == 1 ? "" : "s", packet.size());
        return buf;
    }

    if (const char* name = opcode_name(op)) return name;

    std::snprintf(buf, sizeof(buf), "opcode 0x%04X ?", op);
    return buf;
}

std::string hexline(std::span<const uint8_t> data)
{
    std::string out;
    char buf[8];
    for (size_t i = 0; i < data.size(); ++i) {
        std::snprintf(buf, sizeof(buf), "%02X", data[i]);
        if (i) out += ' ';
        out += buf;
    }
    return out;
}

std::string hexcompact(std::span<const uint8_t> data)
{
    std::string out;
    char buf[4];
    for (uint8_t b : data) {
        std::snprintf(buf, sizeof(buf), "%02x", b);
        out += buf;
    }
    return out;
}

std::string hexdump(std::span<const uint8_t> data, const std::string& indent)
{
    std::string out;
    char buf[32];

    for (size_t off = 0; off < data.size(); off += 16) {
        out += indent;
        std::snprintf(buf, sizeof(buf), "%04zx  ", off);
        out += buf;

        for (size_t i = 0; i < 16; ++i) {
            if (off + i < data.size()) {
                std::snprintf(buf, sizeof(buf), "%02X ", data[off + i]);
                out += buf;
            } else {
                out += "   ";
            }
            if (i == 7) out += ' ';
        }

        out += " |";
        for (size_t i = 0; i < 16 && off + i < data.size(); ++i) {
            unsigned char c = data[off + i];
            out += (std::isprint(c) ? static_cast<char>(c) : '.');
        }
        out += "|\n";
    }
    return out;
}

} // namespace airprobe::proto
