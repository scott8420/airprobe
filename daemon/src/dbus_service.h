// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Scott Combs
#pragma once

// DbusService -- airprobed's face on the session bus.
//
// Everything GLib and everything D-Bus lives behind this class. It knows how
// to turn a State into properties and how to say when they changed. It does
// not know what AirPods are, and nothing above it in the daemon includes a
// GLib header.
//
// --- WHY GDBus, AND NOT sd-bus OR glibmm -----------------------------------
//
// s5 is a BlueZ battery provider, which is an ObjectManager SERVER --
// org.bluez.BatteryProviderManager1 calls RegisterBatteryProvider and then
// walks our tree. g_dbus_object_manager_server does that, every published
// BlueZ provider example is written against it, and hand-rolling an
// ObjectManager on sd-bus to save a dependency GNOME already has would be
// paying twice. GMainLoop is also where the pump wants to live.
//
// Not glibmm: airprobed has no GUI, so the C++ bindings would be a dependency
// bought for syntax alone, and the BlueZ documentation we will be reading at
// s5 is all C.
//
// --- THE SESSION BUS, FOR NOW ----------------------------------------------
//
// M1 owns its name on the session bus. That is the right bus for a per-user
// daemon holding a per-user device, and it is what the s6 GJS tile will talk
// to. s5's BlueZ provider needs the SYSTEM bus as well, since that is where
// org.bluez lives, and it needs a policy file to be allowed to speak there.
// Two connections, one process. That is a real piece of work and it is
// deliberately not in M1.

#include "state.h"

#include <gio/gio.h>

#include <functional>
#include <string>

namespace airprobe::daemon {

// Well-known name, object path, interface. io.github.scott8420 matches the
// app-id convention used across Scott's other projects.
inline constexpr const char* kBusName   = "io.github.scott8420.Airprobe";
inline constexpr const char* kObjectPath = "/io/github/scott8420/Airprobe";
inline constexpr const char* kInterface  = "io.github.scott8420.Airprobe1";

class DbusService {
public:
    // Called when the getter needs the battery age, which is the one property
    // that cannot come from a snapshot -- it is different every time it is
    // read. Returns milliseconds, or -1 when there is no cached reading.
    using AgeFn = std::function<long long()>;

    DbusService();
    ~DbusService();

    DbusService(const DbusService&) = delete;
    DbusService& operator=(const DbusService&) = delete;

    void set_age_source(AgeFn fn) { age_ = std::move(fn); }

    // Requests the well-known name. Returns false only if the XML failed to
    // parse, which would be a bug in this file rather than a runtime
    // condition; acquiring the name itself is asynchronous and reported
    // through the log.
    bool start();

    // Publishes a new snapshot. Emits PropertiesChanged for exactly the
    // properties whose values differ from the last call, and nothing at all
    // when nothing changed -- which is most ticks, since the pods speak
    // roughly never.
    void publish(const State& s);

    // The snapshot currently on the bus.
    const State& state() const { return state_; }

private:
    static void on_bus_acquired(GDBusConnection*, const gchar*, gpointer);
    static void on_name_acquired(GDBusConnection*, const gchar*, gpointer);
    static void on_name_lost(GDBusConnection*, const gchar*, gpointer);

    static GVariant* get_property_trampoline(GDBusConnection*, const gchar*,
                                             const gchar*, const gchar*,
                                             const gchar*, GError**, gpointer);

    GVariant* get_property(const char* name) const;
    GVariant* components_variant() const;
    void emit_changed(GVariant* changed);

    GDBusNodeInfo*   node_ = nullptr;
    GDBusConnection* conn_ = nullptr;
    guint owner_id_ = 0;
    guint reg_id_   = 0;

    State state_;
    bool  published_ = false;   // false until the first publish(), so the
                                // first one emits nothing (there is no
                                // previous value for anything to differ from)
    AgeFn age_;
};

} // namespace airprobe::daemon
