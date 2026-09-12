// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Scott Combs
#include "battery_provider.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>

namespace airprobe::daemon {

namespace {

constexpr const char* kBluezName     = "org.bluez";
constexpr const char* kBluezRoot     = "/";
constexpr const char* kDeviceIface   = "org.bluez.Device1";
constexpr const char* kOmIface       = "org.freedesktop.DBus.ObjectManager";
constexpr const char* kPropsIface    = "org.freedesktop.DBus.Properties";

// org.bluez.BatteryProviderManager(5), BlueZ 5.87.
constexpr const char* kMgrIface      = "org.bluez.BatteryProviderManager1";
// org.bluez.BatteryProvider1, as implemented by
// bluez/test/example-battery-provider (Battery.get_battery_properties).
constexpr const char* kProviderIface = "org.bluez.BatteryProvider1";

// Our provider ID. Any path we own will do; BlueZ only requires that the
// battery objects live beneath it.
constexpr const char* kProviderRoot  = "/io/github/scott8420/Airprobe/battery";

// Only GetManagedObjects is a method. InterfacesAdded / InterfacesRemoved are
// emitted by hand with g_dbus_connection_emit_signal, but they are declared
// here so an introspection of our tree describes what it actually does.
constexpr const char* kOmXml =
    "<node>"
    "  <interface name='org.freedesktop.DBus.ObjectManager'>"
    "    <method name='GetManagedObjects'>"
    "      <arg name='objects' type='a{oa{sa{sv}}}' direction='out'/>"
    "    </method>"
    "    <signal name='InterfacesAdded'>"
    "      <arg name='object' type='o'/>"
    "      <arg name='interfaces' type='a{sa{sv}}'/>"
    "    </signal>"
    "    <signal name='InterfacesRemoved'>"
    "      <arg name='object' type='o'/>"
    "      <arg name='interfaces' type='as'/>"
    "    </signal>"
    "  </interface>"
    "</node>";

// Percentage is a byte and Device is an object path, both lifted from the
// example provider. Source is optional there and we always supply it.
constexpr const char* kBatteryXml =
    "<node>"
    "  <interface name='org.bluez.BatteryProvider1'>"
    "    <property name='Device' type='o' access='read'/>"
    "    <property name='Percentage' type='y' access='read'/>"
    "    <property name='Source' type='s' access='read'/>"
    "  </interface>"
    "</node>";

std::string upper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return s;
}

// AA:BB:CC:DD:EE:FF -> dev_AA_BB_CC_DD_EE_FF, which is how BlueZ names
// device objects and how the example provider names its own leaves.
std::string dev_leaf(const std::string& mac)
{
    std::string out = "dev_";
    for (char c : mac) out += (c == ':') ? '_' : c;
    return out;
}

} // namespace

BatteryProvider::BatteryProvider(std::string mac)
    : mac_(upper(std::move(mac))),
      root_(kProviderRoot),
      object_(std::string(kProviderRoot) + "/" + dev_leaf(upper(mac_)))
{
}

BatteryProvider::~BatteryProvider()
{
    if (name_watch_) g_bus_unwatch_name(name_watch_);
    if (sys_) {
        // Best effort: tell BlueZ to stop watching us before the bus
        // connection goes. It survives us not managing it -- the name
        // disappearing is enough -- but leaving cleanly is cheap.
        if (registered_) unregister_from_bluez();
        if (battery_reg_) g_dbus_connection_unregister_object(sys_, battery_reg_);
        if (om_reg_)      g_dbus_connection_unregister_object(sys_, om_reg_);
        g_object_unref(sys_);
    }
    if (om_node_)      g_dbus_node_info_unref(om_node_);
    if (battery_node_) g_dbus_node_info_unref(battery_node_);
}

bool BatteryProvider::start()
{
    GError* err = nullptr;

    om_node_ = g_dbus_node_info_new_for_xml(kOmXml, &err);
    if (!om_node_) {
        spdlog::error("battery provider: ObjectManager XML failed to parse: {}",
                      err ? err->message : "(no message)");
        if (err) g_error_free(err);
        return false;
    }
    battery_node_ = g_dbus_node_info_new_for_xml(kBatteryXml, &err);
    if (!battery_node_) {
        spdlog::error("battery provider: BatteryProvider1 XML failed to parse: {}",
                      err ? err->message : "(no message)");
        if (err) g_error_free(err);
        return false;
    }

    // The same shared system-bus connection BluezWatch uses: g_bus_get_sync
    // hands out one GDBusConnection per bus type per process, so this is a
    // reference rather than a second socket.
    sys_ = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &err);
    if (!sys_) {
        spdlog::warn("battery provider: no system bus ({}). The pods' charge "
                     "will not appear in Settings; airprobed's own interface "
                     "is unaffected.",
                     err ? err->message : "(no message)");
        if (err) g_error_free(err);
        return false;
    }

    static const GDBusInterfaceVTable om_vtable = {
        &BatteryProvider::om_method_call, nullptr, nullptr, { nullptr }
    };

    om_reg_ = g_dbus_connection_register_object(
        sys_, root_.c_str(), om_node_->interfaces[0], &om_vtable,
        this, nullptr, &err);

    if (om_reg_ == 0) {
        spdlog::error("battery provider: could not export {}: {}", root_,
                      err ? err->message : "(no message)");
        if (err) g_error_free(err);
        return false;
    }

    // BlueZ may not be up yet, and it restarts. Register on every appearance;
    // drop our bookkeeping on every disappearance, because a restarted
    // bluetoothd has forgotten us and rebuilt every object path it owns.
    name_watch_ = g_bus_watch_name_on_connection(
        sys_, kBluezName, G_BUS_NAME_WATCHER_FLAGS_NONE,
        &BatteryProvider::on_bluez_appeared, &BatteryProvider::on_bluez_vanished,
        this, nullptr);

    return true;
}

void BatteryProvider::on_bluez_appeared(GDBusConnection*, const gchar*,
                                        const gchar*, gpointer user_data)
{
    static_cast<BatteryProvider*>(user_data)->register_with_bluez();
}

void BatteryProvider::on_bluez_vanished(GDBusConnection*, const gchar*,
                                        gpointer user_data)
{
    auto* self = static_cast<BatteryProvider*>(user_data);
    if (self->registered_)
        spdlog::info("battery provider: bluetoothd went away; registration "
                     "lost. Will re-register when it returns.");
    self->registered_ = false;
    self->adapter_.clear();
    self->device_.clear();
    // exported_ describes OUR announcement, and bluetoothd has forgotten it
    // along with everything else, so it is no longer true either.
    self->exported_ = false;
    if (self->battery_reg_) {
        g_dbus_connection_unregister_object(self->sys_, self->battery_reg_);
        self->battery_reg_ = 0;
    }
}

bool BatteryProvider::resolve_paths()
{
    GError* err = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        sys_, kBluezName, kBluezRoot, kOmIface, "GetManagedObjects", nullptr,
        G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE, 3000,
        nullptr, &err);

    if (!reply) {
        spdlog::warn("battery provider: GetManagedObjects failed: {}",
                     err ? err->message : "(no message)");
        if (err) g_error_free(err);
        return false;
    }

    adapter_.clear();
    device_.clear();

    GVariant* objects = g_variant_get_child_value(reply, 0);
    GVariantIter it;
    const gchar* obj_path = nullptr;
    GVariant* ifaces = nullptr;

    g_variant_iter_init(&it, objects);
    while (g_variant_iter_next(&it, "{&o@a{sa{sv}}}", &obj_path, &ifaces)) {
        if (adapter_.empty()) {
            GVariant* mgr = g_variant_lookup_value(ifaces, kMgrIface, nullptr);
            if (mgr) { adapter_ = obj_path; g_variant_unref(mgr); }
        }
        if (device_.empty()) {
            GVariant* dev = g_variant_lookup_value(ifaces, kDeviceIface,
                                                   G_VARIANT_TYPE("a{sv}"));
            if (dev) {
                const gchar* addr = nullptr;
                if (g_variant_lookup(dev, "Address", "&s", &addr) && addr
                    && upper(addr) == mac_)
                    device_ = obj_path;
                g_variant_unref(dev);
            }
        }
        g_variant_unref(ifaces);
    }

    g_variant_unref(objects);
    g_variant_unref(reply);

    if (adapter_.empty()) {
        // The single most likely outcome on a stock Fedora, and it is a
        // configuration answer rather than a bug. Say the whole fix.
        spdlog::warn("battery provider: bluetoothd exports no {} on any "
                     "adapter. That interface is experimental: set "
                     "'Experimental = true' under [General] in "
                     "/etc/bluetooth/main.conf and "
                     "`sudo systemctl restart bluetooth`. Until then the pods' "
                     "charge will not reach Settings -> Power; airprobed's own "
                     "interface is unaffected.",
                     kMgrIface);
        return false;
    }

    if (device_.empty())
        spdlog::warn("battery provider: {} is not a device BlueZ knows about. "
                     "Nothing to attach a battery to yet.", mac_);

    return true;
}

namespace {

void on_register_done(GObject* src, GAsyncResult* res, gpointer user_data)
{
    GError* err = nullptr;
    GVariant* reply = g_dbus_connection_call_finish(
        G_DBUS_CONNECTION(src), res, &err);

    auto* flag = static_cast<bool*>(user_data);

    if (!reply) {
        if (err && err->code != G_IO_ERROR_CANCELLED)
            spdlog::warn("battery provider: RegisterBatteryProvider refused: {}",
                         err->message);
        if (err) g_error_free(err);
        return;
    }
    g_variant_unref(reply);
    *flag = true;
    spdlog::info("battery provider: registered with BlueZ");
}

} // namespace

void BatteryProvider::register_with_bluez()
{
    if (registered_) return;
    if (!resolve_paths()) return;

    // Async on purpose. This runs from a name-appeared callback on the main
    // loop, and a synchronous call here would stall the session-bus object
    // for as long as bluetoothd took to answer.
    g_dbus_connection_call(
        sys_, kBluezName, adapter_.c_str(), kMgrIface,
        "RegisterBatteryProvider", g_variant_new("(o)", root_.c_str()),
        nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr,
        &on_register_done, &registered_);
}

void BatteryProvider::unregister_from_bluez()
{
    if (adapter_.empty()) return;
    g_dbus_connection_call(
        sys_, kBluezName, adapter_.c_str(), kMgrIface,
        "UnregisterBatteryProvider", g_variant_new("(o)", root_.c_str()),
        nullptr, G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr, nullptr);
    registered_ = false;
}

// ---------------------------------------------------------------------------
// The number
// ---------------------------------------------------------------------------

std::optional<uint8_t> BatteryProvider::derive_percentage(const State& s) const
{
    std::optional<uint8_t> best;
    for (const char* key : { "left", "right" }) {
        auto it = s.components.find(key);
        if (it == s.components.end()) continue;
        if (!it->second.level) continue;               // present but unreadable
        if (!best || *it->second.level < *best) best = it->second.level;
    }
    return best;
}

std::string BatteryProvider::derive_source(const State& s) const
{
    // Informational, per org.bluez.Battery(5). It is also the only place a
    // left/right transposition would be visible to somebody looking at
    // Settings rather than at our own bus, which is why the per-pod figures
    // go in it rather than a bare protocol name.
    // "at connect" is not decoration. bluetoothd reads Source ONCE, when the
    // object is added, and ignores every later value -- verified on hardware:
    // Percentage tracked 99 -> 96 while Source sat at the 99/100 it was given
    // ten minutes earlier. So this string describes the first packet after
    // the channel opened, and saying otherwise would make it the exact thing
    // this file refuses to publish: a precise-looking number implying a
    // currency it does not have.
    std::string out = "AAP at connect";
    const char* sep = " -- ";
    for (auto [key, label] : { std::pair{"left", "L"},
                               std::pair{"right", "R"},
                               std::pair{"case", "case"} }) {
        auto it = s.components.find(key);
        if (it == s.components.end()) continue;
        out += sep;
        sep = ", ";
        out += label;
        out += ' ';
        if (it->second.level) out += std::to_string(*it->second.level) + "%";
        else                  out += "--";
    }
    return out;
}

// ---------------------------------------------------------------------------
// Exporting
// ---------------------------------------------------------------------------

GVariant* BatteryProvider::battery_properties() const
{
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&b, "{sv}", "Device",
                          g_variant_new_object_path(device_.c_str()));
    g_variant_builder_add(&b, "{sv}", "Percentage",
                          g_variant_new_byte(percentage_));
    g_variant_builder_add(&b, "{sv}", "Source",
                          g_variant_new_string(source_.c_str()));
    return g_variant_builder_end(&b);
}

void BatteryProvider::om_method_call(GDBusConnection*, const gchar*,
                                     const gchar*, const gchar*,
                                     const gchar* method, GVariant*,
                                     GDBusMethodInvocation* inv,
                                     gpointer user_data)
{
    auto* self = static_cast<BatteryProvider*>(user_data);

    if (g_strcmp0(method, "GetManagedObjects") != 0) {
        g_dbus_method_invocation_return_error(
            inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
            "no such method: %s", method);
        return;
    }

    GVariantBuilder objs;
    g_variant_builder_init(&objs, G_VARIANT_TYPE("a{oa{sa{sv}}}"));

    if (self->exported_) {
        GVariantBuilder ifaces;
        g_variant_builder_init(&ifaces, G_VARIANT_TYPE("a{sa{sv}}"));
        g_variant_builder_add(&ifaces, "{s@a{sv}}", kProviderIface,
                              self->battery_properties());
        g_variant_builder_add(&objs, "{o@a{sa{sv}}}", self->object_.c_str(),
                              g_variant_builder_end(&ifaces));
    }

    g_dbus_method_invocation_return_value(
        inv, g_variant_new("(a{oa{sa{sv}}})", &objs));
}

GVariant* BatteryProvider::battery_get_property(GDBusConnection*, const gchar*,
                                                const gchar*, const gchar*,
                                                const gchar* name, GError** err,
                                                gpointer user_data)
{
    auto* self = static_cast<BatteryProvider*>(user_data);

    if (g_strcmp0(name, "Device") == 0)
        return g_variant_new_object_path(self->device_.c_str());
    if (g_strcmp0(name, "Percentage") == 0)
        return g_variant_new_byte(self->percentage_);
    if (g_strcmp0(name, "Source") == 0)
        return g_variant_new_string(self->source_.c_str());

    g_set_error(err, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                "no such property: %s", name);
    return nullptr;
}

void BatteryProvider::add_object()
{
    if (exported_) return;

    static const GDBusInterfaceVTable battery_vtable = {
        nullptr, &BatteryProvider::battery_get_property, nullptr, { nullptr }
    };

    GError* err = nullptr;
    battery_reg_ = g_dbus_connection_register_object(
        sys_, object_.c_str(), battery_node_->interfaces[0], &battery_vtable,
        this, nullptr, &err);

    if (battery_reg_ == 0) {
        spdlog::warn("battery provider: could not export {}: {}", object_,
                     err ? err->message : "(no message)");
        if (err) g_error_free(err);
        return;
    }

    GVariantBuilder ifaces;
    g_variant_builder_init(&ifaces, G_VARIANT_TYPE("a{sa{sv}}"));
    g_variant_builder_add(&ifaces, "{s@a{sv}}", kProviderIface,
                          battery_properties());

    g_dbus_connection_emit_signal(
        sys_, nullptr, root_.c_str(), kOmIface, "InterfacesAdded",
        g_variant_new("(o@a{sa{sv}})", object_.c_str(),
                      g_variant_builder_end(&ifaces)),
        nullptr);

    exported_ = true;
    spdlog::info("battery provider: published {}% to BlueZ ({})",
                 percentage_, source_);
}

void BatteryProvider::remove_object()
{
    if (!exported_) return;

    const gchar* gone[] = { kProviderIface, nullptr };
    g_dbus_connection_emit_signal(
        sys_, nullptr, root_.c_str(), kOmIface, "InterfacesRemoved",
        g_variant_new("(o^as)", object_.c_str(), gone), nullptr);

    if (battery_reg_) {
        g_dbus_connection_unregister_object(sys_, battery_reg_);
        battery_reg_ = 0;
    }

    exported_ = false;
    spdlog::info("battery provider: withdrew the reading from BlueZ. Battery1 "
                 "cannot say how old a number is, so no number is better than "
                 "a stale one.");
}

void BatteryProvider::emit_properties_changed()
{
    g_dbus_connection_emit_signal(
        sys_, nullptr, object_.c_str(), kPropsIface, "PropertiesChanged",
        g_variant_new("(s@a{sv}@as)", kProviderIface, battery_properties(),
                      g_variant_new_strv(nullptr, 0)),
        nullptr);
}

// ---------------------------------------------------------------------------

void BatteryProvider::publish(const State& s)
{
    if (!sys_ || !om_reg_) return;

    // Not registered yet is ordinary at startup and after a bluetoothd
    // restart; the name watch drives that, not this path.
    if (!registered_) return;

    // The device path is only learned from BlueZ, and the pods may have been
    // paired after we started. One cheap retry when we need it and lack it.
    if (device_.empty()) resolve_paths();

    const auto pct = derive_percentage(s);

    // The two conditions for a number reaching the desktop: the channel is up
    // (so the reading is current, not cached), and a pod actually gave us a
    // readable level. Either failing withdraws it. See the header for why
    // this differs from our own interface, which keeps the reading and ages
    // it instead.
    if (!s.channel_open || !pct || device_.empty()) {
        remove_object();
        return;
    }

    const std::string src = derive_source(s);
    const bool changed = (*pct != percentage_) || (src != source_);

    percentage_ = *pct;
    source_     = src;

    if (!exported_) { add_object(); return; }
    if (changed)      emit_properties_changed();
}

} // namespace airprobe::daemon
