// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Scott Combs
#include "pump.h"

#include <airprobe/aap_protocol.h>

#include <glib.h>

#include <iterator>
#include <spdlog/spdlog.h>

namespace airprobe::daemon {

Pump::Pump(Options opts, Publisher publish)
    : opts_(std::move(opts)), publish_(std::move(publish))
{
}

Pump::~Pump()
{
    if (timer_id_) g_source_remove(timer_id_);
}

int Pump::tick_trampoline(void* self)
{
    return static_cast<Pump*>(self)->tick() ? G_SOURCE_CONTINUE
                                            : G_SOURCE_REMOVE;
}

// --- the self test ---------------------------------------------------------
//
// RULES notes that Curvz had a headless selftest and Airprobe has none, and
// that this makes a green build syntax rather than evidence. That is true of
// everything below the seam -- no adapter, no pods, nothing to talk to. It is
// NOT true of the D-Bus encoding, which is pure data transformation and is
// also the fiddliest new code in M1: a{sa{sv}} with a key that is present or
// absent depending on an optional, and a PropertiesChanged that must fire on
// exactly the properties that moved.
//
// So this publishes two synthetic snapshots a second apart. The first
// populates every branch of the Components encoding at once -- a readable
// level, a charging state, and a case that is DISCONNECTED with raw 0xFF and
// no usable level, which is the real shape the s2 capture produced and the
// exact case where a flattened uint8 would have lied. The second changes two
// fields and nothing else, so the PropertiesChanged that follows is a claim
// about the change detection that can be checked against.
//
// The status string says SELF TEST in capitals for a reason: a consumer
// reading this object has no other way to know the numbers are invented, and
// a plausible-looking fake reading sitting on the session bus is precisely
// the kind of thing that gets believed later.
void Pump::run_self_test()
{
    spdlog::warn("SELF TEST: publishing synthetic data. No channel is opened "
                 "and no number here came from any hardware.");

    State s;
    s.address      = opts_.mac.empty() ? "00:00:00:00:00:00" : opts_.mac;
    s.phase        = "listening";
    s.channel_open = true;
    s.status       = "SELF TEST -- synthetic data, not a reading";
    s.have_battery = true;

    s.components["left"]  = { "discharging", uint8_t{96}, 96, false };
    s.components["right"] = { "charging",    uint8_t{94}, 94, false };

    // No level: disconnected, raw 0xFF. This is the component that proves the
    // encoding, because 0xFF must NOT appear as a percentage anywhere.
    s.components["case"]  = { "disconnected", std::nullopt, 0xFF, false };

    publish_(s);

    timer_id_ = g_timeout_add(1000, &Pump::self_test_trampoline, this);
}

int Pump::self_test_trampoline(void* self)
{
    auto* p = static_cast<Pump*>(self);

    State s;
    s.address      = p->opts_.mac.empty() ? "00:00:00:00:00:00" : p->opts_.mac;
    s.phase        = "listening";
    s.channel_open = true;
    s.status       = "SELF TEST -- synthetic data, not a reading";
    s.have_battery = true;

    // Exactly two differences from the first snapshot: the right pod dropped
    // a point, and the case came back with a real reading. Everything else is
    // identical, so a PropertiesChanged carrying anything other than
    // Components is change detection over-reporting.
    s.components["left"]  = { "discharging", uint8_t{96}, 96, false };
    s.components["right"] = { "charging",    uint8_t{95}, 95, false };
    s.components["case"]  = { "discharging", uint8_t{72}, 72, false };

    p->publish_(s);
    p->timer_id_ = 0;
    return G_SOURCE_REMOVE;
}

// --- lifecycle -------------------------------------------------------------

// Bounded retry schedule, in milliseconds, for the ACL-up-but-PSM-not-ready
// race. Three attempts over seven seconds and then silence until BlueZ says
// something changed. Deliberately short: if the channel is not available
// after this, the cause is not a race and waiting longer will not fix it.
namespace { constexpr int kRetryMs[] = { 1000, 2000, 4000 }; }
constexpr int kMaxAttempts = static_cast<int>(std::size(kRetryMs));

void Pump::start()
{
    if (opts_.self_test) { run_self_test(); return; }

    // The Session is created once and reused across every connect for the
    // life of the process. It is the cache.
    session_ = std::make_unique<Session>(SessionConfig::defaults());

    if (opts_.verbose) {
        // Both directions, in order, at debug level. The CLI writes a .cap
        // transcript from this same hook; the daemon does not -- capture is
        // the probe tool's job and a daemon quietly filling a file for months
        // is a different kind of program. The log is enough to answer "did
        // the handshake go out" at 3am.
        session_->on_packet([](Session::Direction dir,
                               std::span<const uint8_t> bytes,
                               const char* what) {
            if (dir == Session::Direction::Sent) {
                spdlog::debug("-> {} ({} bytes)", what ? what : "?", bytes.size());
            } else {
                spdlog::debug("<- {} ({} bytes)",
                              proto::opcode_label(bytes), bytes.size());
            }
        });
    }

    watch_ = std::make_unique<BluezWatch>(opts_.mac);
    watch_->on_connected([this](bool c, const std::string& p) { on_bluez(c, p); });

    if (!watch_->start()) {
        // No system bus, or org.bluez unreachable. Fall back to M1's
        // behaviour rather than exiting: a machine where this fails still has
        // a working daemon for as long as the pods stay put, and refusing to
        // run would be a worse answer than running blind.
        watch_.reset();
        status_ = "no BlueZ watch -- single connect attempt only";
        attach();
        return;
    }

    // watch_->start() has already delivered the current state through
    // on_bluez(), which attached if the pods are here and said so in the log
    // if they are not. Nothing left to do but let the main loop run.
}

void Pump::on_bluez(bool connected, const std::string& path)
{
    // Every transition resets the attempt budget. This is what keeps the
    // bounded retry from slowly becoming an unbounded one across a long
    // uptime.
    attempts_ = 0;
    if (retry_id_) { g_source_remove(retry_id_); retry_id_ = 0; }

    if (connected) {
        spdlog::info("bluez: pods are on this machine ({})",
                     path.empty() ? "no path" : path.c_str());
        attach();
    } else {
        // NOT an error, and the log should not read like one. This is the
        // pods going back to the phone, which is what AirPods do.
        if (session_ && session_->is_open()) {
            detach("pods left this machine");
        } else {
            status_ = path.empty()
                    ? "pods not known to BlueZ"
                    : "waiting for the pods to connect to this machine";
            spdlog::info("{}", status_);
            publish_now();
        }
    }
}

void Pump::attach()
{
    if (!session_ || session_->is_open()) return;

    ++attempts_;
    spdlog::info("opening AAP channel to {} (attempt {})", opts_.mac, attempts_);
    const ConnectResult r = session_->open(opts_.mac);

    if (!r) {
        status_ = std::string(stage_name(r.reached)) + ": " + r.diagnosis;
        spdlog::warn("connect failed at {} (errno {}): {}",
                     stage_name(r.reached), r.err, r.diagnosis);
        publish_now();
        schedule_retry();
        return;
    }

    status_ = "channel open";
    spdlog::info("channel open; pumping every {}ms", opts_.tick_ms);
    publish_now();

    if (timer_id_) g_source_remove(timer_id_);
    timer_id_ = g_timeout_add(opts_.tick_ms, &Pump::tick_trampoline, this);
}

void Pump::schedule_retry()
{
    // Only while BlueZ says the device is here. Without the watch we have no
    // way to know that, so a single attempt is all we are entitled to.
    if (!watch_ || !watch_->connected()) {
        spdlog::info("not retrying: BlueZ does not report the pods on this "
                     "machine. Waiting for it to say otherwise.");
        return;
    }
    if (attempts_ >= kMaxAttempts) {
        status_ += " (gave up after " + std::to_string(attempts_) + " attempts)";
        spdlog::warn("giving up for now. BlueZ still reports the pods "
                     "connected, so this is not the phone holding the channel "
                     "-- check `journalctl -u bluetooth` and the capability "
                     "note in build.sh.");
        publish_now();
        return;
    }

    const int delay = kRetryMs[attempts_ - 1];
    spdlog::info("retrying in {}ms", delay);
    retry_id_ = g_timeout_add(delay, &Pump::retry_trampoline, this);
}

int Pump::retry_trampoline(void* self)
{
    auto* p = static_cast<Pump*>(self);
    p->retry_id_ = 0;
    p->attach();
    return G_SOURCE_REMOVE;
}

void Pump::detach(const char* why)
{
    if (timer_id_) { g_source_remove(timer_id_); timer_id_ = 0; }
    if (session_) session_->close();

    status_ = why;
    spdlog::info("{}. Cached reading kept; it will age.", why);
    publish_now();
}

bool Pump::tick()
{
    // Zero timeout: this is somebody else's main loop and we do not get to
    // block in it. step() still does its phase-timeout and re-send work on
    // the way through, which is why it is called every tick and not only
    // when the fd looks ready.
    const Session::Step st = session_->step(0);

    if (st.note) spdlog::info("session: {}", st.note);

    if (st.battery) {
        // One line per genuine push. These are rare -- two in a 60s run --
        // so logging every one is not noise, it is the record.
        spdlog::info("battery: {}", session_->last_battery()->to_string());
    }

    if (st.closed) {
        // The socket went away without BlueZ telling us first -- the pods
        // were taken, or the link dropped. Tidy up and wait for the watch;
        // do NOT reconnect from here, or a flapping link becomes a spin.
        timer_id_ = 0;         // this source is about to be removed
        detach("channel closed by the peer");
        return false;
    }

    // An idle step cannot change State -- State holds only event-derived
    // fields, and battery age is deliberately not one of them (see state.h).
    // So the common case does no work at all.
    if (!st.idle()) publish_now();

    return true;
}

void Pump::publish_now()
{
    if (publish_) publish_(snapshot());
}

long long Pump::battery_age_ms() const
{
    return session_ ? session_->battery_age_ms() : -1;
}

State Pump::snapshot() const
{
    State s;
    s.address = opts_.mac;
    s.status  = status_;

    if (!session_) return s;

    s.phase          = phase_name(session_->phase());
    s.channel_open   = session_->is_open();
    s.parse_refusals = session_->battery_parse_refusals();

    if (const auto& b = session_->last_battery()) {
        s.have_battery = true;
        for (const BatteryComponent& c : b->components) {
            // component_name() already yields "left"/"right"/"case", which
            // are the keys we want on the bus. Reusing it rather than writing
            // a second mapping means the bus and the logs can never disagree
            // about what a component is called.
            ComponentView v;
            v.status          = status_name(c.status);
            v.level           = c.level;
            v.raw_level       = c.raw_level;
            v.framing_anomaly = c.framing_anomaly;
            s.components[component_name(c.component)] = std::move(v);
        }
    }

    return s;
}

} // namespace airprobe::daemon
