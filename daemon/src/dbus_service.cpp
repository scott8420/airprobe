// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Scott Combs
#include "dbus_service.h"

#include <spdlog/spdlog.h>

#include <vector>

namespace airprobe::daemon {

namespace {

// --- the interface ---------------------------------------------------------
//
// Read-only throughout, and that is not laziness. Battery on the A3056 is
// PUSH ONLY: opcode 0x0004 travels pods -> host and there is no request
// opcode in the protocol (checked against librepods' Linux client and
// librepods-rs, neither of which has one). So there is no Refresh() method
// here and there never will be. A method that cannot do the thing its name
// promises is worse than its absence -- a consumer would call it, get no
// error, and conclude the reading it then read was fresh.
//
// BatteryAgeMs is annotated EmitsChangedSignal=false. It changes every
// millisecond without anything happening, so signalling it would mean a
// PropertiesChanged on every tick forever and would train consumers to ignore
// the signal. Consumers read it when they are about to show a number.
//
// Components is a{sa{sv}} rather than a flat set of LeftLevel/RightLevel/
// CaseLevel properties, because the three distinctions the library works hard
// to preserve all survive that shape and none of them survive a flat uint8:
//
//   component absent from the packet  -> no key in the outer map
//   present but unreadable            -> no "Level" key in the inner dict
//   readable                          -> "Level" present, 0-100
//
// A flat CaseLevel property would have to be 0 or 255 in the case the s2
// capture actually produced, and 0 is a plausible-looking lie that would sit
// in a UI indefinitely.
constexpr const char* kIntrospectionXml = R"XML(
<node>
  <interface name="io.github.scott8420.Airprobe1">
    <property name="Address"       type="s"       access="read"/>
    <property name="Phase"         type="s"       access="read"/>
    <property name="ChannelOpen"   type="b"       access="read"/>
    <property name="Status"        type="s"       access="read"/>
    <property name="HaveBattery"   type="b"       access="read"/>
    <property name="ParseRefusals" type="i"       access="read"/>
    <property name="Components"    type="a{sa{sv}}" access="read"/>
    <property name="BatteryAgeMs"  type="x"       access="read">
      <annotation name="org.freedesktop.DBus.Property.EmitsChangedSignal"
                  value="false"/>
    </property>
  </interface>
</node>
)XML";

} // namespace

// The vtable needs a plain function pointer and the getter needs `this`, so
// the instance rides in as user_data and the trampoline unpacks it.
GVariant* DbusService::get_property_trampoline(GDBusConnection*, const gchar*,
                                               const gchar*, const gchar*,
                                               const gchar* property,
                                               GError** error, gpointer user)
{
    auto* self = static_cast<DbusService*>(user);
    GVariant* v = self->get_property(property);
    if (!v) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                    "no such property: %s", property);
    }
    return v;   // floating; GDBus sinks it
}

DbusService::DbusService() = default;

DbusService::~DbusService()
{
    if (reg_id_ && conn_) g_dbus_connection_unregister_object(conn_, reg_id_);
    if (owner_id_)        g_bus_unown_name(owner_id_);
    if (node_)            g_dbus_node_info_unref(node_);
}

bool DbusService::start()
{
    GError* err = nullptr;
    node_ = g_dbus_node_info_new_for_xml(kIntrospectionXml, &err);
    if (!node_) {
        spdlog::error("dbus: introspection XML failed to parse: {}",
                      err ? err->message : "(no message)");
        if (err) g_error_free(err);
        return false;
    }

    // G_BUS_NAME_OWNER_FLAGS_NONE: we do NOT replace an existing owner. Two
    // airprobed processes would fight over one exclusive L2CAP channel, and
    // the second one losing the name loudly is the behaviour we want.
    owner_id_ = g_bus_own_name(G_BUS_TYPE_SESSION, kBusName,
                               G_BUS_NAME_OWNER_FLAGS_NONE,
                               &DbusService::on_bus_acquired,
                               &DbusService::on_name_acquired,
                               &DbusService::on_name_lost,
                               this, nullptr);
    return true;
}

void DbusService::on_bus_acquired(GDBusConnection* conn, const gchar*,
                                  gpointer user)
{
    auto* self = static_cast<DbusService*>(user);
    self->conn_ = conn;

    // method_call is null: the interface has no methods, deliberately, and
    // set_property is null because every property is read-only.
    GDBusInterfaceVTable vtable{};
    vtable.get_property = &DbusService::get_property_trampoline;

    GError* err = nullptr;
    self->reg_id_ = g_dbus_connection_register_object(
        conn, kObjectPath, self->node_->interfaces[0], &vtable,
        self, nullptr, &err);

    if (self->reg_id_ == 0) {
        spdlog::error("dbus: could not export {}: {}", kObjectPath,
                      err ? err->message : "(no message)");
        if (err) g_error_free(err);
        return;
    }
    spdlog::info("dbus: exported {} on the session bus", kObjectPath);
}

void DbusService::on_name_acquired(GDBusConnection*, const gchar* name, gpointer)
{
    spdlog::info("dbus: own {}", name);
}

void DbusService::on_name_lost(GDBusConnection* conn, const gchar* name, gpointer)
{
    // Two distinct situations share this callback and they need different
    // words, because one is a misconfigured machine and the other is a
    // second copy of the daemon -- and confusing them wastes a debugging
    // session.
    if (!conn) {
        spdlog::error("dbus: no session bus. Is DBUS_SESSION_BUS_ADDRESS set? "
                      "(running under ssh or a bare tty will do this)");
    } else {
        spdlog::error("dbus: lost {} -- another airprobed is probably already "
                      "running. Only one can hold the AAP channel anyway.",
                      name);
    }
}

// --- properties ------------------------------------------------------------

GVariant* DbusService::components_variant() const
{
    GVariantBuilder outer;
    g_variant_builder_init(&outer, G_VARIANT_TYPE("a{sa{sv}}"));

    for (const auto& [key, c] : state_.components) {
        GVariantBuilder inner;
        g_variant_builder_init(&inner, G_VARIANT_TYPE("a{sv}"));

        g_variant_builder_add(&inner, "{sv}", "Status",
                              g_variant_new_string(c.status.c_str()));

        // Omitted entirely when empty. See the header comment: a missing key
        // is the wire's way of saying nullopt, and 0 is not.
        if (c.level) {
            g_variant_builder_add(&inner, "{sv}", "Level",
                                  g_variant_new_byte(*c.level));
        }

        g_variant_builder_add(&inner, "{sv}", "RawLevel",
                              g_variant_new_byte(c.raw_level));

        if (c.framing_anomaly) {
            g_variant_builder_add(&inner, "{sv}", "FramingAnomaly",
                                  g_variant_new_boolean(TRUE));
        }

        g_variant_builder_add(&outer, "{sa{sv}}", key.c_str(), &inner);
    }

    return g_variant_builder_end(&outer);
}

GVariant* DbusService::get_property(const char* name) const
{
    const std::string p = name;

    if (p == "Address")       return g_variant_new_string(state_.address.c_str());
    if (p == "Phase")         return g_variant_new_string(state_.phase.c_str());
    if (p == "ChannelOpen")   return g_variant_new_boolean(state_.channel_open);
    if (p == "Status")        return g_variant_new_string(state_.status.c_str());
    if (p == "HaveBattery")   return g_variant_new_boolean(state_.have_battery);
    if (p == "ParseRefusals") return g_variant_new_int32(state_.parse_refusals);
    if (p == "Components")    return components_variant();

    // Live, never cached, never signalled.
    if (p == "BatteryAgeMs")
        return g_variant_new_int64(age_ ? age_() : -1);

    return nullptr;
}

// --- publishing ------------------------------------------------------------

void DbusService::publish(const State& s)
{
    if (published_ && s == state_) return;   // the common case by a mile

    const State prev = state_;
    state_ = s;

    if (!published_) {
        // First snapshot. Nothing to compare against, and anyone who connects
        // later does a Get/GetAll rather than waiting for a signal, so there
        // is nothing to announce.
        published_ = true;
        return;
    }
    if (!conn_ || reg_id_ == 0) return;   // name not acquired yet

    GVariantBuilder changed;
    g_variant_builder_init(&changed, G_VARIANT_TYPE("a{sv}"));
    bool any = false;

    auto add = [&](const char* key) {
        g_variant_builder_add(&changed, "{sv}", key, get_property(key));
        any = true;
    };

    if (prev.address       != state_.address)       add("Address");
    if (prev.phase         != state_.phase)         add("Phase");
    if (prev.channel_open  != state_.channel_open)  add("ChannelOpen");
    if (prev.status        != state_.status)        add("Status");
    if (prev.have_battery  != state_.have_battery)  add("HaveBattery");
    if (prev.parse_refusals != state_.parse_refusals) add("ParseRefusals");
    if (prev.components    != state_.components)    add("Components");

    if (!any) {
        g_variant_builder_clear(&changed);
        return;
    }

    // The third member is the INVALIDATED list -- properties whose value
    // changed but is not being sent. We always send values, so it is empty;
    // g_variant_new still wants a builder for it rather than a null.
    GVariantBuilder invalidated;
    g_variant_builder_init(&invalidated, G_VARIANT_TYPE("as"));

    GVariant* args = g_variant_new("(sa{sv}as)",
                                   kInterface, &changed, &invalidated);

    GError* err = nullptr;
    g_dbus_connection_emit_signal(conn_, nullptr, kObjectPath,
                                  "org.freedesktop.DBus.Properties",
                                  "PropertiesChanged", args, &err);
    if (err) {
        spdlog::warn("dbus: PropertiesChanged failed: {}", err->message);
        g_error_free(err);
    }
}

} // namespace airprobe::daemon
