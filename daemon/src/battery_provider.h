// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Scott Combs
#pragma once

// BatteryProvider -- feeds the pods' charge to BlueZ, so the rest of the
// desktop can see it without anybody writing a UI.
//
// BlueZ has a hook for exactly this case: a client that can decode battery
// from some profile BlueZ does not speak registers itself with
// org.bluez.BatteryProviderManager1, exposes org.bluez.BatteryProvider1
// objects, and bluetoothd republishes them as org.bluez.Battery1 on the
// device. GNOME Settings -> Power, the system menu and every battery
// extension already read Battery1. So this file is the whole of "make the
// number appear on the desktop".
//
// Named source for every constant here, per CANON:
//   org.bluez.BatteryProviderManager(5) and org.bluez.Battery(5), BlueZ 5.87
//   docs, plus bluez/test/example-battery-provider (the working client, which
//   outranks the doc where they differ). The property set Device / Percentage
//   / Source is lifted from that example's get_battery_properties().
//
// --- ONE NUMBER, AND WHY THAT DISSOLVES THE TRANSPOSITION GATE --------------
//
// Battery1 carries a SINGLE byte Percentage per device. There is no shape in
// which three components reach it -- BlueZ keys one battery per device object
// and the pods are one device. So the provider must pick a figure.
//
// It publishes min(left, right). The case is excluded on purpose: a full case
// beside two dead pods would read 100% in Settings, which is true of the case
// and useless to the person about to take a call.
//
// min() is SYMMETRIC, and that is the interesting part. s2's open worry was
// that the parser might have left and right exchanged, and s4's handoff gated
// this session on settling it -- because a swapped mapping published to BlueZ
// is a swapped mapping the whole desktop believes. But min(L,R) == min(R,L).
// Nothing this file publishes can differ under a transposition, so this
// milestone is not gated after all.
//
// That is not the check being waved away. CANON's own rule -- check that the
// test you designed can fail in the specific way you fear -- cuts both ways:
// an output that CANNOT fail in that way is not evidence about the mapping,
// it is simply out of the mapping's reach. The check still gates s6, where
// the tile shows the two pods separately and a transposition is visible and
// wrong. It just does not gate this.
//
// Source carries the detail that Percentage cannot: "AAP -- L 86%, R 83%,
// case 100%". The doc calls Source informational and for debugging, which is
// exactly what it is being used for -- including as the one place a
// transposition would be visible from Settings.
//
// --- WHAT IS PUBLISHED, AND WHEN IT IS TAKEN AWAY ---------------------------
//
// The battery object exists only while the channel is open AND a pod reading
// is usable. When the pods go back to the phone the object is REMOVED, and
// that is a deliberate difference from our own session-bus interface, which
// keeps the reading and ages it.
//
// The reason is that Battery1 has no notion of age. A consumer of our own
// interface can read BatteryAgeMs and say "four minutes old". GNOME Settings
// cannot; it would render a three-hour-old 86% as the current charge,
// indefinitely, with no way to know better. That is THE FIELD THAT LIES
// again, one layer further out: a plausible-looking number that sits in a UI
// and gets believed. Where the wire format cannot carry the doubt, publish
// nothing rather than the number without it.
//
// --- EXPERIMENTAL ----------------------------------------------------------
//
// BatteryProviderManager1 shipped as [experimental] and on most distributions
// still only appears when bluetoothd runs with experimental features on
// (`Experimental = true` in /etc/bluetooth/main.conf, then restart
// bluetooth.service). If the interface is absent, start() says so in those
// words and the daemon carries on serving its own bus. Fedora's stock config
// has it off, so this is the likely first result on Scott's machine and is a
// configuration answer, not a code one.

#include "state.h"

#include <gio/gio.h>

#include <optional>
#include <string>

namespace airprobe::daemon {

class BatteryProvider {
public:
    explicit BatteryProvider(std::string mac);
    ~BatteryProvider();

    BatteryProvider(const BatteryProvider&) = delete;
    BatteryProvider& operator=(const BatteryProvider&) = delete;

    // Connects to the system bus, exports the provider root, and registers
    // with BlueZ when it is available. Returns false when the system bus is
    // unreachable or our own objects will not export -- both of which the
    // caller should survive rather than exit on. A missing BlueZ, or a BlueZ
    // without the manager interface, is NOT a false return: the name watch
    // will register later if it appears.
    bool start();

    // Feeds a snapshot. Adds, updates or removes the battery object as the
    // reading warrants. Cheap and idempotent -- it is called on every publish
    // and does nothing at all when the derived percentage has not moved.
    void publish(const State& s);

private:
    static void on_bluez_appeared(GDBusConnection*, const gchar*, const gchar*, gpointer);
    static void on_bluez_vanished(GDBusConnection*, const gchar*, gpointer);

    static void om_method_call(GDBusConnection*, const gchar*, const gchar*,
                               const gchar*, const gchar*, GVariant*,
                               GDBusMethodInvocation*, gpointer);
    static GVariant* battery_get_property(GDBusConnection*, const gchar*,
                                          const gchar*, const gchar*,
                                          const gchar*, GError**, gpointer);

    // One GetManagedObjects against org.bluez, filling adapter_ (the object
    // exporting BatteryProviderManager1) and device_ (our MAC's Device1).
    // Both are re-derived rather than cached across a bluetoothd restart,
    // because a restart rebuilds every path.
    bool resolve_paths();

    void register_with_bluez();
    void unregister_from_bluez();

    GVariant* battery_properties() const;   // a{sv}, floating
    void add_object();
    void remove_object();
    void emit_properties_changed();

    // min(left, right) over the components that have a usable level, and the
    // Source string that goes with it. Empty when neither pod gave a reading.
    std::optional<uint8_t> derive_percentage(const State& s) const;
    std::string            derive_source(const State& s) const;

    std::string mac_;         // uppercase, colons
    std::string root_;        // provider ID object path
    std::string object_;      // root_ + "/dev_XX_XX_..."

    GDBusConnection* sys_ = nullptr;
    GDBusNodeInfo*   om_node_      = nullptr;
    GDBusNodeInfo*   battery_node_ = nullptr;

    guint om_reg_      = 0;
    guint battery_reg_ = 0;
    guint name_watch_  = 0;

    std::string adapter_;     // /org/bluez/hciN
    std::string device_;      // /org/bluez/hciN/dev_XX_XX_...

    bool registered_ = false; // RegisterBatteryProvider has succeeded
    bool exported_   = false; // the battery object is currently announced

    uint8_t     percentage_ = 0;
    std::string source_;
};

} // namespace airprobe::daemon
