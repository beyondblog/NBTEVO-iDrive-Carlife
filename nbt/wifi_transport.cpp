#include "wifi_transport.hpp"
#include "lifecycle.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace carlife {
namespace {

const unsigned Ports[] = {7240, 8240, 9240, 9241, 9242, 9340};

}

bool WifiTransport::ready(int fd, short events, int timeout) const {
    double until = now() + timeout / 1000.0;
    if (deadline_ && deadline_ < until) until = deadline_;
    while (!stop_ || !*stop_) {
        const double remaining = (until - now()) * 1000;
        if (remaining <= 0) return false;
        struct pollfd item = {fd, events, 0};
        const int result = poll(&item, 1, remaining < 100 ? static_cast<int>(remaining) + 1 : 100);
        if (result > 0) return item.revents & events;
        if (result < 0 && errno != EINTR) return false;
    }
    return false;
}

WifiTransport::WifiTransport()
    : address_(0), deadline_(0), stop_(NULL), wfd_only_(false), error_("") {
    for (unsigned i = 0; i < 6; ++i) sockets_[i] = -1;
}

WifiTransport::~WifiTransport() { close(); }

void WifiTransport::close() {
    for (unsigned i = 0; i < 6; ++i) {
        if (sockets_[i] >= 0) ::close(sockets_[i]);
        sockets_[i] = -1;
    }
}

bool WifiTransport::connectPhone(const char* peer, double deadline, const volatile sig_atomic_t* stop,
                                 bool wfd_only) {
    close();
    deadline_ = deadline;
    stop_ = stop;
    wfd_only_ = wfd_only;
    struct in_addr ip;
    if (inet_pton(AF_INET, peer, &ip) != 1) {
        error_ = "expected phone IPv4 address";
        return false;
    }
    address_ = ip.s_addr;
    return connectLane(0);
}

bool WifiTransport::connectLane(unsigned index) {
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = address_;
    address.sin_port = htons(Ports[index]);
    const int fd = sockets_[index] = socket(AF_INET, SOCK_STREAM, 0);
    error_ = "could not open nonblocking CarLife socket";
    if (fd < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0) return false;
    if (wfd_only_) {
        struct sockaddr_in source = {};
        source.sin_family = AF_INET;
        source.sin_addr.s_addr = inet_addr("172.16.222.1");
        if (bind(fd, reinterpret_cast<struct sockaddr*>(&source), sizeof(source)) != 0) return false;
    }
    error_ = index == 0 ? "phone is not listening on TCP 7240" :
                         "phone did not accept all six CarLife channels";
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) < 0) {
        if (errno != EINPROGRESS || !ready(fd, POLLOUT, 1500)) return false;
        int result;
        socklen_t size = sizeof(result);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &result, &size) || result) return false;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    error_ = "";
    return true;
}

bool WifiTransport::send(const Bytes& frame) {
    if (frame.size() <= 8 || (frame[3] != 1 && frame[3] != 6)) return false;
    // HU_INFO follows VERSION_MATCH; vivo opens the remaining listeners at that point.
    if (frame.size() >= 16 && frame[3] == 1 && frame[13] == 1 &&
        frame[14] == 0x80 && frame[15] == 3 && sockets_[1] < 0) {
        for (unsigned i = 1; i < 6; ++i)
            if (!connectLane(i)) return false;
    }
    const int fd = sockets_[frame[3] - 1];
    size_t offset = 8; // TCP lanes carry the inner packet, without the USB multiplex header.
    while (offset < frame.size()) {
        if (!ready(fd, POLLOUT, 1000)) return false;
        const ssize_t count = ::send(fd, &frame[offset], frame.size() - offset, 0);
        if (count <= 0) return false;
        offset += size_t(count);
    }
    return true;
}

bool WifiTransport::receive(Session& session) {
    struct pollfd lanes[6];
    for (unsigned i = 0; i < 6; ++i) {
        lanes[i].fd = sockets_[i];
        lanes[i].events = POLLIN;
        lanes[i].revents = 0;
    }
    if (poll(lanes, 6, 0) < 0) return errno == EINTR;
    uint8_t data[32768];
    for (unsigned i = 0; i < 6; ++i) {
        if (lanes[i].revents & (POLLERR | POLLNVAL)) return false;
        if (!(lanes[i].revents & (POLLIN | POLLHUP))) continue;
        const ssize_t count = recv(lanes[i].fd, data, sizeof(data), 0);
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        if (count <= 0 || !session.receiveChannel(i + 1, data, size_t(count))) return false;
    }
    return true;
}

}
