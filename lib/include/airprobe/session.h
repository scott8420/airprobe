// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Scott Combs
#pragma once

// Session -- the AAP conversation, from socket to cached battery reading.
//
// THIS IS THE SEAM. Everything a consumer needs to know about talking to a
// pair of AirPods is behind this one class: open the channel, run the
// handshake sequence, pump it, read the cache. The CLI is its first consumer
// and `airprobed` will be its second, and neither of them will contain a
// line of protocol sequencing.
//
// WHY IT EXISTS. After s3's first checkpoint the three aap_* units were in
// the library but the SEQUENCER was still inside main.cpp -- handshake, wait
// for the handshake ack, send host capabilities, wait for the features ack,
// request notifications, re-send after 2s, listen. That is the part the
// daemon needs. Left in the CLI, s4 would have copied it, and then there
// would be two of them drifting apart. A protocol bug would have to be fixed
// twice.
//
// --- THE PUMP, AND WHY IT IS NOT A CALLBACK LOOP ----------------------------
//
// Session does not own a run loop. The consumer calls step() and Session
// advances by at most one packet. That is the shape the harder consumer
// needs: a D-Bus daemon already has a main loop and cannot be asked to give
// it up, and the CLI prints its nudge prompts on a timer of its own between
// reads. A run loop is four lines on top of step(); prising a consumer's loop
// back out of a callback-driven Session is not.
//
// --- TWO REPORTING MECHANISMS, AND WHY --------------------------------------
//
// step() RETURNS what arrived. Sent packets go to the on_packet() hook
// instead, because a single step can send more than one packet (capabilities
// and notifications both fire when an ack lands) and a return value cannot
// carry a list. The hook sees both directions so a consumer writing a
// transcript gets them correctly interleaved, which is exactly what the .cap
// file needs.
//
// --- WHAT IS DELIBERATELY NOT HERE ------------------------------------------
//
// No argv, no stdout, no capture file, no hexdump, no decision about how a
// battery reading is presented. Those are the CLI's job. If a change in here
// forces a change to how output LOOKS, the change is on the wrong side of
// the seam.
//
// The experiment knobs are likewise not here as an enum. SessionConfig takes
// spans, and the consumer points them at the proto:: constants it wants. The
// vocabulary "--caps d7" is the probe CLI's vocabulary; the library only
// needs to know which bytes to send.
//
// --- THE CACHE IS THE POINT -------------------------------------------------
//
// Battery is PUSH ONLY (CANON: push-only means the experiment is the
// environment, not the code). Opcode 0x0004 travels pods -> host and there is
// no request opcode, so no consumer can ever ASK for a reading. It can only
// be told, once, and then serve what it was told. That makes the cache the
// daemon's entire job, which is why it lives under the seam where both
// consumers get the same one -- along with its age, because a consumer
// deciding whether to show a number needs to know how old the number is, and
// "I have no reading" is a different answer from "I have a stale one".

#include "airprobe/aap_battery.h"
#include "airprobe/aap_socket.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace airprobe {

// Which bytes to send and how to sequence them. Defaults match the working
// Linux client -- see aap_protocol.h for why each constant is what it is.
//
// The spans must outlive the Session. In practice they point at the
// proto:: constexpr arrays, which have static storage, so this costs the
// consumer nothing to honour.
struct SessionConfig {
    // Empty span means "do not send host capabilities at all" -- a real
    // experimental condition, not an omission.
    std::span<const uint8_t> host_caps;

    // Never empty in practice; if it is, no notifications are requested and
    // battery will never arrive.
    std::span<const uint8_t> notify_request;

    // true  -- wait for each ack before sending the next packet (upstream's
    //          behaviour, and the default).
    // false -- fire the whole sequence on 200ms timers, reproducing s1.
    bool ack_driven = true;

    // false skips the handshake entirely, to demonstrate that it is required.
    bool send_handshake = true;

    // setsockopt(BT_SECURITY_MEDIUM) before connect.
    bool require_security = true;

    // How long to wait for an ack before giving up and advancing anyway. A
    // stall must not hang the pump: "the A3056 never acks host capabilities"
    // is a finding worth capturing, not a reason to block forever.
    int ack_timeout_ms = 2000;

    // Upstream re-sends the notification request if no battery has arrived
    // shortly after the first one, which is a strong hint the first is
    // sometimes simply missed. Negative disables it.
    int notify_resend_ms = 2000;

    // Builds a config from the proto:: defaults. Free function rather than a
    // member initialiser so session.h does not have to include
    // aap_protocol.h for constants it otherwise does not need.
    static SessionConfig defaults();
};

// Where the handshake sequence has got to. Exposed because a consumer may
// want to say so, and because "not ready yet" and "ready but silent" are
// different situations for a daemon deciding what to serve.
enum class Phase {
    Closed,             // no socket
    WaitHandshakeAck,   // handshake sent, waiting for header 01 00 04 00
    WaitFeaturesAck,    // capabilities sent, waiting for opcode 0x2B
    Listening           // sequence complete; anything that arrives is news
};

const char* phase_name(Phase p);

class Session {
public:
    enum class Direction { Sent, Received };

    // Called for every packet in both directions, in order. `what` names an
    // outbound packet ("handshake", "host-capabilities") and is nullptr for
    // inbound ones, where the consumer can label the bytes itself with
    // proto::opcode_label if it wants a name.
    //
    // The span is valid only for the duration of the call.
    using PacketHook =
        std::function<void(Direction, std::span<const uint8_t>, const char* what)>;

    // The outcome of one step(). Several flags can be true at once -- the
    // handshake ack is a packet AND the thing that completes a phase -- which
    // is why this is a struct and not an enum.
    struct Step {
        bool packet  = false;   // a packet arrived
        bool battery = false;   // ...and it parsed as a battery report
        bool closed  = false;   // channel gone; stop pumping
        bool phase_changed = false;

        // A one-line protocol event worth recording: an ack timeout, a
        // notification re-send, a parse refusal. nullptr most steps.
        //
        // This exists so the .cap transcript keeps the notes it had when the
        // sequencer lived in main.cpp. Losing them would quietly degrade the
        // one durable artifact this project produces.
        const char* note = nullptr;

        // The packet that arrived, when `packet` is true. VALID UNTIL THE
        // NEXT step() -- it is a view into the Session's read buffer, not a
        // copy. A consumer that needs to keep the bytes must copy them.
        std::span<const uint8_t> bytes;

        // True when nothing at all happened, which is the normal case and
        // not an error. The pods speak on their schedule, not ours.
        bool idle() const { return !packet && !closed && !phase_changed && !note; }
    };

    explicit Session(SessionConfig cfg = SessionConfig::defaults());
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Set before open() to see the handshake packets go out.
    void on_packet(PacketHook hook);

    // Opens the channel and sends the first packet of the sequence. On
    // failure the returned ConnectResult carries the stage, errno and a
    // diagnosis, exactly as AapSocket does -- Session does not flatten that
    // detail, because "which stage" is the whole actionable content of a
    // connect failure.
    //
    // MAY BE CALLED AGAIN after close(). The handshake bookkeeping resets;
    // the cached battery reading, its timestamp and the refusal count do NOT.
    // That is what lets a daemon keep serving "this reading is four minutes
    // old" across the pods going to the phone and coming back. See the
    // comment in open() for which is which and why.
    ConnectResult open(const std::string& mac);

    void close();
    bool is_open() const { return sock_.is_open(); }

    // Advances by at most one packet. Waits up to timeout_ms for something
    // to arrive; returns an idle Step if nothing does. Also drives phase
    // timeouts and the notification re-send, so a consumer that pumps only
    // when it expects traffic will stall the sequence -- pump unconditionally.
    Step step(int timeout_ms);

    Phase phase() const { return phase_; }
    bool listening() const { return phase_ == Phase::Listening; }

    // --- the cache ------------------------------------------------------
    //
    // Empty until a battery packet has arrived AND parsed. Never populated
    // from a refused parse: a consumer must not be handed a half-report.
    const std::optional<BatteryReport>& last_battery() const { return last_battery_; }

    // Milliseconds since the cached report arrived, or -1 if there is none.
    // A consumer showing a number needs this; there is no way to refresh on
    // demand, so age is the only signal of staleness available.
    long long battery_age_ms() const;

    // How many opcode-0x0004 packets arrived whose shape the parser refused.
    // Nonzero means a structural surprise, and the consumer should say so
    // rather than silently serving an older reading.
    int battery_parse_refusals() const { return refusals_; }

    // Wall-clock since open(), for transcripts that timestamp relative to
    // the start of the run.
    long long elapsed_ms() const;

private:
    bool emit(std::span<const uint8_t> bytes, const char* what);
    bool send_caps();
    bool send_notify();
    void enter(Phase p);

    SessionConfig cfg_;
    AapSocket     sock_;
    PacketHook    hook_;

    Phase phase_ = Phase::Closed;
    std::chrono::steady_clock::time_point started_{};
    std::chrono::steady_clock::time_point phase_at_{};

    std::vector<uint8_t> buf_;

    std::optional<BatteryReport> last_battery_;
    std::chrono::steady_clock::time_point battery_at_{};
    bool battery_seen_ = false;
    int  refusals_     = 0;

    long long notify_sent_at_ = -1;
    bool      notify_resent_  = false;
};

} // namespace airprobe
