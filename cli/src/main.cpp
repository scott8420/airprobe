// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Scott Combs
// airprobe -- the probe CLI. libairprobe's first consumer.
//
// s3 SPLIT. This file moved from daemon/src/ to cli/src/ and the three
// aap_* translation units moved to lib/. Nothing here changed behaviourally;
// the point of the move is that the include paths below are now the ONLY way
// this program reaches the pods. When airprobed arrives at s4 it links the
// same archive and shares nothing else with this file -- no argv parsing, no
// .cap transcript, no hexdump to stdout. Those are this program's job, and
// keeping them out of the library is what makes the daemon cheap.
//
// What follows is s2's narrative, kept because it is why the defaults are
// what they are.
//
// s2 milestone 1: make a battery packet arrive.
//
// s1 connected, handshook, and captured 29 packets from the A3056. Opcode
// 0x04 -- battery -- was not among them. This session's job is to find out
// why, and the shape of the answer came from upstream rather than from the
// protocol doc:
//
//   1. Battery is PUSH ONLY. librepods' docs/opcodes.md lists 0x0004 with
//      destination "Host". There is no poll opcode. We cannot ask. We can
//      only arrange to be told.
//
//   2. Our connect sequence differed from the working Linux client in three
//      ways -- all three are now selectable at runtime so ONE hardware
//      session can sweep the matrix instead of needing a rebuild per guess:
//
//        a. SEQUENCING. librepods is ack-driven: handshake, wait for the
//           handshake ack, send host-capabilities, wait for the features
//           ack, then request notifications. s1 slept 200ms and hoped.
//        b. HOST CAPABILITIES. s1 sent this only under --enable-features,
//           and sent 0xFF. librepods sends it on every connect, with 0xD7.
//        c. NOTIFICATION MASK. s1 sent ten bytes with four mask bytes.
//           librepods sends ELEVEN, with five.
//
// STILL NO PARSING. A label is a claim about what a packet IS, never about
// what it SAYS. Milestone 2 parses battery, and only against a real packet
// validated by what the iPhone reports for the same pods at the same moment.

#include "airprobe/aap_socket.h"
#include "airprobe/aap_protocol.h"
#include "airprobe/aap_battery.h"
#include "airprobe/session.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

// --- run configuration --------------------------------------------------

enum class Caps   { D7, FF, None };
enum class Notify { Five, FourFF, FourFE };
enum class Seq    { Ack, Sleep };

const char* caps_name(Caps c)
{
    switch (c) {
    case Caps::D7:   return "d7";
    case Caps::FF:   return "ff";
    case Caps::None: return "none";
    }
    return "?";
}

const char* notify_name(Notify n)
{
    switch (n) {
    case Notify::Five:   return "5ff";
    case Notify::FourFF: return "4ff";
    case Notify::FourFE: return "4fe";
    }
    return "?";
}

std::span<const uint8_t> caps_packet(Caps c)
{
    switch (c) {
    case Caps::D7: return airprobe::proto::kHostCapsD7;
    case Caps::FF: return airprobe::proto::kHostCapsFF;
    case Caps::None: break;
    }
    return {};
}

std::span<const uint8_t> notify_packet(Notify n)
{
    switch (n) {
    case Notify::Five:   return airprobe::proto::kRequestNotify5;
    case Notify::FourFF: return airprobe::proto::kRequestNotify4FF;
    case Notify::FourFE: return airprobe::proto::kRequestNotify4FE;
    }
    return airprobe::proto::kRequestNotify5;
}

// --- capture file -------------------------------------------------------
//
// The container is disposable and Scott's terminal scrollback is not a
// durable artifact. Every run writes a machine-re-readable transcript so a
// later session can re-analyse bytes nobody understood at the time -- which
// is exactly what happened to s1's 0x2B packet.

class Capture {
public:
    bool open(const std::string& path)
    {
        f_ = std::fopen(path.c_str(), "w");
        if (!f_) {
            spdlog::error("could not open capture file '{}': {}", path, std::strerror(errno));
            return false;
        }
        path_ = path;
        return true;
    }

    void header(const std::string& mac, Caps c, Notify n, Seq s)
    {
        if (!f_) return;
        std::time_t now = std::time(nullptr);
        char when[64];
        std::strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
        std::fprintf(f_, "# airprobe capture v1\n");
        std::fprintf(f_, "# started %s\n", when);
        std::fprintf(f_, "# mac %s\n", mac.c_str());
        std::fprintf(f_, "# caps %s notify %s sequence %s\n",
                     caps_name(c), notify_name(n), s == Seq::Ack ? "ack" : "sleep");
        std::fprintf(f_, "# columns: direction elapsed_ms hex label\n");
        std::fflush(f_);
    }

    void packet(char dir, long long ms, std::span<const uint8_t> p, const std::string& label)
    {
        if (!f_) return;
        std::fprintf(f_, "%c %lld %s %s\n", dir, ms,
                     airprobe::proto::hexcompact(p).c_str(), label.c_str());
        std::fflush(f_);
    }

    void note(const std::string& text)
    {
        if (!f_) return;
        std::fprintf(f_, "# %s\n", text.c_str());
        std::fflush(f_);
    }

    void close()
    {
        if (f_) { std::fclose(f_); f_ = nullptr; }
    }

    bool is_open() const { return f_ != nullptr; }
    const std::string& path() const { return path_; }

    ~Capture() { close(); }

private:
    std::FILE*  f_ = nullptr;
    std::string path_;
};

void usage()
{
    std::puts(
        "airprobe -- AirPods AAP channel probe (s2: battery hunt)\n"
        "\n"
        "usage: airprobe <MAC> [options]\n"
        "\n"
        "  <MAC>              classic paired address, AA:BB:CC:DD:EE:FF\n"
        "                     find it with:  bluetoothctl devices\n"
        "\n"
        "the battery-hunt knobs (defaults match the working Linux client):\n"
        "  --notify VARIANT   5ff  eleven bytes, five FF mask bytes [default]\n"
        "                     4ff  ten bytes, FF FF FF FF\n"
        "                     4fe  ten bytes, FF FF FE FF  (what s1 sent)\n"
        "  --caps VARIANT     d7   host capabilities 0xD7        [default]\n"
        "                     ff   host capabilities 0xFF\n"
        "                     none skip the host-capabilities packet\n"
        "  --sequence MODE    ack    wait for each ack before the next packet\n"
        "                            [default]\n"
        "                     sleep  200ms delays, no waiting (s1 behaviour)\n"
        "\n"
        "other options:\n"
        "  --duration N       seconds to listen (default 90, 0 = until Ctrl-C)\n"
        "  --capture FILE     write a transcript (default airprobe-<time>.cap)\n"
        "  --no-capture       do not write a transcript\n"
        "  --no-handshake     skip the handshake, to prove it is required\n"
        "  --insecure         skip setsockopt(BT_SECURITY_MEDIUM)\n"
        "  --verbose          debug-level logging\n"
        "  --help             this text\n"
        "\n"
        "To reproduce s1 exactly:  --notify 4fe --caps none --sequence sleep\n"
        "\n"
        "The pods must be connected to THIS machine. Only one host holds the\n"
        "AAP channel at a time -- if the iPhone has them, this will fail.\n"
        "\n"
        "WHILE IT LISTENS, GENERATE EVENTS. Battery is push-only: the pods\n"
        "send it when something changes, so make something change. Take a pod\n"
        "out, put it back, open the case, close the case, put a pod in the\n"
        "case, long-press a stem.\n");
}

std::string default_capture_name()
{
    std::time_t now = std::time(nullptr);
    char buf[64];
    std::strftime(buf, sizeof(buf), "airprobe-%Y%m%d-%H%M%S.cap", std::localtime(&now));
    return buf;
}

// ms_since() used to live here. Session owns the run clock now --
// session.elapsed_ms() -- so the CLI and the transcript cannot disagree
// about when a packet arrived.

} // namespace

int main(int argc, char** argv)
{
    auto logger = spdlog::stderr_color_mt("airprobe");
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    spdlog::set_level(spdlog::level::info);

    std::string mac;
    std::string capture_path;
    bool   want_capture   = true;
    bool   require_security = true;
    bool   do_handshake   = true;
    int    duration       = 90;
    Caps   caps           = Caps::D7;
    Notify notify         = Notify::Five;
    Seq    sequence       = Seq::Ack;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto need_value = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", what);
                std::exit(2);
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") { usage(); return 0; }
        else if (arg == "--insecure")     require_security = false;
        else if (arg == "--no-handshake") do_handshake = false;
        else if (arg == "--no-capture")   want_capture = false;
        else if (arg == "--verbose" || arg == "-v") spdlog::set_level(spdlog::level::debug);
        else if (arg == "--capture")  { capture_path = need_value("--capture"); }
        else if (arg == "--duration") { duration = std::atoi(need_value("--duration")); }
        else if (arg == "--notify") {
            std::string v = need_value("--notify");
            if      (v == "5ff") notify = Notify::Five;
            else if (v == "4ff") notify = Notify::FourFF;
            else if (v == "4fe") notify = Notify::FourFE;
            else { std::fprintf(stderr, "--notify must be 5ff, 4ff or 4fe\n"); return 2; }
        }
        else if (arg == "--caps") {
            std::string v = need_value("--caps");
            if      (v == "d7")   caps = Caps::D7;
            else if (v == "ff")   caps = Caps::FF;
            else if (v == "none") caps = Caps::None;
            else { std::fprintf(stderr, "--caps must be d7, ff or none\n"); return 2; }
        }
        else if (arg == "--sequence") {
            std::string v = need_value("--sequence");
            if      (v == "ack")   sequence = Seq::Ack;
            else if (v == "sleep") sequence = Seq::Sleep;
            else { std::fprintf(stderr, "--sequence must be ack or sleep\n"); return 2; }
        }
        else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "unknown option: %s\n\n", arg.c_str());
            usage();
            return 2;
        }
        else if (mac.empty()) mac = arg;
        else { std::fprintf(stderr, "unexpected argument: %s\n\n", arg.c_str()); usage(); return 2; }
    }

    if (mac.empty()) { usage(); return 2; }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    Capture cap;
    if (want_capture) {
        if (capture_path.empty()) capture_path = default_capture_name();
        cap.open(capture_path);   // a failure here is not fatal; the run still has value
    }

    std::printf("run: caps=%s notify=%s sequence=%s\n",
                caps_name(caps), notify_name(notify), sequence == Seq::Ack ? "ack" : "sleep");
    if (cap.is_open()) std::printf("capture: %s\n", cap.path().c_str());

    // --- connect ---------------------------------------------------------
    //
    // The whole handshake sequence is behind Session now. What used to be
    // ~120 lines of phase machine in this file is one open() and a pump.
    airprobe::SessionConfig scfg;
    scfg.host_caps        = caps_packet(caps);
    scfg.notify_request   = notify_packet(notify);
    scfg.ack_driven       = (sequence == Seq::Ack);
    scfg.send_handshake   = do_handshake;
    scfg.require_security = require_security;

    airprobe::Session session(scfg);

    // The transcript hangs off the packet hook, which sees both directions in
    // order. That interleaving is the reason the hook exists: a return value
    // from step() cannot report the two packets that go out when an ack
    // lands, and a .cap file with the sends missing is not a transcript.
    long long sent_count = 0;
    session.on_packet([&](airprobe::Session::Direction dir,
                          std::span<const uint8_t> bytes,
                          const char* what) {
        if (dir == airprobe::Session::Direction::Sent) {
            ++sent_count;
            cap.packet('>', session.elapsed_ms(), bytes, what ? what : "sent");
        }
        // Inbound packets are recorded in the loop below, where the label has
        // already been computed for the console. Labelling twice would be the
        // only cost of doing it here, and the label is not free.
    });

    if (!do_handshake) cap.note("handshake skipped");

    auto result = session.open(mac);
    if (!result.ok) {
        std::printf("\nRESULT: failed\n");
        std::printf("  stage reached : %s\n", airprobe::stage_name(result.reached));
        std::printf("  errno         : %d\n", result.err);
        std::printf("\n%s\n", result.diagnosis.c_str());
        cap.note("connect failed");
        return 1;
    }

    cap.header(mac, caps, notify, sequence);

    // --- listen ----------------------------------------------------------
    std::printf("\n");
    std::printf("listening. GENERATE EVENTS -- battery is push-only, so the pods\n");
    std::printf("only report it when something changes. Take a pod out. Put it\n");
    std::printf("back. Open the case. Close the case. Long-press a stem.\n");
    if (duration > 0)
        std::printf("stopping after %d seconds. Ctrl-C to stop early.\n", duration);
    else
        std::printf("running until Ctrl-C.\n");
    std::printf("%s\n", std::string(70, '-').c_str());
    std::fflush(stdout);

    int  count = 0;
    bool battery_seen = false;
    long long last_nudge = 0;
    std::map<std::string, int> label_counts;

    // Every battery report, kept in order. Printed as a table at the end so
    // the numbers can be held against a physical state in one glance instead
    // of being scraped back out of the scrollback.
    struct BatterySample { long long ms; airprobe::BatteryReport report; };
    std::vector<BatterySample> battery_log;

    // Rotating prompts. Battery is push-only, so a silent run is ambiguous
    // between "not registered to hear it" and "nothing happened worth
    // reporting" -- and the second is ruled out only by making something
    // happen. Nudging on our own timer between reads is also the concrete
    // reason Session exposes step() rather than owning the loop.
    const char* nudges[] = {
        "take a pod OUT of your ear",
        "put the pod BACK in",
        "OPEN the case lid",
        "CLOSE the case lid",
        "put one pod IN the case, lid open",
        "take it back out",
        "long-press a stem to change noise mode",
    };
    const int nudge_count = static_cast<int>(sizeof(nudges) / sizeof(nudges[0]));
    int nudge_index = 0;

    while (!g_stop) {
        const long long now = session.elapsed_ms();

        if (duration > 0 && now >= static_cast<long long>(duration) * 1000) break;

        if (session.listening() && now - last_nudge >= 12000) {
            std::printf("\n  >>> try this now: %s\n", nudges[nudge_index % nudge_count]);
            std::fflush(stdout);
            ++nudge_index;
            last_nudge = now;
        }

        auto s = session.step(500);

        // Session's notes are protocol events worth keeping: ack timeouts,
        // notification re-sends, parse refusals. They used to be written
        // inline by the sequencer; they arrive through the Step now so the
        // transcript is unchanged by the move.
        if (s.note) cap.note(s.note);

        if (s.closed) {
            std::printf("\nchannel closed by peer.\n");
            break;
        }
        if (!s.packet) continue;

        ++count;
        const long long at = session.elapsed_ms();
        const std::string label = airprobe::proto::opcode_label(s.bytes);
        label_counts[label]++;

        std::printf("\n[%6lld ms] packet #%d  %zu bytes  <%s>\n",
                    at, count, s.bytes.size(), label.c_str());
        std::printf("%s", airprobe::proto::hexdump(s.bytes).c_str());
        std::fflush(stdout);

        cap.packet('<', at, s.bytes, label);

        if (airprobe::proto::opcode_of(s.bytes) == airprobe::proto::kOpBattery) {
            if (!battery_seen) {
                battery_seen = true;
                std::printf("\n  *** FIRST BATTERY PACKET ***\n");
                cap.note("FIRST BATTERY PACKET");
            }

            if (s.battery) {
                const auto& report = *session.last_battery();
                std::printf("  PARSED: %s\n", report.to_string().c_str());
                cap.note("parsed: " + report.to_string());
                battery_log.push_back({at, report});
            } else {
                // Session already recorded the refusal in s.note and kept the
                // bytes in the transcript. Say it on the console too, loudly:
                // a structural surprise is a finding.
                std::printf("  PARSE REFUSED -- shape does not match the known format.\n");
            }
            std::fflush(stdout);
        }
    }

    // --- summary ---------------------------------------------------------
    std::printf("%s\n", std::string(70, '-').c_str());
    std::printf("\nRESULT: %d packet%s received\n", count, count == 1 ? "" : "s");
    std::printf("  run   : caps=%s notify=%s sequence=%s\n",
                caps_name(caps), notify_name(notify), sequence == Seq::Ack ? "ack" : "sleep");
    std::printf("  BATTERY (opcode 0x04): %s\n", battery_seen ? "YES" : "no");

    if (!label_counts.empty()) {
        std::printf("\nwhat arrived:\n");
        for (const auto& [label, n] : label_counts)
            std::printf("  %3d x  %s\n", n, label.c_str());
    }

    if (!battery_log.empty()) {
        std::printf("\nbattery reports (%zu). Hold these against the iPhone:\n",
                    battery_log.size());
        for (const auto& s : battery_log)
            std::printf("  %7lld ms  %s\n", s.ms, s.report.to_string().c_str());
        std::puts("");
        std::puts("A level shown as --% is NOT zero. It means the component was");
        std::puts("disconnected and its level byte carried no reading.");
    }

    if (cap.is_open()) {
        cap.note(battery_seen ? "end: battery SEEN" : "end: no battery");
        std::printf("\ncapture written: %s\n", cap.path().c_str());
        std::printf("Upload that file next session -- it is the durable evidence.\n");
    }

    if (!battery_seen) {
        std::puts("");
        std::puts("No battery packet. Battery is push-only, so there are only two");
        std::puts("possible reasons: the pods had nothing to report, or they were");
        std::puts("not told to report it. Work the matrix, changing ONE thing:");
        std::puts("");
        std::puts("  1. Re-run and generate events aggressively the whole time --");
        std::puts("     a pod in and out of the case is the biggest state change");
        std::puts("     available. Rule this out FIRST; it costs nothing.");
        std::puts("  2. --caps ff        (different capability claim)");
        std::puts("  3. --caps none      (does capabilities matter at all?)");
        std::puts("  4. --notify 4ff     (ten-byte mask, all FF)");
        std::puts("  5. --sequence sleep (was the ack-waiting the difference?)");
        std::puts("");
        std::puts("Keep every .cap file. Whichever run produces battery, the");
        std::puts("difference between its header line and the others IS the answer.");
    }

    cap.close();
    return count > 0 ? 0 : 1;
}
