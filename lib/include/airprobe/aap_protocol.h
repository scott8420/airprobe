// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Scott Combs
#pragma once

// AAP wire constants.
//
// SOURCES -- two of them now, and they disagree. Every constant below names
// which one it came from.
//
//   [doc]  librepods `docs/AAP Definitions.md`, captured from AirPods Pro 2
//          (USB-C) firmware 7A305. Lives in our docs/ and is uploaded each
//          session.
//   [code] librepods `linux/airpods_packets.h` and `linux/main.cpp` at commit
//          e6ebbed6 -- the client that demonstrably works on Linux today.
//
// s2 finding: where they disagree, [code] wins. The doc records packets its
// author observed once; the code records packets that keep working. Two of
// our s1 constants came from the doc and are wrong in [code]'s terms -- see
// kRequestNotify5 and kHostCapsD7 below. That divergence is the leading
// suspect for s1's missing battery packet.
//
// Scott's hardware is AirPods 4 (ANC), model A3056. Both sources are Pro 2.
// Divergence from BOTH is possible and already observed once (three header
// forms, fourteen 0x09 settings).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace airprobe::proto {

// --- Handshake ----------------------------------------------------------
// [doc] line 9 and [code] Connection::HANDSHAKE -- these AGREE byte for byte.
// Verified working against the A3056 in s1.
//
// NOTE THE HEADER: 00 00 04 00, not the 04 00 04 00 every other packet uses.
inline constexpr std::array<uint8_t, 16> kHandshake = {
    0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// --- Host capabilities (opcode 0x4D) ------------------------------------
//
// [doc] line 22 calls this "setting specific features for AirPods Pro 2" and
// gives payload byte 0xFF. [code] Connection::SET_SPECIFIC_FEATURES gives
// 0xD7 and sends it on EVERY connect, unconditionally, the moment the
// handshake ack arrives.
//
// `docs/opcodes.md` names 0x4D "Host capabilities" -- the host telling the
// pods what it can do. Read that way the payload is a capability bitmask, and
// 0xD7 vs 0xFF is not cosmetic: it is a different claim about the host. Which
// capabilities battery reporting sits behind is unknown, so both are
// selectable at runtime.
//
//   0xD7 = 1101 0111    0xFF = 1111 1111
inline constexpr std::array<uint8_t, 14> kHostCapsD7 = {
    0x04, 0x00, 0x04, 0x00, 0x4D, 0x00, 0xD7, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// [doc] line 22 form. Marked upstream "may work for airpods 4 anc also, NOT
// TESTED". This is what s1's --enable-features sent, and s1 did not send it
// by default.
inline constexpr std::array<uint8_t, 14> kHostCapsFF = {
    0x04, 0x00, 0x04, 0x00, 0x4D, 0x00, 0xFF, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// --- Requesting notifications (opcode 0x0F) -----------------------------
//
// THE LENGTHS DIFFER. [code] Connection::REQUEST_NOTIFICATIONS is ELEVEN
// bytes with FIVE mask bytes:
//
//     04 00 04 00 0F 00 FF FF FF FF FF
//
// [doc] line 35 gives TEN bytes with four:
//
//     04 00 04 00 0F 00 FF FF FE FF
//
// s1 sent the ten-byte form and got no battery packet in 29 packets. If the
// mask is a bitfield over notification classes, a missing fifth byte is a
// whole missing byte of classes -- and battery could be sitting in it. This
// is the most promising line of attack in s2, so the eleven-byte form is now
// the DEFAULT.
inline constexpr std::array<uint8_t, 11> kRequestNotify5 = {
    0x04, 0x00, 0x04, 0x00, 0x0F, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

// [doc] line 41 -- ten bytes, all-FF mask. s1's --alt-notify.
inline constexpr std::array<uint8_t, 10> kRequestNotify4FF = {
    0x04, 0x00, 0x04, 0x00, 0x0F, 0x00, 0xFF, 0xFF, 0xFF, 0xFF
};

// [doc] line 35 -- ten bytes, FE in byte 8. This is what s1 sent by default
// and it produced no battery packet. Kept so that failure can be reproduced
// deliberately rather than remembered.
inline constexpr std::array<uint8_t, 10> kRequestNotify4FE = {
    0x04, 0x00, 0x04, 0x00, 0x0F, 0x00, 0xFF, 0xFF, 0xFE, 0xFF
};

// --- Opcodes ------------------------------------------------------------
//
// [code] docs/opcodes.md. The opcode is a 16-bit LITTLE-ENDIAN integer at
// bytes 4-5, not the single byte s1 assumed. Every opcode seen so far has
// 0x00 in byte 5, so s1's byte-4-only reading has not been wrong yet -- but
// it is wrong in principle, and it is corrected here before it bites.
inline constexpr uint16_t kOpBattery      = 0x0004;  // pods -> host. PUSH ONLY.
inline constexpr uint16_t kOpEarDetection = 0x0006;
inline constexpr uint16_t kOpControl      = 0x0009;  // both directions
inline constexpr uint16_t kOpNotifyReq    = 0x000F;
inline constexpr uint16_t kOpDeviceInfo   = 0x001D;
inline constexpr uint16_t kOpFeaturesAck  = 0x002B;  // s1's 102-byte mystery
inline constexpr uint16_t kOpHostCaps     = 0x004D;

// Listening-mode values. [doc] line 101, [code] control_commands.md 0x0D.
inline constexpr uint8_t kNoiseOff          = 0x01;
inline constexpr uint8_t kNoiseCancellation = 0x02;
inline constexpr uint8_t kNoiseTransparency = 0x03;
inline constexpr uint8_t kNoiseAdaptive     = 0x04;

// Battery components. [doc] line 51 table. NOT VERIFIED against the A3056 --
// no battery packet has ever been seen from it. These exist so the labeller
// can annotate one the moment it arrives. Parsing is milestone 2 and happens
// only once a real packet exists to parse.
inline constexpr uint8_t kBatteryCase  = 0x08;
inline constexpr uint8_t kBatteryLeft  = 0x04;
inline constexpr uint8_t kBatteryRight = 0x02;

// True if the packet carries the 04 00 04 00 standard header.
bool has_std_header(std::span<const uint8_t> p);

// True if the packet is the handshake acknowledgement (header 01 00 04 00).
// Observed on the A3056, s1 packet #1.
bool is_handshake_ack(std::span<const uint8_t> p);

// 16-bit LE opcode at bytes 4-5. Returns 0xFFFF when the packet is too short
// or does not carry the standard header.
uint16_t opcode_of(std::span<const uint8_t> p);

// Name for an opcode, or nullptr when we have no name for it.
// Source: librepods docs/opcodes.md.
const char* opcode_name(uint16_t op);

// Name for an opcode-0x09 control identifier (byte 6), or nullptr.
// Source: librepods docs/control_commands.md, extracted upstream from the
// iOS 19.1 beta Bluetooth stack. This names all twelve of the 0x09 settings
// s1 saw and could not identify.
const char* control_name(uint8_t id);

// Best-effort label for a received packet. Returns something carrying a "?"
// when the shape is not recognised, which is information too.
std::string opcode_label(std::span<const uint8_t> packet);

// Canonical hexdump: offset, 16 bytes per row, ASCII gutter.
std::string hexdump(std::span<const uint8_t> data, const std::string& indent = "  ");

// One-line form: "04 00 04 00 04 00 03 ..."
std::string hexline(std::span<const uint8_t> data);

// Contiguous lowercase hex, no separators -- for the capture file, where the
// point is machine re-readability rather than human scanning.
std::string hexcompact(std::span<const uint8_t> data);

} // namespace airprobe::proto
