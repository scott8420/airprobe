#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Scott Combs
# Airprobe -- install / uninstall. One script for both halves of the thing:
# the airprobed user service and the GNOME Shell tile.
#
#   ./install.sh install              both, in the order that works
#   ./install.sh uninstall            both
#   ./install.sh status               what is installed, enabled, running
#
#   ./install.sh daemon-install [MAC] just the service
#   ./install.sh daemon-uninstall
#   ./install.sh extension-install    just the tile
#   ./install.sh extension-uninstall
#   ./install.sh diff                 installed extension vs this tree
#   ./install.sh adopt-version        set metadata.json to the running shell
#
# WHY THIS IS NOT IN build.sh. build.sh configures and compiles; it touches
# nothing outside build/. This script writes into the live session -- it can
# change what the panel is running and what starts at login. Those are
# different verbs and a person typing the first should never get the second.
# build.sh points here and stops.
#
# Nothing here needs root. The extension lives under $HOME, airprobed is a
# per-user service, and the AAP channel is an ordinary socket to a paired
# device. If a command in this file ever wants sudo, something is wrong with
# the command and not with your permissions.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

UUID="airprobe@scott8420.github.io"
SRC="$ROOT/shell-extension/$UUID"
DEST="${XDG_DATA_HOME:-$HOME/.local/share}/gnome-shell/extensions/$UUID"

# The daemon side. All four destinations are fixed by systemd and D-Bus
# convention, not chosen by us -- which is most of the argument for putting
# them in a script instead of a README nobody re-reads.
BINARY_SRC="$ROOT/build/daemon/airprobed"
BINARY_DEST="$HOME/.local/bin/airprobed"
UNIT_SRC="$ROOT/packaging/airprobed.service"
UNIT_DEST="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/airprobed.service"
DBUS_SRC="$ROOT/packaging/io.github.scott8420.Airprobe.service"
DBUS_DEST="${XDG_DATA_HOME:-$HOME/.local/share}/dbus-1/services/io.github.scott8420.Airprobe.service"
CONF="${XDG_CONFIG_HOME:-$HOME/.config}/airprobe/airprobed.conf"

# The files GNOME Shell actually loads. Listed rather than globbed so a
# half-copied tree fails here with a name instead of failing in the compositor
# with a stack trace.
REQUIRED=(metadata.json extension.js format.js)

red()  { printf '\033[31m%s\033[0m\n' "$*"; }
bold() { printf '\033[1m%s\033[0m\n' "$*"; }
note() { printf '  %s\n' "$*"; }

die() { red "$*"; exit 1; }

have() { command -v "$1" >/dev/null 2>&1; }

# --- version ---------------------------------------------------------------
#
# Version checking in GNOME Shell is STRICT and silent: a metadata.json that
# does not name the running shell makes the extension refuse to load, with no
# error anywhere the user is looking. That is a bad failure to debug and a
# trivial one to prevent, so it is checked before anything is copied.

running_shell_version() {
    have gnome-shell || return 1
    gnome-shell --version 2>/dev/null | grep -oE '[0-9]+' | head -1
}

metadata_versions() {
    grep -o '"shell-version"[^]]*]' "$SRC/metadata.json" \
        | grep -oE '[0-9]+' | tr '\n' ' '
}

check_version() {
    local running declared
    running="$(running_shell_version || true)"
    declared="$(metadata_versions)"

    if [[ -z "$running" ]]; then
        note "gnome-shell not found -- cannot check the version. Declared: $declared"
        return 0
    fi

    if [[ " $declared " == *" $running "* ]]; then
        note "GNOME Shell $running, metadata declares: $declared -- match"
        return 0
    fi

    red "GNOME Shell $running is running; metadata.json declares: $declared"
    note "The extension would install and then silently refuse to load."
    note "Fix it in the tree with:"
    note "    ./shell-extension/install.sh adopt-version"
    return 1
}

# Rewrite the SOURCE metadata.json, not the installed copy. Patching only what
# was installed would leave the tree and the running extension disagreeing,
# and the next install would put the old value back -- a bug that looks like
# the shell forgetting.
adopt_version() {
    local running
    running="$(running_shell_version)" || die "gnome-shell not found."
    sed -i -E "s/\"shell-version\":[^]]*\]/\"shell-version\": [\"$running\"]/" \
        "$SRC/metadata.json"
    bold "metadata.json now declares shell-version [\"$running\"]"
    note "That edits the source tree, not just the installed copy."
    grep '"shell-version"' "$SRC/metadata.json"
}

# --- the master switch -----------------------------------------------------
#
# org.gnome.shell disable-user-extensions turns off EVERY user extension at a
# level above any individual one. With it set, `gnome-extensions enable`
# succeeds, writes the UUID into enabled-extensions, prints nothing, and the
# shell ignores all of it. The extension then sits at State: INITIALIZED --
# metadata loaded, never started -- with no error in the journal, because
# nothing was ever loaded to throw one.
#
# That is a perfectly healthy-looking dead end and it cost us a round trip.
# A status command exists precisely to answer "the thing is switched off one
# level above the thing you are looking at", so it is checked first and it is
# checked loudly.
#
# It gets set by the master toggle in the Extensions app, and GNOME also sets
# it itself after certain shell crashes -- so it can become true without
# anybody deciding it should.

master_switch_off() {
    have gsettings || return 1
    [[ "$(gsettings get org.gnome.shell disable-user-extensions 2>/dev/null)" == "true" ]]
}

check_master_switch() {
    if master_switch_off; then
        red "ALL user extensions are disabled (org.gnome.shell disable-user-extensions = true)"
        note "Nothing below matters until this is off. Individual enables are ignored."
        note "    gsettings set org.gnome.shell disable-user-extensions false"
        return 1
    fi
    return 0
}

# --- install ---------------------------------------------------------------

do_extension_install() {
    [[ -d "$SRC" ]] || die "not found: $SRC"
    for f in "${REQUIRED[@]}"; do
        [[ -f "$SRC/$f" ]] || die "missing from the tree: $f"
    done

    check_version || exit 1

    # Remove before copying rather than copying over the top. A file deleted
    # from the tree but left behind in the install is a stale module the shell
    # will happily keep importing, and that is a genuinely confusing hour.
    rm -rf "$DEST"
    mkdir -p "$(dirname "$DEST")"
    cp -r "$SRC" "$DEST"
    bold "installed -> $DEST"

    if ! have gnome-extensions; then
        note "gnome-extensions not on PATH; enable it from the Extensions app."
    # This is expected to fail on a first install: the shell builds its list
    # of extensions at startup and does not know this UUID yet. That is not an
    # error worth printing in red, so it is caught and explained.
    elif gnome-extensions enable "$UUID" 2>/dev/null; then
        bold "enabled"
    else
        note "Not enabled yet -- the running shell has not seen it."
        note "Log out and back in, then:  gnome-extensions enable $UUID"
    fi

    echo
    check_master_switch || true
    restart_advice
}

restart_advice() {
    if [[ "${XDG_SESSION_TYPE:-}" == "x11" ]]; then
        bold "Reload the shell with Alt+F2, then r, then Enter."
    else
        bold "Log out and back in for the shell to pick this up."
        note "GNOME 50 is Wayland-only, so there is no Alt+F2 r."
    fi
}

# --- uninstall -------------------------------------------------------------

do_extension_uninstall() {
    if have gnome-extensions; then
        gnome-extensions disable "$UUID" 2>/dev/null \
            && bold "disabled" \
            || note "was not enabled"
    fi

    if [[ -d "$DEST" ]]; then
        rm -rf "$DEST"
        bold "removed  $DEST"
    else
        note "nothing installed at $DEST"
    fi

    # The extension has no GSettings schema and writes no state anywhere, so
    # removing the directory removes all of it. Said out loud because "did that
    # leave something behind" is the question every uninstaller raises.
    note "No settings, no schema, no cache -- that is the whole of it."
    echo
    restart_advice
}

# --- daemon ----------------------------------------------------------------
#
# Four copies into four fixed paths plus one generated file. Every one of
# those paths is dictated by systemd or D-Bus rather than chosen by us, which
# is most of the argument for a script: there is nothing here to think about
# and four things to get subtly wrong by hand.
#
# THE MAC IS THE ONLY THING THIS NEEDS FROM YOU. A bus-activated service has
# no terminal to type argv into, so the address lives in an EnvironmentFile.
# Passed once, remembered after -- a second daemon-install with no argument
# reuses what is already on disk rather than making you find the address
# again.

current_mac() {
    [[ -f "$CONF" ]] || return 1
    sed -n 's/^AIRPROBE_MAC=//p' "$CONF" | head -1
}

do_daemon_install() {
    local mac="${1:-}"

    if [[ -z "$mac" ]]; then
        mac="$(current_mac || true)"
    fi
    if [[ -z "$mac" ]]; then
        red "no MAC address."
        note "Pass it once and it is remembered:"
        note "    ./install.sh daemon-install AA:BB:CC:DD:EE:FF"
        note "Find it with:  bluetoothctl devices"
        exit 1
    fi

    # ExecStart points at ~/.local/bin, so a tree that has not been built has
    # nothing to install and should say so here rather than at first start.
    [[ -x "$BINARY_SRC" ]] || die "not built: $BINARY_SRC  (run ./build.sh)"
    [[ -f "$UNIT_SRC" ]]   || die "missing: $UNIT_SRC"
    [[ -f "$DBUS_SRC" ]]   || die "missing: $DBUS_SRC"

    mkdir -p "$(dirname "$BINARY_DEST")" "$(dirname "$UNIT_DEST")" \
             "$(dirname "$DBUS_DEST")"   "$(dirname "$CONF")"

    install -m 755 "$BINARY_SRC" "$BINARY_DEST"
    install -m 644 "$UNIT_SRC"   "$UNIT_DEST"
    install -m 644 "$DBUS_SRC"   "$DBUS_DEST"
    printf 'AIRPROBE_MAC=%s\n' "$mac" > "$CONF"

    bold "installed"
    note "$BINARY_DEST"
    note "$UNIT_DEST"
    note "$DBUS_DEST"
    note "$CONF   ($mac)"

    have systemctl || { note "systemctl not found; nothing enabled."; return 0; }

    systemctl --user daemon-reload
    # enable AND start. D-Bus activation alone would never fire for the tile:
    # the extension watches the name with DO_NOT_AUTO_START and deliberately
    # does not activate anything, because a tile appearing in a panel must not
    # open a Bluetooth channel on its own. So `enable` is what makes the
    # reading present at login; activation is for a CLI that asks by name.
    if systemctl --user enable --now airprobed; then
        sleep 1
        bold "service"
        systemctl --user --no-pager --lines=0 status airprobed \
            | sed -n 's/^ *\(Loaded\|Active\):/  &/p' | sed 's/^ *//;s/^/  /'
        note "logs: journalctl --user -u airprobed -f"
    fi
}

do_daemon_uninstall() {
    if have systemctl; then
        systemctl --user disable --now airprobed 2>/dev/null \
            && bold "stopped and disabled" \
            || note "was not enabled"
    fi
    for f in "$UNIT_DEST" "$DBUS_DEST" "$BINARY_DEST"; do
        if [[ -e "$f" ]]; then
            rm -f "$f"
            note "removed $f"
        fi
    done
    have systemctl && systemctl --user daemon-reload || true

    # The conf survives on purpose: it holds the MAC, it is two lines, and
    # re-finding a Bluetooth address is more annoying than deleting a file.
    if [[ -f "$CONF" ]]; then
        note "kept $CONF (it has the MAC; delete it by hand if you want it gone)"
    fi
}

# --- status ----------------------------------------------------------------

do_status() {
    check_master_switch || echo

    bold "extension"
    if [[ -d "$DEST" ]]; then
        note "installed at $DEST"
    else
        note "not installed"
    fi

    if have gnome-extensions; then
        local info
        info="$(gnome-extensions info "$UUID" 2>/dev/null || true)"
        if [[ -n "$info" ]]; then
            # Error is in this list because a load failure puts its reason
            # there and nowhere else. A status line that filters out the one
            # field carrying the diagnosis is worse than no status line.
            echo "$info" \
                | grep -E '^ *(State|Version|Enabled|Error)' \
                | sed 's/^ *//;s/^/  /'
        else
            note "the shell does not know this UUID (restart pending?)"
        fi
    fi

    # The tile reads airprobed and nothing else, so "is the daemon on the bus"
    # is half the answer to "why is there no tile". Checked with the same name
    # the extension watches, not with pgrep -- a process that has not acquired
    # the name is not yet serving anybody.
    bold "daemon"
    if [[ -x "$BINARY_DEST" ]]; then
        note "binary at $BINARY_DEST"
        # Same lesson as `diff` for the extension: after a rebuild, the thing
        # running is whatever was last COPIED, and "I fixed that" against a
        # stale binary is an expensive hour.
        if [[ -x "$BINARY_SRC" ]] && ! cmp -s "$BINARY_SRC" "$BINARY_DEST"; then
            red "  installed binary differs from build/ -- re-run daemon-install"
        fi
    else
        note "binary NOT installed (run: ./install.sh daemon-install)"
    fi

    if have systemctl; then
        note "unit: $(systemctl --user is-enabled airprobed 2>/dev/null || echo 'not installed')/$(systemctl --user is-active airprobed 2>/dev/null || echo inactive)"
    fi
    [[ -f "$CONF" ]] && note "MAC: $(current_mac)"

    if have gdbus; then
        if gdbus call --session -d org.freedesktop.DBus -o /org/freedesktop/DBus \
             -m org.freedesktop.DBus.NameHasOwner io.github.scott8420.Airprobe \
             2>/dev/null | grep -q true; then
            note "io.github.scott8420.Airprobe is on the session bus"
        else
            note "io.github.scott8420.Airprobe is NOT on the session bus"
            note "the tile hides itself in this state -- start airprobed"
        fi
    else
        note "gdbus not found; cannot check"
    fi

    bold "logs"
    note "journalctl --user -f -o cat /usr/bin/gnome-shell"
}

# --- diff ------------------------------------------------------------------
#
# Answers "am I looking at the code I think I am looking at", which after a
# session of edits is the first question worth asking when behaviour does not
# match the source.

do_diff() {
    [[ -d "$DEST" ]] || die "nothing installed at $DEST"
    if diff -ru "$DEST" "$SRC"; then
        bold "installed copy matches the tree"
    else
        echo
        red "installed copy differs from the tree (left: installed, right: tree)"
        exit 1
    fi
}

case "${1:-}" in
    install)
        # Daemon first: the tile hides itself when nobody owns the bus name,
        # so installing the extension first shows you an empty grid and an
        # open question. This order makes the last thing you do the thing you
        # can see.
        do_daemon_install "${2:-}"
        echo
        do_extension_install
        echo
        do_status
        ;;
    uninstall)
        do_daemon_uninstall
        echo
        do_extension_uninstall
        ;;
    daemon-install)      do_daemon_install "${2:-}" ;;
    daemon-uninstall)    do_daemon_uninstall ;;
    extension-install)   do_extension_install; echo; do_status ;;
    extension-uninstall) do_extension_uninstall ;;
    status)              do_status ;;
    diff)                do_diff ;;
    adopt-version)       adopt_version ;;
    *)
        sed -n '2,14p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'
        exit 2
        ;;
esac
