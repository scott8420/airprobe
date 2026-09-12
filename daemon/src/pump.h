// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Scott Combs
#pragma once

// Pump -- owns the Session and drives it from the GLib main loop.
//
// This is the half of airprobed that knows about AirPods. It holds the AAP
// channel, advances the session, and hands a State snapshot to whoever asked
// to be told. It does not know that D-Bus exists.
//
// --- WHY A TIMER AND NOT THE SOCKET FD --------------------------------------
//
// The obvious shape is g_unix_fd_add() on the L2CAP descriptor: wake only
// when bytes arrive. We are not doing that at M1, for two reasons.
//
// Session deliberately does not expose its fd. It owns the socket, and the
// phase timeouts and the notification re-send live inside step() -- which
// means step() must be called even when nothing has arrived, or a missing
// handshake ack hangs the sequence forever. session.h says so in as many
// words: pump unconditionally. An fd source alone would not do that; it would
// need a timer beside it anyway, and then there are two things to keep in
// sync.
//
// And the latency argument is not close. The battery push arrives ROUGHLY
// NINE SECONDS after the physical event that causes it -- the pods batch, and
// the s3 run measured it. Against a nine-second debounce, a 200ms tick is
// noise. Five poll() calls a second on an idle fd is not a cost worth
// designing around.
//
// If s6's tile ever wants sub-tick latency, the change is an fd accessor on
// Session plus a g_unix_fd_add beside this timer, not a redesign.
//
// --- LIFECYCLE, AND THE ONE THING IT MUST NOT DO ----------------------------
//
// BlueZ says when the pods are on this host; BluezWatch relays that and the
// pump attaches and detaches accordingly. There is NO periodic retry, and
// that is the decision s3's handoff asked to be made rather than discovered.
// The pods living on the iPhone is an ordinary state, not a fault. A daemon
// that retries the L2CAP connect on a timer would spend all day failing to
// reach a device that is deliberately elsewhere, and would race the phone
// every time the user picked the pods up.
//
// The one exception is BOUNDED and only applies while BlueZ says the device
// IS connected here. There is a real race: BlueZ reports Connected as soon as
// the ACL link is up, which can be before PSM 0x1001 will accept. So a failed
// connect in that state is retried a few times with backoff and then left
// alone until the next Connected transition. That is a race being absorbed,
// not a loop being spun -- the distinguishing test is that it cannot run
// while the pods are away, and it always terminates.
//
// --- THE SESSION OUTLIVES THE CHANNEL ---------------------------------------
//
// Session is not destroyed when the channel drops -- only stopped -- and the
// SAME Session is reopened when the pods come back. last_battery() and
// battery_age_ms() keep answering across the gap, which is the whole product:
// "the pods went back to the phone and this reading is four minutes old" is
// exactly the answer a consumer needs, and it is unavailable to a daemon that
// throws the Session away. Session::open() is documented to support this and
// resets its per-connection bookkeeping while keeping the cache.

#include "bluez_watch.h"
#include "state.h"

#include <airprobe/session.h>

#include <functional>
#include <memory>
#include <string>

namespace airprobe::daemon {

class Pump {
public:
    struct Options {
        std::string mac;
        int  tick_ms = 200;
        bool verbose = false;

        // Publish synthetic snapshots instead of opening a channel. See
        // run_self_test() for why this exists -- briefly: it is the only way
        // any part of Airprobe can be exercised without AirPods in the room,
        // and the D-Bus encoding is the one half that does not need them.
        bool self_test = false;
    };

    using Publisher = std::function<void(const State&)>;

    Pump(Options opts, Publisher publish);
    ~Pump();

    Pump(const Pump&) = delete;
    Pump& operator=(const Pump&) = delete;

    // Starts the BlueZ watch and attaches if the pods are already here.
    // Publishes immediately either way, so a consumer that connects at the
    // same moment sees a populated object rather than defaults -- including
    // when there is nothing to report, where saying WHY is the most useful
    // thing the daemon has.
    void start();

    // Milliseconds since the cached reading arrived, or -1. Survives the
    // channel closing; this is what DbusService's BatteryAgeMs getter calls.
    long long battery_age_ms() const;

private:
    static int tick_trampoline(void* self);
    static int self_test_trampoline(void* self);
    static int retry_trampoline(void* self);
    bool tick();                       // false removes the timer source
    void run_self_test();

    void on_bluez(bool connected, const std::string& path);
    void attach();                     // try to open the channel and pump
    void detach(const char* why);      // stop pumping, keep the cache
    void schedule_retry();

    State snapshot() const;
    void publish_now();

    Options   opts_;
    Publisher publish_;

    std::unique_ptr<Session>    session_;
    std::unique_ptr<BluezWatch> watch_;

    unsigned timer_id_ = 0;
    unsigned retry_id_ = 0;

    // Bounded, and only while BlueZ says the device is connected here. Reset
    // on every Connected transition, so an absent pair can never accumulate
    // attempts.
    int attempts_ = 0;

    // Last thing worth saying about the channel, in words. Seeded by the
    // connect result -- on failure it carries AapSocket's stage/errno
    // diagnosis verbatim, which is the whole actionable content of a connect
    // failure and would be thrown away by a bare boolean.
    std::string status_ = "starting";
};

} // namespace airprobe::daemon
