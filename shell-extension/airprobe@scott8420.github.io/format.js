// SPDX-License-Identifier: GPL-2.0-or-later
//
// format.js -- every decision about what words go on the tile, and NOTHING
// else. No `gi://` imports, no `resource:///` imports, no side effects.
//
// WHY THIS FILE EXISTS SEPARATELY, and it is the same argument that put
// libairprobe behind a seam at s3: the container has no GNOME Shell, so any
// logic living in extension.js can only be read, never run. Pulled out here,
// the interesting half runs under plain `gjs` against synthetic states --
// which is the only trace channel this session has. Everything in
// extension.js is then plumbing thin enough to review by eye.
//
// The input is a plain object, deliberately not a D-Bus proxy:
//
//   {
//     daemonPresent : bool        // does anyone own the well-known name
//     phase         : string      // closed / wait-handshake-ack /
//                                 // wait-features-ack / listening
//     channelOpen   : bool        // is the AAP channel up RIGHT NOW
//     status        : string      // the daemon's own words, for the menu
//     haveBattery   : bool
//     parseRefusals : int
//     ageMs         : int64       // -1 when there is no cached reading
//     components    : { left?: {status, level?, rawLevel, framingAnomaly?},
//                       right?: ..., case?: ... }
//   }
//
// A MISSING KEY AND A MISSING LEVEL ARE DIFFERENT THINGS and that distinction
// is carried all the way from aap_battery.h through State to the bus. Here it
// finally reaches a human: on the tile both render as an em dash, because the
// tile has no room to explain; in the menu they get different words. That is
// the right place to collapse it -- visibly, once, at the last moment.

const DASH = '\u2014';        // em dash: "no reading", never 0%
const SEP  = '\u00b7';        // middle dot

// ---------------------------------------------------------------------------
// Age
//
// Rendered coarsely on purpose. "4 min ago" invites no more trust than it has
// earned; "4:12" would imply a precision the pods do not offer, since they
// push on their own schedule roughly every nine seconds and only when
// something changed.
export function formatAge(ms) {
    if (ms === null || ms === undefined || ms < 0)
        return 'age unknown';

    const s = Math.floor(ms / 1000);
    if (s < 45)
        return 'just now';

    const m = Math.round(s / 60);
    if (m < 60)
        return `${m} min ago`;

    const h = Math.round(m / 60);
    if (h < 24)
        return h === 1 ? '1 hr ago' : `${h} hr ago`;

    const d = Math.round(h / 24);
    return d === 1 ? '1 day ago' : `${d} days ago`;
}

// One component as a short string. Absent, present-but-unreadable and
// disconnected all come out as the dash.
export function levelText(component) {
    if (!component)
        return DASH;
    if (component.level === null || component.level === undefined)
        return DASH;
    return `${component.level}%`;
}

// Does the snapshot contain anything worth putting a number next to.
function anyPodReading(components) {
    const l = components?.left;
    const r = components?.right;
    return (l && l.level !== undefined && l.level !== null) ||
           (r && r.level !== undefined && r.level !== null);
}

// ---------------------------------------------------------------------------
// The subtitle -- the one line under "AirPods" on the tile.
//
// THE TILE IS HALF THE GRID WIDE AND THE SUBTITLE ELLIPSIZES RATHER THAN
// WRAPPING. That constraint decides the word order, and the rule is worth
// stating because it is not obvious:
//
//   WHEN A FIELD CAN BE TRUNCATED, PUT THE DOUBT AT THE FRONT.
//
// A stale reading rendered "L 86% - R 83% -- 4 min ago" clips to
// "L 86% - R 83% ..." on a narrow tile, and what survives is exactly the part
// that lies. Rendered "4 min ago - L 86% - R 83%" it clips to "4 min ago ..."
// and what survives is the caveat. Truncation must eat the number, never the
// qualifier. Same instinct as THE FIELD THAT LIES, one layer further out
// again: where the format cannot carry the whole truth, keep the half that
// stops a reader being wrong.
//
// When the channel IS open there is no qualifier, because there is nothing to
// doubt -- the reading is live and the numbers get the whole line.
export function subtitle(view) {
    if (!view.daemonPresent)
        return 'airprobed not running';

    if (view.haveBattery && anyPodReading(view.components)) {
        const pods = `L ${levelText(view.components.left)} ${SEP} ` +
                     `R ${levelText(view.components.right)}`;
        if (view.channelOpen)
            return pods;
        return `${formatAge(view.ageMs)} ${SEP} ${pods}`;
    }

    // No usable pod number. Say WHY, since "nothing" is the state a person is
    // most likely to read as a broken extension.
    if (view.channelOpen) {
        if (view.phase === 'listening')
            return view.haveBattery ? 'No pod reading' : 'Waiting for a reading';
        return 'Connecting\u2026';
    }

    if (view.haveBattery)
        return `${formatAge(view.ageMs)} ${SEP} no pod reading`;

    return 'Not connected';
}

// The menu header's second line. Roomier than the tile, so it can afford the
// daemon's own words -- which carry the connect diagnosis on failure, and
// "it didn't connect" is not actionable while "ENODEV at stage 2" is.
export function headerSubtitle(view) {
    if (!view.daemonPresent)
        return 'The Airprobe daemon is not on the session bus';
    if (view.channelOpen)
        return view.status || 'Connected';
    if (view.haveBattery)
        return `Last reading ${formatAge(view.ageMs)} ${SEP} ${view.status || 'not connected'}`;
    return view.status || 'Not connected';
}

// True when the tile should fly a warning. parse_refusals is the daemon
// saying it saw a 0x0004 packet whose shape it refused: the cached number may
// predate whatever changed, so a consumer showing it should say so rather
// than serve it quietly.
export function hasWarning(view) {
    if (!view.daemonPresent)
        return false;
    if (view.parseRefusals > 0)
        return true;
    for (const key of ['left', 'right', 'case']) {
        if (view.components?.[key]?.framingAnomaly)
            return true;
    }
    return false;
}

export function warningText(view) {
    const bits = [];
    if (view.parseRefusals > 0) {
        bits.push(view.parseRefusals === 1
            ? '1 battery packet was refused by the parser'
            : `${view.parseRefusals} battery packets were refused by the parser`);
    }
    const odd = ['left', 'right', 'case']
        .filter(k => view.components?.[k]?.framingAnomaly);
    if (odd.length)
        bits.push(`unexpected framing on: ${odd.join(', ')}`);
    return bits.join('; ');
}

export const Glyphs = {DASH, SEP};

// ---------------------------------------------------------------------------
// The Components property arrives as a{sa{sv}} and comes out of
// recursiveUnpack() with the daemon's capitalised key names. Translate once,
// here, rather than teaching every render site two vocabularies.
//
// The one rule that matters: WHEN THERE IS NO "Level" KEY, DO NOT INVENT ONE.
// Not 0, not null-with-a-flag -- absent. The whole chain from aap_battery.h
// down has kept "present but unreadable" distinct from "0%", and a `?? 0` on
// this line would throw it away at the last possible moment.
export function componentsFromDbus(raw) {
    const out = {};
    if (!raw)
        return out;
    for (const [key, dict] of Object.entries(raw)) {
        const c = {
            status: dict.Status ?? 'unknown',
            rawLevel: dict.RawLevel ?? 0,
            framingAnomaly: dict.FramingAnomaly === true,
        };
        if (dict.Level !== undefined && dict.Level !== null)
            c.level = dict.Level;
        out[key] = c;
    }
    return out;
}
