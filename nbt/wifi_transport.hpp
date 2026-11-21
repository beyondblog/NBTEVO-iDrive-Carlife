#ifndef NBT_CARLIFE_WIFI_TRANSPORT_HPP
#define NBT_CARLIFE_WIFI_TRANSPORT_HPP

#include "carlife_session.hpp"
#include <signal.h>

namespace carlife {

// Historical CarLife Wi-Fi profile: the phone is the TCP server.
class WifiTransport {
public:
    WifiTransport();
    ~WifiTransport();
    bool connectPhone(const char* peer, double deadline = 0, const volatile sig_atomic_t* stop = 0,
                      bool wfd_only = false);
    void setDeadline(double deadline) { deadline_ = deadline; }
    void allowShutdown(double deadline) { deadline_ = deadline; stop_ = 0; }
    void close();
    bool send(const Bytes& usb_frame);
    bool receive(Session& session);
    const char* error() const { return error_; }

private:
    WifiTransport(const WifiTransport&);
    WifiTransport& operator=(const WifiTransport&);
    bool connectLane(unsigned index);
    bool ready(int fd, short events, int timeout) const;
    uint32_t address_;
    double deadline_;
    const volatile sig_atomic_t* stop_;
    bool wfd_only_;
    int sockets_[6];
    const char* error_;
};

}
#endif
