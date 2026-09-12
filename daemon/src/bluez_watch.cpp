// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Scott Combs
#include "bluez_watch.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>

namespace airprobe::daemon {

namespace {

constexpr const char* kBluezName    = "org.bluez";
constexpr const char* kBluezRoot    = "/";
constexpr const char* kDeviceIface  = "org.bluez.Device1";

// Long enough to swallow the four-signal burst BlueZ emits on connect (about
// 300ms end to end at s4), short enough that it is invisible. See the comment
// on rescan_soon() -- this is a coalescing window, not a scan interval.
constexpr unsigned kRescanDebounceMs = 200;

std::string upper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return s;
}

} // namespace

BluezWatch::BluezWatch(std::string mac) : mac_(upper(std::move(mac))) {}

BluezWatch::~BluezWatch()
{
    if (name_watch_) g_bus_unwatch_name(name_watch_);
    if (rescan_timer_) g_source_remove(rescan_timer_);
    if (sys_) {
        if (sub_props_)   g_dbus_connection_signal_unsubscribe(sys_, sub_props_);
        if (sub_added_)   g_dbus_connection_signal_unsubscribe(sys_, sub_added_);
        if (sub_removed_) g_dbus_connection_signal_unsubscribe(sys_, sub_removed_);
        g_object_unref(sys_);
    }
}

bool BluezWatch::start()
{
    GError* err = nullptr;
    sys_ = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &err);
    if (!sys_) {
        spdlog::warn("bluez: no system bus ({}). Falling back to a single "
                     "connect attempt; the daemon will not notice the pods "
                     "arriving or leaving.",
                     err ? err->message : "(no message)");
        if (err) g_error_free(err);
        return false;
    }

    // Path filter is deliberately null. The device object path is not known
    // yet on the first scan and CHANGES when a device is removed and re-added,
    // so binding the subscription to a path discovered once would go deaf
    // exactly when something interesting happened. arg0 narrows it to
    // Device1 properties, and the callback checks the path itself.
    sub_props_ = g_dbus_connection_signal_subscribe(
        sys_, kBluezName, "org.freedesktop.DBus.Properties",
        "PropertiesChanged", nullptr, kDeviceIface,
        G_DBUS_SIGNAL_FLAGS_NONE, &BluezWatch::on_props_changed, this, nullptr);

    sub_added_ = g_dbus_connection_signal_subscribe(
        sys_, kBluezName, "org.freedesktop.DBus.ObjectManager",
        "InterfacesAdded", nullptr, nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, &BluezWatch::on_interfaces_changed, this, nullptr);

    sub_removed_ = g_dbus_connection_signal_subscribe(
        sys_, kBluezName, "org.freedesktop.DBus.ObjectManager",
        "InterfacesRemoved", nullptr, nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, &BluezWatch::on_interfaces_changed, this, nullptr);

    name_watch_ = g_bus_watch_name_on_connection(
        sys_, kBluezName, G_BUS_NAME_WATCHER_FLAGS_NONE,
        &BluezWatch::on_bluez_appeared, &BluezWatch::on_bluez_vanished,
        this, nullptr);

    rescan();
    return true;
}

int BluezWatch::rescan_trampoline(void* user)
{
    auto* self = static_cast<BluezWatch*>(user);
    self->rescan_timer_ = 0;
    self->rescan();
    return G_SOURCE_REMOVE;
}

void BluezWatch::rescan_soon()
{
    // Trailing edge: every signal in the burst pushes the scan back, so it
    // runs once, after the last one. A leading-edge version would scan on the
    // first signal and miss whatever the other three were telling us.
    if (rescan_timer_) g_source_remove(rescan_timer_);
    rescan_timer_ = g_timeout_add(kRescanDebounceMs,
                                  &BluezWatch::rescan_trampoline, this);
}

void BluezWatch::rescan()
{
    if (!sys_) return;

    GError* err = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        sys_, kBluezName, kBluezRoot, "org.freedesktop.DBus.ObjectManager",
        "GetManagedObjects", nullptr, G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
        G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, &err);

    if (!reply) {
        // bluetoothd not running is the ordinary reason and is recoverable --
        // the name watch will fire when it comes back.
        spdlog::warn("bluez: GetManagedObjects failed: {}",
                     err ? err->message : "(no message)");
        if (err) g_error_free(err);
        update(false, "");
        return;
    }

    GVariant* objects = g_variant_get_child_value(reply, 0);
    GVariantIter it;
    const gchar* obj_path = nullptr;
    GVariant* ifaces = nullptr;

    std::string found_path;
    bool found_connected = false;

    g_variant_iter_init(&it, objects);
    while (g_variant_iter_next(&it, "{&o@a{sa{sv}}}", &obj_path, &ifaces)) {
        GVariant* dev = g_variant_lookup_value(ifaces, kDeviceIface,
                                               G_VARIANT_TYPE("a{sv}"));
        if (dev) {
            const gchar* addr = nullptr;
            if (g_variant_lookup(dev, "Address", "&s", &addr) && addr
                && upper(addr) == mac_) {
                found_path = obj_path;
                gboolean c = FALSE;
                // Absent Connected is false, not an error: BlueZ omits
                // properties it has no value for.
                g_variant_lookup(dev, "Connected", "b", &c);
                found_connected = c;
            }
            g_variant_unref(dev);
        }
        g_variant_unref(ifaces);
        if (!found_path.empty()) break;
    }

    g_variant_unref(objects);
    g_variant_unref(reply);

    if (found_path.empty()) {
        spdlog::warn("bluez: {} is not a known device. Pair it first: "
                     "`bluetoothctl` then `devices`.", mac_);
    }

    update(found_connected, std::move(found_path));
}

void BluezWatch::update(bool connected, std::string path)
{
    const bool changed = !primed_ || connected != connected_ || path != path_;
    connected_ = connected;
    path_      = std::move(path);
    primed_    = true;

    if (changed && cb_) cb_(connected_, path_);
}

void BluezWatch::on_props_changed(GDBusConnection*, const gchar*,
                                  const gchar* object_path, const gchar*,
                                  const gchar*, GVariant* params, gpointer user)
{
    auto* self = static_cast<BluezWatch*>(user);
    if (self->path_.empty() || self->path_ != object_path) return;

    GVariant* changed = g_variant_get_child_value(params, 1);
    gboolean c = FALSE;
    if (g_variant_lookup(changed, "Connected", "b", &c)) {
        spdlog::info("bluez: {} Connected -> {}", object_path, c ? "true" : "false");
        self->update(c, self->path_);
    }
    g_variant_unref(changed);
}

void BluezWatch::on_interfaces_changed(GDBusConnection*, const gchar*,
                                       const gchar*, const gchar*,
                                       const gchar* signal, GVariant*,
                                       gpointer user)
{
    // Deliberately not parsing the payload. Either signal means the object
    // tree moved, and a rescan is one call that cannot be subtly wrong about
    // which case this was.
    spdlog::debug("bluez: {} -- rescan queued", signal);
    static_cast<BluezWatch*>(user)->rescan_soon();
}

void BluezWatch::on_bluez_appeared(GDBusConnection*, const gchar*,
                                   const gchar*, gpointer user)
{
    spdlog::info("bluez: bluetoothd is up");
    static_cast<BluezWatch*>(user)->rescan();
}

void BluezWatch::on_bluez_vanished(GDBusConnection*, const gchar*, gpointer user)
{
    // Not the same as the pods leaving, and worth different words in the log:
    // the channel we hold may well still be alive, but we have gone blind to
    // what happens to it next.
    spdlog::warn("bluez: bluetoothd went away");
    static_cast<BluezWatch*>(user)->update(false, "");
}

} // namespace airprobe::daemon
