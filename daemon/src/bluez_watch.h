// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Scott Combs
#pragma once

// BluezWatch -- tells the pump when the pods are on THIS host.
//
// M1 connected once at startup and gave up. That is wrong in the ordinary
// case rather than an edge case: AirPods spend most of their life attached to
// the phone, and a daemon started while they are there serves nothing until
// it is restarted by hand.
//
// --- WHY BLUEZ AND NOT A RETRY TIMER ----------------------------------------
//
// The tempting shape is a loop that tries the L2CAP connect every N seconds
// until it works. RULES is explicit about why not: only one host holds the
// AAP channel, and the pods being on the iPhone is an ENVIRONMENT STATE, not
// a fault. A retry loop treats it as a fault and spends all day failing to
// connect to a device that is deliberately elsewhere -- burning the adapter,
// filling the log, and racing the phone every time the user picks the pods
// up.
//
// BlueZ already knows the answer. `org.bluez.Device1.Connected` is true
// exactly when the device is linked to this machine. Watching one property is
// free, correct, and silent while the pods are away.
//
// --- WHAT IT WATCHES --------------------------------------------------------
//
// Three signals, because the device object is not permanent:
//
//   PropertiesChanged on org.bluez.Device1 -- the Connected transitions
//     themselves, which is the interesting one.
//
//   InterfacesAdded / InterfacesRemoved -- the device object appears and
//     disappears. `bluetoothctl remove` then re-pair, or an adapter coming
//     back after rfkill, both produce a NEW object path. A watch bound to a
//     path discovered once at startup would go permanently deaf.
//
//   The org.bluez name itself -- bluetoothd restarts, and on a restart every
//     object path is rebuilt. Re-scanning on name-appeared costs one call and
//     saves a class of "worked until I updated bluez" bug.
//
// --- THE SYSTEM BUS ---------------------------------------------------------
//
// org.bluez lives on the system bus, so this is a SECOND connection: airprobed
// owns a name on the session bus and reads from the system bus. Reading
// device properties is permitted to ordinary users by the stock BlueZ policy.
// REGISTERING a battery provider is not necessarily, and that is s5's problem
// rather than this file's -- which is the reason this class only reads.
//
// If the system bus is unreachable, start() returns false and says why. The
// daemon falls back to M1's behaviour rather than exiting; a machine where
// this fails still has a working daemon for as long as the pods stay put.

#include <gio/gio.h>

#include <functional>
#include <string>

namespace airprobe::daemon {

class BluezWatch {
public:
    // Called on every transition, and once at start() with the current state.
    // `path` is the BlueZ object path, empty when the device is not known to
    // BlueZ at all -- which is a different situation from known-and-
    // disconnected and worth being able to say so.
    using ConnectedFn = std::function<void(bool connected, const std::string& path)>;

    explicit BluezWatch(std::string mac);
    ~BluezWatch();

    BluezWatch(const BluezWatch&) = delete;
    BluezWatch& operator=(const BluezWatch&) = delete;

    void on_connected(ConnectedFn fn) { cb_ = std::move(fn); }

    // Connects to the system bus, finds the device, and subscribes. Returns
    // false if the system bus or org.bluez is unreachable; the caller should
    // carry on without it rather than treat that as fatal.
    bool start();

    bool connected() const { return connected_; }
    const std::string& device_path() const { return path_; }

private:
    static void on_props_changed(GDBusConnection*, const gchar*, const gchar*,
                                 const gchar*, const gchar*, GVariant*, gpointer);
    static void on_interfaces_changed(GDBusConnection*, const gchar*, const gchar*,
                                      const gchar*, const gchar*, GVariant*, gpointer);
    static void on_bluez_appeared(GDBusConnection*, const gchar*, const gchar*, gpointer);
    static void on_bluez_vanished(GDBusConnection*, const gchar*, gpointer);

    // Re-reads the whole object tree and re-derives path_ / connected_.
    // Cheap, and the only thing that needs to be right after any disturbance.
    void rescan();

    // rescan() with a trailing-edge debounce. BlueZ fires InterfacesAdded
    // FOUR TIMES in about 300ms on connect, and InterfacesRemoved four times
    // on disconnect -- measured, on Scott's hardware, at s4. Each one drove a
    // full synchronous GetManagedObjects, so four blocking calls happened
    // where one would do. This waits for the burst to stop and then scans
    // once.
    //
    // It is NOT a poll interval. Nothing in this class polls: while the pods
    // are on the phone, no signal arrives and no scan runs. The window only
    // ever delays a scan that a signal already asked for, which is why it is
    // short -- longer than the burst it collapses buys nothing, and every
    // millisecond of it is added latency on noticing the tree moved.
    //
    // The Connected transition does NOT come through here. on_props_changed
    // acts immediately, so attaching to the pods is not slowed by this at
    // all. The one case that is: a device object appearing for the first time
    // since startup, where the path is learned from the scan.
    void rescan_soon();
    static int rescan_trampoline(void* self);
    void update(bool connected, std::string path);

    std::string mac_;        // uppercase, as BlueZ reports addresses
    std::string path_;
    bool        connected_ = false;
    bool        primed_    = false;   // has update() ever run

    GDBusConnection* sys_ = nullptr;
    guint sub_props_  = 0;
    guint sub_added_  = 0;
    guint sub_removed_ = 0;
    guint name_watch_ = 0;
    guint rescan_timer_ = 0;   // pending debounced rescan, 0 when none

    ConnectedFn cb_;
};

} // namespace airprobe::daemon
