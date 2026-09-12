// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Scott Combs
#include "airprobe/session.h"
#include "airprobe/aap_protocol.h"

#include <spdlog/spdlog.h>

#include <thread>
#include <utility>

namespace airprobe {

namespace {

long long ms_between(std::chrono::steady_clock::time_point a,
                     std::chrono::steady_clock::time_point b)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
}

long long ms_since(std::chrono::steady_clock::time_point t)
{
    return ms_between(t, std::chrono::steady_clock::now());
}

} // namespace

const char* phase_name(Phase p)
{
    switch (p) {
    case Phase::Closed:           return "closed";
    case Phase::WaitHandshakeAck: return "wait-handshake-ack";
    case Phase::WaitFeaturesAck:  return "wait-features-ack";
    case Phase::Listening:        return "listening";
    }
    return "?";
}

SessionConfig SessionConfig::defaults()
{
    SessionConfig c;
    c.host_caps      = proto::kHostCapsD7;
    c.notify_request = proto::kRequestNotify5;
    return c;
}

Session::Session(SessionConfig cfg) : cfg_(cfg) {}

Session::~Session() { close(); }

void Session::on_packet(PacketHook hook) { hook_ = std::move(hook); }

bool Session::emit(std::span<const uint8_t> bytes, const char* what)
{
    if (bytes.empty()) return true;          // a skipped packet is not a failure
    if (!sock_.send_packet(bytes, what)) return false;
    if (hook_) hook_(Direction::Sent, bytes, what);
    return true;
}

bool Session::send_caps()
{
    return emit(cfg_.host_caps, "host-capabilities");
}

bool Session::send_notify()
{
    if (!emit(cfg_.notify_request, "request-notifications")) return false;
    notify_sent_at_ = elapsed_ms();
    return true;
}

void Session::enter(Phase p)
{
    phase_ = p;
    phase_at_ = std::chrono::steady_clock::now();
}

long long Session::elapsed_ms() const
{
    if (phase_ == Phase::Closed && started_.time_since_epoch().count() == 0) return 0;
    return ms_since(started_);
}

long long Session::battery_age_ms() const
{
    if (!last_battery_) return -1;
    return ms_since(battery_at_);
}

ConnectResult Session::open(const std::string& mac)
{
    auto result = sock_.connect_to(mac, cfg_.require_security);
    if (!result.ok) return result;

    started_ = std::chrono::steady_clock::now();

    // open() may be called again on a Session that has already run -- s4's
    // daemon reconnects when the pods come back from the phone, and it keeps
    // the same Session on purpose so the cached reading and its age survive
    // the gap.
    //
    // That makes two kinds of state distinguishable, and the distinction
    // belongs here rather than in a consumer:
    //
    //   PER CONNECTION, reset below -- the handshake bookkeeping. Leaving
    //     battery_seen_ true from a previous run would disable the
    //     notification re-send on every connection after the first, silently,
    //     and the symptom would be "battery arrives on the first connect and
    //     never again" a long way from the cause.
    //
    //   CACHED KNOWLEDGE, deliberately kept -- last_battery_, battery_at_ and
    //     refusals_. A reading does not stop being true because the channel
    //     dropped; it starts being old, which is what battery_age_ms() is
    //     for. And a parse refusal is evidence about the protocol, not about
    //     one connection.
    battery_seen_   = false;
    notify_resent_  = false;
    notify_sent_at_ = -1;

    enter(Phase::WaitHandshakeAck);

    if (cfg_.send_handshake) {
        if (!emit(proto::kHandshake, "handshake")) {
            close();
            result.ok = false;
            result.diagnosis = "channel opened but the handshake could not be written";
            return result;
        }
    } else {
        spdlog::warn("session: handshake SKIPPED -- expect total silence");
    }

    // Without a handshake there is no ack to wait for, and in sleep mode we
    // deliberately do not wait. Either way: fire the rest of the sequence up
    // front, exactly as s1 did, and fall straight into listening.
    //
    // The sleeps are the one place this library blocks. They belong to a
    // reproduction mode nobody uses in production -- the whole point of
    // --sequence sleep is to recreate a known failure -- so paying for it
    // with 400ms of stall in a path the daemon will never take is the right
    // trade against threading a timer through the state machine.
    if (!cfg_.ack_driven || !cfg_.send_handshake) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (!send_caps()) { close(); result.ok = false; return result; }
        if (!cfg_.host_caps.empty())
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (!send_notify()) { close(); result.ok = false; return result; }
        enter(Phase::Listening);
    }

    return result;
}

void Session::close()
{
    sock_.close();
    phase_ = Phase::Closed;
}

Session::Step Session::step(int timeout_ms)
{
    Step out;

    if (!sock_.is_open()) {
        out.closed = true;
        return out;
    }

    // --- phase timeouts --------------------------------------------------
    //
    // Never block on an ack that is not coming. Advancing anyway is not
    // papering over a failure: which ack failed to arrive is recorded, and
    // the run continues to produce evidence instead of hanging.
    if (phase_ != Phase::Listening && phase_ != Phase::Closed
        && ms_since(phase_at_) > cfg_.ack_timeout_ms) {

        if (phase_ == Phase::WaitHandshakeAck) {
            spdlog::warn("session: no handshake ack after {}ms -- sending capabilities anyway",
                         cfg_.ack_timeout_ms);
            out.note = "TIMEOUT waiting for handshake-ack";
            if (!send_caps()) { close(); out.closed = true; return out; }
            enter(cfg_.host_caps.empty() ? Phase::Listening : Phase::WaitFeaturesAck);
            if (phase_ == Phase::Listening && !send_notify()) {
                close(); out.closed = true; return out;
            }
        } else {
            spdlog::warn("session: no features ack after {}ms -- requesting notifications anyway",
                         cfg_.ack_timeout_ms);
            out.note = "TIMEOUT waiting for features-ack (opcode 0x2B)";
            if (!send_notify()) { close(); out.closed = true; return out; }
            enter(Phase::Listening);
        }
        out.phase_changed = true;
    }

    // --- notification re-send -------------------------------------------
    if (phase_ == Phase::Listening && !battery_seen_ && !notify_resent_
        && cfg_.notify_resend_ms >= 0 && notify_sent_at_ >= 0
        && elapsed_ms() - notify_sent_at_ >= cfg_.notify_resend_ms) {

        spdlog::info("session: no battery yet -- re-sending the notification request");
        if (!out.note) out.note = "re-sending notification request (no battery yet)";
        notify_resent_ = true;
        if (!send_notify()) { close(); out.closed = true; return out; }
    }

    // --- read ------------------------------------------------------------
    const int n = sock_.read_packet(buf_, timeout_ms);
    if (n < 0) {
        close();
        out.closed = true;
        if (!out.note) out.note = "channel closed by peer";
        return out;
    }
    if (n == 0) return out;     // timed out. Entirely normal.

    out.packet = true;
    out.bytes  = std::span<const uint8_t>(buf_);
    if (hook_) hook_(Direction::Received, out.bytes, nullptr);

    // --- battery ---------------------------------------------------------
    if (proto::opcode_of(out.bytes) == proto::kOpBattery) {
        battery_seen_ = true;
        if (auto report = parse_battery(out.bytes)) {
            last_battery_ = std::move(report);
            battery_at_   = std::chrono::steady_clock::now();
            out.battery   = true;
        } else {
            // The labeller said battery and the parser refused. That is a
            // structural surprise, not a nuisance. The cache is left alone
            // rather than cleared -- an older good reading beats no reading,
            // and the refusal count is how a consumer learns to distrust it.
            ++refusals_;
            if (!out.note) out.note = "PARSE REFUSED on an opcode 0x04 packet";
            spdlog::warn("session: battery parse refused ({} so far)", refusals_);
        }
    }

    // --- phase transitions, ack mode -------------------------------------
    if (phase_ == Phase::WaitHandshakeAck && proto::is_handshake_ack(out.bytes)) {
        if (!send_caps()) { close(); out.closed = true; return out; }
        enter(cfg_.host_caps.empty() ? Phase::Listening : Phase::WaitFeaturesAck);
        out.phase_changed = true;
        if (phase_ == Phase::Listening && !send_notify()) {
            close(); out.closed = true; return out;
        }
    }
    else if (phase_ == Phase::WaitFeaturesAck
             && proto::opcode_of(out.bytes) == proto::kOpFeaturesAck) {
        if (!send_notify()) { close(); out.closed = true; return out; }
        enter(Phase::Listening);
        out.phase_changed = true;
    }

    return out;
}

} // namespace airprobe
