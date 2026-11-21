#ifndef CARLIFE_LIFECYCLE_HPP
#define CARLIFE_LIFECYCLE_HPP

#include <time.h>

namespace carlife {

inline double now() {
    struct timespec stamp;
    clock_gettime(CLOCK_MONOTONIC, &stamp);
    return stamp.tv_sec + stamp.tv_nsec / 1e9;
}

enum Status {
    Idle = 0, Searching, Connecting, Projecting, Hidden, NotFound,
    HandshakeTimeout, Disconnected, DisplayUnavailable, Stopped, ChoosePhone,
    ApplyingWifiKey, WifiKeyApplyFailed, PreparingDisplay,
    WatchdogReady, FactoryBorrowing, FactoryRestored, Heartbeat, Finishing,
    CoveringDisplay, DisplayReleased
};

const int SearchSeconds = 60;
const int HandshakeSeconds = 15;
const unsigned StatusFd = 3;

struct StatusEvent {
    unsigned char state, count, peers[3];
};

class ProjectionWatchdog {
public:
    enum Action { Wait, Recover, ForceStop, VehicleBusy };

    void start(double time) {
        deadline_ = time + SearchSeconds + HandshakeSeconds + 8;
        stoppedAt_ = -1;
        prepared_ = compatible_ = factory_ = wifi_ = forced_ = false;
    }

    void observe(unsigned state, double time) {
        switch (state) {
        case WatchdogReady: compatible_ = true; break;
        case ApplyingWifiKey:
            wifi_ = true;
            deadline_ = time + SearchSeconds;
            break;
        case WifiKeyApplyFailed: wifi_ = false; break;
        case Searching: wifi_ = false; break;
        case Connecting: deadline_ = time + HandshakeSeconds + 3; break;
        case PreparingDisplay: prepared_ = true; deadline_ = time + 22; break;
        case FactoryBorrowing: factory_ = true; break;
        case FactoryRestored: factory_ = false; break;
        case Projecting: case Hidden: case Heartbeat: deadline_ = time + 7; break;
        case Finishing: deadline_ = time + 10; break;
        }
    }

    bool stopping() const { return stoppedAt_ >= 0; }
    void stop(double time) { stoppedAt_ = time; }
    bool restoreDue(double time) const { return stopping() && time - stoppedAt_ >= 4; }

    Action poll(double time) {
        if (!stopping()) return time >= deadline_ ? Recover : Wait;
        if (wifi_ || factory_ || (prepared_ && !compatible_))
            return time - stoppedAt_ >= 10 ? VehicleBusy : Wait;
        if (!forced_ && time - stoppedAt_ >= 4) {
            forced_ = true;
            return ForceStop;
        }
        return time - stoppedAt_ >= 10 ? VehicleBusy : Wait;
    }

    bool factoryPending() const { return factory_; }

private:
    double deadline_ = 0, stoppedAt_ = -1;
    bool prepared_ = false, compatible_ = false, factory_ = false, wifi_ = false, forced_ = false;
};

}

#endif
