#!/usr/bin/env gjs -m
// SPDX-License-Identifier: GPL-2.0-or-later
//
// format-check.js -- runs format.js against synthetic states and prints what
// the tile would say. NOT shipped inside the extension directory; it is a
// development tool, and the extension folder should contain only what GNOME
// Shell loads.
//
//   gjs -m shell-extension/tests/format-check.js
//
// WHY IT EXISTS. Airprobe has no trace channel in the container -- no
// Bluetooth, no pods, and now no GNOME Shell either. This is the first thing
// in the project that can actually be RUN where it is written, and it only
// works because format.js imports nothing. Exit code is nonzero if any
// expectation fails, so it is a check and not just a demo.

import {subtitle, headerSubtitle, formatAge, hasWarning, warningText,
        componentsFromDbus}
    from '../airprobe@scott8420.github.io/format.js';

let failures = 0;

function expect(label, got, want) {
    const ok = got === want;
    if (!ok)
        failures++;
    print(`${ok ? 'ok  ' : 'FAIL'}  ${label.padEnd(34)} ${JSON.stringify(got)}`);
    if (!ok)
        print(`      wanted: ${JSON.stringify(want)}`);
}

function view(over = {}) {
    return Object.assign({
        daemonPresent: true,
        phase: 'listening',
        channelOpen: true,
        status: 'listening',
        haveBattery: true,
        parseRefusals: 0,
        ageMs: 1200,
        components: {
            left:  {status: 'discharging', level: 86, rawLevel: 86},
            right: {status: 'discharging', level: 83, rawLevel: 83},
            case:  {status: 'disconnected', rawLevel: 0},
        },
    }, over);
}

print('--- subtitle ---');

expect('live, both pods',
    subtitle(view()),
    'L 86% \u00b7 R 83%');

expect('worst case width',
    subtitle(view({components: {
        left:  {status: 'charging', level: 100, rawLevel: 100},
        right: {status: 'charging', level: 100, rawLevel: 100},
    }})),
    'L 100% \u00b7 R 100%');

// One pod reporting. The absent side is a dash, never a silently dropped
// half and never a zero.
expect('right absent from packet',
    subtitle(view({components: {
        left: {status: 'discharging', level: 86, rawLevel: 86},
    }})),
    'L 86% \u00b7 R \u2014');

expect('left present but unreadable',
    subtitle(view({components: {
        left:  {status: 'disconnected', rawLevel: 0},
        right: {status: 'discharging', level: 83, rawLevel: 83},
    }})),
    'L \u2014 \u00b7 R 83%');

// Pods went back to the phone. The caveat leads, so a narrow tile truncates
// the numbers and keeps the doubt.
expect('stale reading',
    subtitle(view({channelOpen: false, phase: 'closed', ageMs: 252000})),
    '4 min ago \u00b7 L 86% \u00b7 R 83%');

expect('stale, seconds old',
    subtitle(view({channelOpen: false, ageMs: 8000})),
    'just now \u00b7 L 86% \u00b7 R 83%');

expect('stale, hours old',
    subtitle(view({channelOpen: false, ageMs: 3 * 3600 * 1000})),
    '3 hr ago \u00b7 L 86% \u00b7 R 83%');

expect('connected, nothing pushed yet',
    subtitle(view({haveBattery: false, ageMs: -1, components: {}})),
    'Waiting for a reading');

expect('mid-handshake',
    subtitle(view({haveBattery: false, phase: 'wait-features-ack',
                   ageMs: -1, components: {}})),
    'Connecting\u2026');

expect('pods elsewhere, never read',
    subtitle(view({channelOpen: false, phase: 'closed', haveBattery: false,
                   ageMs: -1, components: {}})),
    'Not connected');

// Case reporting but both pods absent: the tile is about the pods, so this
// is "no pod reading", not a case percentage wearing a pod's label.
expect('case only',
    subtitle(view({components: {case: {status: 'charging', level: 90, rawLevel: 90}}})),
    'No pod reading');

expect('daemon gone',
    subtitle(view({daemonPresent: false})),
    'airprobed not running');

print('\n--- header subtitle ---');

expect('header, live',
    headerSubtitle(view({status: 'listening on AAP'})),
    'listening on AAP');

expect('header, stale',
    headerSubtitle(view({channelOpen: false, ageMs: 252000,
                         status: 'channel closed by peer'})),
    'Last reading 4 min ago \u00b7 channel closed by peer');

expect('header, connect failure keeps the diagnosis',
    headerSubtitle(view({channelOpen: false, haveBattery: false, ageMs: -1,
                         status: 'connect failed: ECONNREFUSED -- pods are on another host'})),
    'connect failed: ECONNREFUSED -- pods are on another host');

print('\n--- age ---');

expect('no reading at all', formatAge(-1), 'age unknown');
expect('undefined age', formatAge(undefined), 'age unknown');
expect('under a minute', formatAge(30000), 'just now');
expect('one minute', formatAge(61000), '1 min ago');
expect('one hour', formatAge(3600 * 1000), '1 hr ago');
expect('one day', formatAge(26 * 3600 * 1000), '1 day ago');

print('\n--- warnings ---');

expect('clean', hasWarning(view()), false);
expect('refusals', hasWarning(view({parseRefusals: 2})), true);
expect('refusal wording',
    warningText(view({parseRefusals: 2})),
    '2 battery packets were refused by the parser');
expect('framing anomaly',
    hasWarning(view({components: {
        left: {status: 'discharging', level: 86, rawLevel: 86, framingAnomaly: true},
    }})),
    true);
expect('framing wording',
    warningText(view({parseRefusals: 1, components: {
        right: {status: 'discharging', level: 83, rawLevel: 83, framingAnomaly: true},
    }})),
    '1 battery packet was refused by the parser; unexpected framing on: right');
expect('no daemon is not a warning',
    hasWarning(view({daemonPresent: false, parseRefusals: 3})), false);

print('\n--- D-Bus mapping ---');

// What recursiveUnpack() hands us for a packet with one readable pod, one
// unreadable case, and nothing at all from the right.
const mapped = componentsFromDbus({
    left: {Status: 'discharging', Level: 86, RawLevel: 86},
    case: {Status: 'disconnected', RawLevel: 0},
});
expect('left level survives', mapped.left.level, 86);
expect('right stays absent', mapped.right, undefined);
expect('unreadable case has NO level key',
    Object.hasOwn(mapped.case, 'level'), false);
expect('raw byte kept anyway', mapped.case.rawLevel, 0);
expect('and it renders as a dash, not 0%',
    subtitle(view({components: mapped})), 'L 86% \u00b7 R \u2014');
expect('framing flag defaults false', mapped.left.framingAnomaly, false);
expect('framing flag read when set',
    componentsFromDbus({left: {Status: 'x', RawLevel: 1, FramingAnomaly: true}})
        .left.framingAnomaly, true);

print('');
if (failures) {
    print(`${failures} FAILED`);
    imports.system.exit(1);
}
print('all checks passed');
