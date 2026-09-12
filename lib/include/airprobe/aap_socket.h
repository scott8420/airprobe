// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Scott Combs
#pragma once

// AapSocket -- opens the Apple Accessory Protocol L2CAP channel to a pair of
// AirPods and reports, in detail, exactly where and why it failed if it did.
//
// Milestone 1 scope: connect only. No handshake, no packets, no parsing. The
// point of this file at s1 is to answer one question on Scott's hardware --
// "can a plain L2CAP socket reach PSM 0x1001 on these pods, and if not, which
// of the known environment snags is biting?"
//
// The connection is against the STABLE CLASSIC PAIRED MAC (BDADDR_BREDR), not
// a BLE address. AirPods rotate their BLE address roughly every 15 minutes;
// the classic address does not move.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace airprobe {

// PSM 4097. The AAP channel. Source: librepods, AAP Definitions.md.
inline constexpr uint16_t kAapPsm = 0x1001;

// Where in the connect sequence we got to. Each stage has a distinct set of
// plausible causes, which is the whole reason the enum exists -- "it didn't
// connect" is not an actionable report.
enum class Stage {
    ParseAddress,   // string -> bdaddr_t
    AdapterCheck,   // is there a live Bluetooth adapter at all
    CreateSocket,   // socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP)
    SetSecurity,    // setsockopt(BT_SECURITY, BT_SECURITY_MEDIUM)
    Connect,        // connect() to PSM 0x1001
    Connected       // success
};

const char* stage_name(Stage s);

struct ConnectResult {
    bool ok = false;
    Stage reached = Stage::ParseAddress;
    int err = 0;                 // errno at the point of failure, 0 if none
    std::string diagnosis;       // human-readable "what this probably means"

    explicit operator bool() const { return ok; }
};

class AapSocket {
public:
    AapSocket() = default;
    ~AapSocket();

    AapSocket(const AapSocket&) = delete;
    AapSocket& operator=(const AapSocket&) = delete;
    AapSocket(AapSocket&&) noexcept;
    AapSocket& operator=(AapSocket&&) noexcept;

    // Attempts the full sequence. On failure the socket is closed again before
    // returning, so a failed AapSocket is safe to discard or retry.
    //
    // require_security -- when true, BT_SECURITY_MEDIUM is set before connect.
    // Whether the pods need this is an open question on Fedora; --insecure
    // exists so we can test both without a rebuild.
    ConnectResult connect_to(const std::string& mac, bool require_security = true);

    void close();
    bool is_open() const { return fd_ >= 0; }
    int fd() const { return fd_; }

    // Writes one packet. Returns false and logs on short write or error --
    // a partial write on SOCK_SEQPACKET means something is wrong, not that
    // we should retry the tail.
    bool send_packet(std::span<const uint8_t> bytes, const char* what);

    // Waits up to timeout_ms for one packet.
    //   >0  : bytes read, out is filled
    //    0  : timed out, out untouched (NORMAL -- the pods speak when they
    //         have something to say, not on our schedule)
    //   -1  : error or peer closed
    int read_packet(std::vector<uint8_t>& out, int timeout_ms);

private:
    int fd_ = -1;
};

// Maps (stage, errno) onto the known snags from the discovery session. Kept
// separate from connect_to() so the mapping can be read and corrected without
// touching the socket logic.
std::string diagnose(Stage stage, int err);

} // namespace airprobe
