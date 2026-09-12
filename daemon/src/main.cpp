// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Scott Combs
// airprobed -- the Airprobe user-session daemon.
//
// Holds the AAP channel to one pair of AirPods, pumps a Session, and serves
// the cached battery reading on the session bus at
// io.github.scott8420.Airprobe.
//
// WHY THIS EXISTS AT ALL. Battery on the A3056 is push-only and the AAP
// channel is exclusive, so a reading can only be obtained by somebody who is
// already holding the channel and was already listening when the pods spoke.
// That somebody has to be a long-lived process. It cannot be the s6 Quick
// Settings tile: that is GJS running inside gnome-shell, which cannot open an
// L2CAP socket and would stall the compositor if it tried to do blocking IO.
// Hence a daemon, and hence D-Bus between them.
//
// Usage:
//   airprobed <MAC> [--verbose] [--tick-ms N]
//
// Find the MAC with `bluetoothctl devices`. It must be the CLASSIC paired
// address, not a BLE one -- AirPods rotate their BLE address every fifteen
// minutes or so and the classic address does not move.

#include "battery_provider.h"
#include "dbus_service.h"
#include "pump.h"

#include <glib.h>
#include <glib-unix.h>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace {

void usage()
{
    std::fputs(
        "airprobed -- Airprobe session daemon\n"
        "\n"
        "  airprobed <MAC> [options]\n"
        "\n"
        "  <MAC>           classic paired address of the pods\n"
        "                  (bluetoothctl devices)\n"
        "\n"
        "  --verbose       debug logging, including every packet in both\n"
        "                  directions\n"
        "  --tick-ms N     how often to pump the session (default 200)\n"
        "  --no-bluez-battery\n"
        "                  do not register as a BlueZ battery provider, so\n"
        "                  nothing appears in Settings -> Power. airprobed's\n"
        "                  own interface is unaffected.\n"
        "  --self-test     publish SYNTHETIC snapshots and open no channel.\n"
        "                  Exercises the D-Bus encoding with no hardware.\n"
        "                  Every number it publishes is invented.\n"
        "  --help          this\n"
        "\n"
        "  <MAC> may also come from AIRPROBE_MAC, which is how the systemd\n"
        "  unit supplies it. argv wins when both are present.\n"
        "\n"
        "Serves io.github.scott8420.Airprobe on the session bus.\n"
        "Inspect it with:\n"
        "  gdbus introspect --session -d io.github.scott8420.Airprobe \\\n"
        "                             -o /io/github/scott8420/Airprobe\n",
        stderr);
}

gboolean on_signal(gpointer loop)
{
    spdlog::info("signal received, shutting down");
    g_main_loop_quit(static_cast<GMainLoop*>(loop));
    return G_SOURCE_REMOVE;
}

} // namespace

int main(int argc, char** argv)
{
    using namespace airprobe::daemon;

    Pump::Options opts;

    // On by default: s5 exists so that the charge shows up in Settings
    // without anybody opting in. The flag is for taking the provider out of
    // the picture while diagnosing something else.
    bool bluez_battery = true;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];

        if (a == "--help" || a == "-h") { usage(); return 0; }
        else if (a == "--verbose" || a == "-v") { opts.verbose = true; }
        else if (a == "--self-test") { opts.self_test = true; }
        else if (a == "--no-bluez-battery") { bluez_battery = false; }
        else if (a == "--tick-ms" && i + 1 < argc) {
            opts.tick_ms = std::atoi(argv[++i]);
            if (opts.tick_ms <= 0) {
                std::fputs("--tick-ms must be positive\n", stderr);
                return 2;
            }
        }
        else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            usage();
            return 2;
        }
        else if (opts.mac.empty()) { opts.mac = a; }
        else {
            std::fputs("one MAC at a time. The AAP channel is exclusive and\n"
                       "this daemon holds exactly one.\n", stderr);
            return 2;
        }
    }

    // --self-test never touches the adapter, so it does not need a real
    // address; anything else does, and guessing one would open a channel to
    // a device that is not the pods.
    // Fall back to the environment. A D-Bus-activated or systemd-started
    // daemon has no terminal and nobody to type argv, so the unit file
    // supplies the address this way; see packaging/airprobed.service. argv
    // still wins, so running it by hand to test a different pair does not
    // require editing the unit.
    if (opts.mac.empty()) {
        if (const char* env = std::getenv("AIRPROBE_MAC")) opts.mac = env;
    }

    if (opts.mac.empty() && !opts.self_test) {
        std::fputs("no MAC given, and AIRPROBE_MAC is not set.\n\n", stderr);
        usage();
        return 2;
    }

    spdlog::set_level(opts.verbose ? spdlog::level::debug : spdlog::level::info);
    spdlog::set_pattern("[%H:%M:%S.%e] %^%l%$ %v");

    // Order matters slightly: the service is constructed first so the pump's
    // very first publish() -- which happens inside start(), and carries the
    // connect diagnosis when the connect failed -- has somewhere to land.
    DbusService service;

    // --self-test NEVER registers a provider, and that is not a convenience.
    // Self-test publishes invented numbers, labelled in capitals so that a
    // consumer of OUR interface can see they are invented. Battery1 has no
    // such label and no room for one: a synthetic 91% pushed through BlueZ
    // would reach GNOME Settings indistinguishable from a reading. The one
    // place the fake data must not go is the one place it cannot be marked.
    std::unique_ptr<BatteryProvider> provider;
    if (bluez_battery && !opts.self_test) {
        provider = std::make_unique<BatteryProvider>(opts.mac);
        if (!provider->start()) provider.reset();   // said why; carry on
    }

    Pump pump(opts, [&service, &provider](const State& s) {
        service.publish(s);
        if (provider) provider->publish(s);
    });
    service.set_age_source([&pump] { return pump.battery_age_ms(); });

    if (!service.start()) return 1;

    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);

    // Ctrl-C and systemctl --user stop both want a clean exit: the L2CAP
    // channel should be released rather than left to the kernel, because the
    // next thing that wants it is probably the phone.
    g_unix_signal_add(SIGINT,  on_signal, loop);
    g_unix_signal_add(SIGTERM, on_signal, loop);

    pump.start();

    g_main_loop_run(loop);
    g_main_loop_unref(loop);

    spdlog::info("airprobed stopped");
    return 0;
}
