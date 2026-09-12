// SPDX-License-Identifier: GPL-2.0-or-later
//
// extension.js -- Airprobe's Quick Settings tile.
//
// This file is PLUMBING ONLY. Every decision about what words appear lives in
// format.js, which imports nothing and therefore runs under plain gjs; see
// shell-extension/tests/format-check.js. What is left here is D-Bus wiring
// and widget lifetime, both of which are reviewed by eye because there is no
// GNOME Shell in the container to run them against.
//
// --- THE ONE HARD RULE: NOTHING BLOCKS ------------------------------------
//
// This code runs INSIDE the compositor process. A synchronous D-Bus call here
// freezes the desktop -- not the extension, the desktop. So: no call_sync, no
// new_sync, no L2CAP, no Bluetooth of any kind. Every reading in this file
// arrives asynchronously from airprobed, which does all the waiting in its
// own process where waiting is free.
//
// --- WHY A NAME WATCH AND NOT JUST A PROXY --------------------------------
//
// A Gio.DBusProxy will happily exist for a name nobody owns and report every
// property as null. That is indistinguishable, at the render site, from a
// daemon that is running and has nothing to say -- and those two states want
// completely different words. So the name watch is the source of truth for
// "is airprobed there", and the proxy only exists while it is.
//
// DO_NOT_AUTO_START, because a tile appearing in the panel must never start a
// daemon that opens a Bluetooth channel. The user starts airprobed; we watch.
//
// --- BatteryAgeMs IS NOT CACHED -------------------------------------------
//
// The daemon annotates it EmitsChangedSignal=false on purpose: it changes
// every millisecond without anything happening, so signalling it would fire
// PropertiesChanged forever and train consumers to ignore the signal. The
// consequence here is that get_cached_property('BatteryAgeMs') returns
// whatever it was when the proxy was built and never moves again. It must be
// fetched live, with an explicit Properties.Get, every time we are about to
// show it. Caching it would produce a number that looks like a measurement of
// staleness and is itself stale -- exactly the failure the field was added to
// prevent.

import GObject from 'gi://GObject';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';
import * as QuickSettings from 'resource:///org/gnome/shell/ui/quickSettings.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

import {subtitle, headerSubtitle, hasWarning, warningText, componentsFromDbus}
    from './format.js';

const BUS_NAME   = 'io.github.scott8420.Airprobe';
const OBJECT_PATH = '/io/github/scott8420/Airprobe';
const INTERFACE  = 'io.github.scott8420.Airprobe1';

// How often to re-render while the reading is STALE. Only while stale: a live
// reading re-renders when PropertiesChanged says so, and a dead one never
// changes. This timer exists solely so "4 min ago" does not sit there saying
// four minutes an hour later. Half a minute is finer than the coarsest thing
// the label can say.
const STALE_REFRESH_SECONDS = 30;

// Device icon, not a charge-level glyph. The percentages are already in the
// subtitle, and a battery glyph next to "L 50% - R 48%" would be ambiguous
// about which pod it meant. Not an Apple silhouette either -- that is their
// trademark, and a named theme icon follows dark mode and the accent colour
// for free.
const ICON_CANDIDATES = [
    'audio-headphones-symbolic',
    'audio-headset-symbolic',
    'audio-speakers-symbolic',
    'audio-card-symbolic',
];

function pickIcon() {
    try {
        const theme = new St.IconTheme();
        for (const name of ICON_CANDIDATES) {
            if (theme.has_icon(name))
                return name;
        }
    } catch (e) {
        logError(e, 'airprobe: icon theme lookup failed');
    }
    // Every theme has this one. A tile with a generic glyph beats a tile with
    // a missing-image box.
    return 'image-missing';
}

const AirprobeToggle = GObject.registerClass(
class AirprobeToggle extends QuickSettings.QuickMenuToggle {
    constructor(iconName) {
        super({
            title: 'AirPods',
            subtitle: 'Starting\u2026',
            iconName,
            // NOT a toggle. There is nothing here for a click to switch:
            // battery on the A3056 is push-only and this interface has no
            // methods at all. toggleMode:true would give the user a control
            // that silently does nothing, which is worse than no control.
            toggleMode: false,
        });

        // With toggleMode off, a click on the body has nowhere to go, so send
        // it to the menu -- the whole tile becomes one target instead of just
        // the arrow.
        this.connect('clicked', () => this.menu.open());

        // Kept because setHeader() needs it on every update and reading it
        // back off the widget invites gicon/iconName ambiguity.
        this._iconName = iconName;
        this.menu.setHeader(iconName, 'AirPods', '');

        this._warningIcon = new St.Icon({
            iconName: 'dialog-warning-symbolic',
            visible: false,
        });
        this.menu.addHeaderSuffix(this._warningIcon);

        this._detail = new PopupMenu.PopupMenuItem('', {
            reactive: false,
            can_focus: false,
        });
        this._detail.label.clutter_text.line_wrap = true;
        this.menu.addMenuItem(this._detail);
    }

    // `view` is the plain object format.js documents.
    render(view) {
        this.visible = view.daemonPresent;
        this.subtitle = subtitle(view);
        this.menu.setHeader(this._iconName, 'AirPods', headerSubtitle(view));

        // Accent ON means "these numbers are live". Off means stale, absent,
        // or not connected. It is a statement about the reading, not a switch
        // the user owns -- which is why nothing here reacts to a click.
        this.checked = view.channelOpen && view.haveBattery;

        const warn = hasWarning(view);
        this._warningIcon.visible = warn;
        this._detail.label.text = warn
            ? warningText(view)
            : `Phase: ${view.phase}`;
    }
});

const AirprobeIndicator = GObject.registerClass(
class AirprobeIndicator extends QuickSettings.SystemIndicator {
    constructor() {
        super();

        // No panel icon at this milestone. The top-bar glyph is a separate,
        // optional piece of work and a permanent icon up there is a cost the
        // user did not ask for yet.
        this._toggle = new AirprobeToggle(pickIcon());
        this.quickSettingsItems.push(this._toggle);

        this._proxy = null;
        this._propsId = 0;
        this._staleTimer = 0;
        this._cancellable = new Gio.Cancellable();
        this._ageMs = -1;

        this._watchId = Gio.bus_watch_name(
            Gio.BusType.SESSION, BUS_NAME,
            Gio.BusNameWatcherFlags.NONE,
            () => this._onAppeared(),
            () => this._onVanished());

        // Until the watch reports, assume nothing.
        this._render(false);
    }

    _onAppeared() {
        if (this._proxy)
            return;
        Gio.DBusProxy.new(
            Gio.DBus.session,
            Gio.DBusProxyFlags.DO_NOT_AUTO_START,
            null, BUS_NAME, OBJECT_PATH, INTERFACE,
            this._cancellable,
            (obj, res) => {
                let proxy;
                try {
                    proxy = Gio.DBusProxy.new_finish(res);
                } catch (e) {
                    if (!e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                        logError(e, 'airprobe: could not proxy airprobed');
                    return;
                }
                // disable() may have run while that was in flight.
                if (this._cancellable.is_cancelled())
                    return;
                this._proxy = proxy;
                this._propsId = proxy.connect('g-properties-changed',
                                              () => this._refresh());
                this._refresh();
            });
    }

    _onVanished() {
        this._dropProxy();
        this._ageMs = -1;
        this._render(false);
    }

    _dropProxy() {
        if (this._propsId && this._proxy) {
            this._proxy.disconnect(this._propsId);
            this._propsId = 0;
        }
        this._proxy = null;
    }

    // Fetch the age, then draw. Always in that order, because the age is the
    // only field that is wrong the moment it is cached.
    _refresh() {
        if (!this._proxy) {
            this._render(false);
            return;
        }
        this._proxy.g_connection.call(
            BUS_NAME, OBJECT_PATH, 'org.freedesktop.DBus.Properties', 'Get',
            new GLib.Variant('(ss)', [INTERFACE, 'BatteryAgeMs']),
            new GLib.VariantType('(v)'),
            Gio.DBusCallFlags.NONE, -1, this._cancellable,
            (conn, res) => {
                try {
                    const reply = conn.call_finish(res);
                    this._ageMs = reply.recursiveUnpack()[0];
                } catch (e) {
                    if (e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                        return;
                    // Not fatal and not silent: an unknown age renders as
                    // "age unknown", which is honest, and the log says why.
                    logError(e, 'airprobe: BatteryAgeMs read failed');
                    this._ageMs = -1;
                }
                this._render(true);
            });
    }

    _cached(name, fallback) {
        const v = this._proxy?.get_cached_property(name);
        return v === null || v === undefined ? fallback : v.recursiveUnpack();
    }

    _render(daemonPresent) {
        const view = {
            daemonPresent,
            phase: this._cached('Phase', 'closed'),
            channelOpen: this._cached('ChannelOpen', false),
            status: this._cached('Status', ''),
            haveBattery: this._cached('HaveBattery', false),
            parseRefusals: this._cached('ParseRefusals', 0),
            ageMs: this._ageMs,
            components: componentsFromDbus(this._cached('Components', {})),
        };

        this._toggle.render(view);

        // Re-tick only while there is an ageing number on screen.
        const needsTimer = daemonPresent && view.haveBattery && !view.channelOpen;
        if (needsTimer && !this._staleTimer) {
            this._staleTimer = GLib.timeout_add_seconds(
                GLib.PRIORITY_DEFAULT, STALE_REFRESH_SECONDS, () => {
                    this._refresh();
                    return GLib.SOURCE_CONTINUE;
                });
        } else if (!needsTimer && this._staleTimer) {
            GLib.Source.remove(this._staleTimer);
            this._staleTimer = 0;
        }
    }

    destroy() {
        this._cancellable.cancel();
        if (this._staleTimer) {
            GLib.Source.remove(this._staleTimer);
            this._staleTimer = 0;
        }
        if (this._watchId) {
            Gio.bus_unwatch_name(this._watchId);
            this._watchId = 0;
        }
        this._dropProxy();
        this.quickSettingsItems.forEach(item => item.destroy());
        super.destroy();
    }
});

export default class AirprobeExtension extends Extension {
    enable() {
        this._indicator = new AirprobeIndicator();
        Main.panel.statusArea.quickSettings.addExternalIndicator(this._indicator);
    }

    disable() {
        // Everything this extension holds -- a name watch, a proxy, a timer,
        // one in-flight async call -- is released in the indicator's destroy.
        // A leaked timeout here is the classic way an extension survives its
        // own disable and keeps poking the bus.
        this._indicator?.destroy();
        this._indicator = null;
    }
}
