// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Scott Combs
#include "airprobe/aap_socket.h"
#include "airprobe/aap_protocol.h"

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/l2cap.h>

#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace airprobe {

const char* stage_name(Stage s)
{
    switch (s) {
    case Stage::ParseAddress: return "parse-address";
    case Stage::AdapterCheck: return "adapter-check";
    case Stage::CreateSocket: return "create-socket";
    case Stage::SetSecurity:  return "set-security";
    case Stage::Connect:      return "connect";
    case Stage::Connected:    return "connected";
    }
    return "unknown";
}

std::string diagnose(Stage stage, int err)
{
    switch (stage) {
    case Stage::ParseAddress:
        return "Not a Bluetooth address. Expected form AA:BB:CC:DD:EE:FF. "
               "Get the classic paired address with: bluetoothctl devices";

    case Stage::AdapterCheck:
        return "No live Bluetooth adapter. Check `rfkill list` for a soft block "
               "and `bluetoothctl show` for Powered: yes. The daemon must never "
               "spin a connect loop in this state.";

    case Stage::CreateSocket:
        if (err == EPERM || err == EACCES)
            return "Kernel refused the raw L2CAP socket. Either add the binary "
                   "cap_net_raw,cap_net_admin, or add your user to the "
                   "'bluetooth' group. See build.sh notes.";
        if (err == EAFNOSUPPORT || err == EPROTONOSUPPORT)
            return "Kernel has no BTPROTO_L2CAP support. Unexpected on Fedora; "
                   "check the Asahi kernel config.";
        return "socket() failed for an unclassified reason.";

    case Stage::SetSecurity:
        if (err == EPERM || err == EACCES)
            return "Refused BT_SECURITY_MEDIUM -- permissions again. Try "
                   "--insecure to see whether the pods accept the channel "
                   "without it.";
        if (err == ENOPROTOOPT)
            return "BT_SECURITY not supported on this socket type. Try "
                   "--insecure.";
        return "setsockopt(BT_SECURITY) failed. Try --insecure to isolate.";

    case Stage::Connect:
        if (err == EBUSY)
            return "Channel busy. Almost certainly the iPhone holds the pods -- "
                   "only ONE host holds the AAP channel at a time. Disconnect "
                   "them from the phone and retry.";
        if (err == EHOSTDOWN || err == ENETDOWN)
            return "Host down. The pods are not currently connected to this "
                   "machine as a classic device. Connect them first in "
                   "GNOME Settings -> Bluetooth, then retry.";
        if (err == ECONNREFUSED)
            return "Refused at PSM 0x1001. Either the pods are connected to "
                   "another host, or this model does not expose AAP on that "
                   "PSM. Confirm the pods show as Connected before suspecting "
                   "the PSM.";
        if (err == ETIMEDOUT)
            return "Timed out. Pods may be in the case or out of range.";
        if (err == EPERM || err == EACCES)
            return "Permission denied on connect. Capabilities / bluetooth "
                   "group -- see build.sh notes.";
        if (err == EINVAL)
            return "Invalid argument -- suspect the address type. This must be "
                   "BDADDR_BREDR (classic), never BDADDR_LE_PUBLIC or "
                   "BDADDR_LE_RANDOM.";
        return "connect() failed for an unclassified reason.";

    case Stage::Connected:
        return "";
    }
    return "";
}

namespace {

ConnectResult fail(Stage stage, int err)
{
    ConnectResult r;
    r.ok = false;
    r.reached = stage;
    r.err = err;
    r.diagnosis = diagnose(stage, err);
    return r;
}

} // namespace

AapSocket::~AapSocket()
{
    close();
}

AapSocket::AapSocket(AapSocket&& other) noexcept
    : fd_(std::exchange(other.fd_, -1))
{
}

AapSocket& AapSocket::operator=(AapSocket&& other) noexcept
{
    if (this != &other) {
        close();
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

void AapSocket::close()
{
    if (fd_ >= 0) {
        ::close(fd_);
        spdlog::debug("aap: closed fd {}", fd_);
        fd_ = -1;
    }
}

ConnectResult AapSocket::connect_to(const std::string& mac, bool require_security)
{
    close();

    // --- Stage: parse address -------------------------------------------
    bdaddr_t target{};
    if (str2ba(mac.c_str(), &target) < 0) {
        spdlog::error("aap: cannot parse address '{}'", mac);
        return fail(Stage::ParseAddress, EINVAL);
    }
    spdlog::debug("aap: target address parsed: {}", mac);

    // --- Stage: adapter check -------------------------------------------
    // hci_get_route(nullptr) returns the id of the first available adapter,
    // or -1 when there is none up. Checking here turns "adapter is off" into
    // a clean diagnosis instead of a confusing connect failure.
    int dev_id = hci_get_route(nullptr);
    if (dev_id < 0) {
        int e = errno;
        spdlog::error("aap: no route to any adapter (errno {})", e);
        return fail(Stage::AdapterCheck, e);
    }
    spdlog::debug("aap: using adapter hci{}", dev_id);

    // --- Stage: create socket -------------------------------------------
    int fd = ::socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (fd < 0) {
        int e = errno;
        spdlog::error("aap: socket() failed: {}", std::strerror(e));
        return fail(Stage::CreateSocket, e);
    }
    fd_ = fd;
    spdlog::debug("aap: socket created, fd {}", fd_);

    // --- Stage: security -------------------------------------------------
    // Open question from the discovery session: the pods may require an
    // encrypted link before they will accept the channel. --insecure skips
    // this so we can test both paths without a rebuild.
    if (require_security) {
        struct bt_security sec{};
        sec.level = BT_SECURITY_MEDIUM;
        if (::setsockopt(fd_, SOL_BLUETOOTH, BT_SECURITY, &sec, sizeof(sec)) < 0) {
            int e = errno;
            spdlog::error("aap: setsockopt(BT_SECURITY) failed: {}",
                          std::strerror(e));
            close();
            return fail(Stage::SetSecurity, e);
        }
        spdlog::debug("aap: BT_SECURITY_MEDIUM set");
    } else {
        spdlog::debug("aap: security step skipped (--insecure)");
    }

    // --- Stage: connect --------------------------------------------------
    struct sockaddr_l2 addr{};
    addr.l2_family      = AF_BLUETOOTH;
    addr.l2_psm         = htobs(kAapPsm);
    addr.l2_bdaddr      = target;
    addr.l2_bdaddr_type = BDADDR_BREDR;   // classic, NOT LE

    spdlog::debug("aap: connecting to {} psm 0x{:04x} (BDADDR_BREDR)",
                  mac, kAapPsm);

    if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        int e = errno;
        spdlog::error("aap: connect() failed: {}", std::strerror(e));
        close();
        return fail(Stage::Connect, e);
    }

    spdlog::info("aap: channel open to {} on psm 0x{:04x}", mac, kAapPsm);

    ConnectResult r;
    r.ok = true;
    r.reached = Stage::Connected;
    r.err = 0;
    return r;
}

bool AapSocket::send_packet(std::span<const uint8_t> bytes, const char* what)
{
    if (fd_ < 0) {
        spdlog::error("aap: send '{}' with no socket", what);
        return false;
    }

    spdlog::debug("aap: -> {} [{} bytes] {}", what, bytes.size(),
                  proto::hexline(bytes));

    ssize_t n = ::write(fd_, bytes.data(), bytes.size());
    if (n < 0) {
        spdlog::error("aap: write '{}' failed: {}", what, std::strerror(errno));
        return false;
    }
    if (static_cast<size_t>(n) != bytes.size()) {
        // SOCK_SEQPACKET preserves message boundaries; a short write is a
        // fault, not a resumable state.
        spdlog::error("aap: short write on '{}': {} of {} bytes",
                      what, n, bytes.size());
        return false;
    }
    return true;
}

int AapSocket::read_packet(std::vector<uint8_t>& out, int timeout_ms)
{
    if (fd_ < 0) return -1;

    struct pollfd pfd{};
    pfd.fd = fd_;
    pfd.events = POLLIN;

    int pr = ::poll(&pfd, 1, timeout_ms);
    if (pr < 0) {
        if (errno == EINTR) return 0;   // signal during wait; caller re-loops
        spdlog::error("aap: poll failed: {}", std::strerror(errno));
        return -1;
    }
    if (pr == 0) return 0;              // timeout -- expected and normal

    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        spdlog::warn("aap: socket hung up (revents 0x{:x})", pfd.revents);
        return -1;
    }

    // AAP packets are small; 1024 is generous. SOCK_SEQPACKET gives us one
    // whole message per read, so there is no reassembly to do.
    out.assign(1024, 0);
    ssize_t n = ::read(fd_, out.data(), out.size());
    if (n < 0) {
        spdlog::error("aap: read failed: {}", std::strerror(errno));
        return -1;
    }
    if (n == 0) {
        spdlog::warn("aap: peer closed the channel");
        return -1;
    }

    out.resize(static_cast<size_t>(n));
    return static_cast<int>(n);
}

} // namespace airprobe
