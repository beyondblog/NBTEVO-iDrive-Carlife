// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// RHMI ABI declarations adapted from Supra-iDrive6-Android-Auto.
// Required Notice: Copyright (c) 2026 nanaisu - NBT-AAD (https://github.com/Sq00ky/Supra-iDrive6-Android-Auto)
#include "../firewall.hpp"
#include "../lifecycle.hpp"
#include "../wifi_credentials.hpp"
#include <errno.h>
#include <fcntl.h>
#include <new>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace bmw {
class IDispatcher;
class Bundle;
class Runnable;
class Parcel {
public:
    explicit Parcel(bool);
    ~Parcel();
    void readBool(bool&) const;
private:
    uint32_t storage_[8];
};
enum RHMIInterfaceVersion { Version1 = 1 };
template<class T> class BasicString {
public:
    BasicString(const T*);
    BasicString(const BasicString&);
    ~BasicString();
private:
    T* data_;
};
class TextID {
public:
    explicit TextID(unsigned);
private:
    unsigned id_, type_;
};
template<class R, class A1, class A2, class A3, class A4> struct Delegate {
    void* object;
    R (*stub)(void*, A1);
};
typedef Delegate<bool, const Parcel&, void, void, void> Action;
class PluginApp {
public:
    PluginApp(IDispatcher&, BasicString<char>, unsigned, unsigned, RHMIInterfaceVersion,
              const Bundle&, const Bundle&);
    void addHandler(unsigned, const Action&);
    void addHandler(unsigned, unsigned, const Action&);
    void triggerEvent(unsigned);
    void updateModel(unsigned, const TextID&);
    void updateModel(unsigned, const BasicString<char>&);
    void setProperty(unsigned, unsigned, const Parcel&);
    void post(Runnable*, unsigned);
    bool onInit();
    void onFinalize();
private:
    unsigned char storage_[0x200];
};
}

namespace {

const char* const Daemon = "/var/opt/nbt-carlife/nbt-carlife";
const unsigned AppStatusModel = 20, PfStatusModel = 21;
const unsigned MenuState = 10, BlankState = 11, ChoiceState = 12;
const unsigned WifiPage = 13, WifiStatusModel = 43, WifiSsidModel = 44, WifiPassModel = 45;
const unsigned ShowBlankEvent = 50, ShowChoiceEvent = 51, ShowMenuEvent = 52;
const unsigned ShowWifiEvent = 53;
const unsigned InitSlot = 22, FinalizeSlot = 23, SlotCount = 34;
const unsigned PfIdle = 120, PfWorking = 121, PfInstalled = 122, PfSaved = 123,
               PfAbsent = 124, PfOff = 125, PfFailed = 126, Stopping = 127, NeedsRecovery = 128,
               PfOffAndAbsent = 129, PfStopFirst = 130, Recovering = 131, Recovered = 132;
const unsigned WifiHidden = 140, WifiReading = 141, WifiResetting = 142;
const unsigned WifiResultBase = 143;

struct Poller { void** vptr; };
void discardPoller(void*) {}
void pollApp(void*);
void* pollVtable[] = {
    reinterpret_cast<void*>(&discardPoller), reinterpret_cast<void*>(&discardPoller),
    reinterpret_cast<void*>(&pollApp)
};
Poller poller = {pollVtable};
void* pluginVtable[2 + SlotCount];

bmw::PluginApp* app = NULL;
bool running = false, menuFocused = false, blankFocused = false, choiceFocused = false, resume = false;
bool wifiFocused = false;
unsigned choices = 0;
int readFd = -1;
pid_t daemon = -1, pfChild = -1;
int wifiFd = -1;
pid_t wifiChild = -1;
unsigned wifiState = WifiHidden;
unsigned projection = carlife::Idle, pfState = PfIdle;
carlife::ProjectionWatchdog watchdog;
bool automaticRecovery = false, needsDisplayRestore = false, restoreAttempted = false;
pid_t restoreChild = -1;
double restoreDeadline = 0;

void update() {
    if (!app || !running) return;
    const unsigned shown = projection >= Stopping ? projection : 100 + projection;
    app->updateModel(AppStatusModel, bmw::TextID(shown));
    app->updateModel(PfStatusModel, bmw::TextID(pfState));
}

pid_t launch(const char* option, int statusFd = -1, const char* arg = NULL, const char* extra = NULL) {
    int input = open("/dev/null", O_RDONLY);
    if (input < 0) return -1;
    static char path[] = "PATH=/bin:/usr/bin:/sbin:/usr/sbin:/opt/sys/bin:/opt/mm/bin";
    static char libs[] = "LD_LIBRARY_PATH=/opt/sys/lib:/opt/hmi/lib:/opt/conn/lib:/opt/conn/lib/carplay:/opt/mm/lib:/usr/lib:/lib";
    char* argv[] = {const_cast<char*>(Daemon), const_cast<char*>(option),
                    const_cast<char*>(arg ? arg : statusFd < 0 ? NULL : "--managed"),
                    const_cast<char*>(extra), NULL};
    char* env[128] = {path, libs};
    size_t count = 2;
    for (char** entry = environ; entry && *entry && count < 127; ++entry)
        if (strncmp(*entry, "PATH=", 5) && strncmp(*entry, "LD_LIBRARY_PATH=", 16) &&
            strncmp(*entry, "LD_PRELOAD=", 11))
            env[count++] = *entry;
    struct inheritance flags = {};
    flags.flags = SPAWN_SETGROUP | SPAWN_SETSIGDEF | SPAWN_SETSIGMASK;
    flags.pgroup = SPAWN_NEWPGROUP;
    sigfillset(&flags.sigdefault);
    sigemptyset(&flags.sigmask);
    int fds[] = {input, STDOUT_FILENO, STDERR_FILENO, statusFd};
    const pid_t child = spawn(Daemon, statusFd < 0 ? 3 : 4, fds, &flags, argv, env);
    close(input);
    return child;
}

void leaveProjection() {
    if (blankFocused || choiceFocused) app->triggerEvent(ShowMenuEvent);
}

void requestDisplayRestore() {
    if (!needsDisplayRestore || restoreAttempted) return;
    restoreAttempted = true;
    restoreChild = launch("--restore-idrive");
    restoreDeadline = carlife::now() + 5;
}

void pollDisplayRestore() {
    if (restoreChild < 0) return;
    int result = 0;
    const pid_t ended = waitpid(restoreChild, &result, WNOHANG);
    if (ended == restoreChild || (ended < 0 && errno == ECHILD)) {
        restoreChild = -1;
        const bool restored = ended > 0 && WIFEXITED(result) && WEXITSTATUS(result) == 0;
        if (restored) needsDisplayRestore = false;
        if (daemon < 0 && automaticRecovery && !watchdog.factoryPending())
            projection = restored ? Recovered : carlife::DisplayUnavailable;
    } else if (carlife::now() >= restoreDeadline) {
        kill(restoreChild, SIGKILL);
    }
}

void beginStop(bool recovery) {
    watchdog.stop(carlife::now());
    automaticRecovery = recovery;
    resume = false;
    projection = recovery ? Recovering : Stopping;
    kill(daemon, SIGTERM);
    leaveProjection();
}

void pollProjection() {
    pollDisplayRestore();
    if (daemon < 0) return;
    carlife::StatusEvent event;
    while (read(readFd, &event, sizeof(event)) == sizeof(event)) {
        const unsigned msg = event.state;
        watchdog.observe(msg, carlife::now());
        if (msg == carlife::CoveringDisplay || msg == carlife::Projecting) needsDisplayRestore = true;
        if (msg == carlife::Hidden || msg == carlife::DisplayReleased) needsDisplayRestore = false;
        if (msg > carlife::WifiKeyApplyFailed || watchdog.stopping()) continue;
        projection = msg;
        if (msg == carlife::ChoosePhone && event.count >= 2 && event.count <= 3) {
            choices = event.count;
            for (unsigned i = 0; i < 3; ++i) {
                const unsigned label = i < choices ? 1000 + event.peers[i] : 113;
                app->updateModel(27 + i, bmw::TextID(label));
                app->setProperty(70 + i, 1, bmw::Parcel(i < choices));
            }
            if (menuFocused) app->triggerEvent(ShowChoiceEvent);
        } else if (msg == carlife::Searching || msg == carlife::NotFound) {
            choices = 0;
            if (choiceFocused) app->triggerEvent(ShowMenuEvent);
        }
        if (msg == carlife::Projecting) {
            if (menuFocused || resume) app->triggerEvent(ShowBlankEvent);
            else kill(daemon, SIGUSR2);
            resume = false;
        }
    }
    int result = 0;
    const pid_t ended = waitpid(daemon, &result, WNOHANG);
    if (ended == daemon || (ended < 0 && errno == ECHILD)) {
        close(readFd);
        readFd = -1;
        daemon = -1;
        choices = 0;
        resume = false;
        requestDisplayRestore();
        if (watchdog.factoryPending()) projection = NeedsRecovery;
        else if (automaticRecovery) projection = restoreChild >= 0 ? Recovering :
                                                needsDisplayRestore ? carlife::DisplayUnavailable : Recovered;
        else if (watchdog.stopping()) projection = carlife::Stopped;
        else if (projection <= carlife::Hidden || projection == carlife::ChoosePhone)
            projection = carlife::Disconnected;
        leaveProjection();
        return;
    }
    const double time = carlife::now();
    switch (watchdog.poll(time)) {
    case carlife::ProjectionWatchdog::Recover:
        beginStop(true);
        break;
    case carlife::ProjectionWatchdog::ForceStop:
        kill(daemon, SIGKILL);
        break;
    case carlife::ProjectionWatchdog::VehicleBusy:
        projection = NeedsRecovery;
        break;
    case carlife::ProjectionWatchdog::Wait:
        break;
    }
    if (watchdog.restoreDue(time)) requestDisplayRestore();
}

void clearWifiText() {
    app->updateModel(WifiSsidModel, bmw::BasicString<char>(""));
    app->updateModel(WifiPassModel, bmw::BasicString<char>(""));
}

void showWifiState(unsigned state) {
    wifiState = state;
    if (wifiFocused) app->updateModel(WifiStatusModel, bmw::TextID(state));
}

void pollWifi() {
    if (wifiChild < 0) return;
    int result = 0;
    const pid_t ended = waitpid(wifiChild, &result, WNOHANG);
    if (ended != wifiChild && !(ended < 0 && errno == ECHILD)) return;
    wifiChild = -1;
    carlife::WifiKeyReply reply = {};
    const bool complete = read(wifiFd, &reply, sizeof(reply)) == sizeof(reply) &&
                          reply.state <= carlife::WifiRollbackFailed;
    close(wifiFd);
    wifiFd = -1;
    showWifiState(WifiResultBase + (complete ? reply.state : static_cast<unsigned>(carlife::WifiFailed)));
    if (wifiFocused && complete && reply.state <= carlife::WifiPending) {
        reply.ssid[sizeof(reply.ssid) - 1] = 0;
        reply.password[sizeof(reply.password) - 1] = 0;
        app->updateModel(WifiSsidModel, bmw::BasicString<char>(reply.ssid));
        app->updateModel(WifiPassModel, bmw::BasicString<char>(reply.password));
    }
    // Never put the reply in a log, file, or app-global cache.
}

void pollApp(void*) {
    if (!running || !app) return;
    const unsigned old = projection, oldPf = pfState;
    pollProjection();
    pollWifi();
    if (pfChild >= 0) {
        int result = 0;
        const pid_t ended = waitpid(pfChild, &result, WNOHANG);
        if (ended == pfChild || (ended < 0 && errno == ECHILD)) {
            pfChild = -1;
            const int code = ended > 0 && WIFEXITED(result) ? WEXITSTATUS(result) : carlife::PfError;
            switch (code) {
            case carlife::PfActive: pfState = PfInstalled; break;
            case carlife::PfInactive: pfState = PfSaved; break;
            case carlife::PfMissing: pfState = PfAbsent; break;
            case carlife::PfDisabled: pfState = PfOff; break;
            case carlife::PfDisabledAndMissing: pfState = PfOffAndAbsent; break;
            default: pfState = PfFailed; break;
            }
        }
    }
    if (old != projection || oldPf != pfState) update();
    app->post(reinterpret_cast<bmw::Runnable*>(&poller), 250);
}

bool start(void*, const bmw::Parcel&) {
    if (daemon >= 0 && projection == carlife::ChoosePhone && choices) {
        app->triggerEvent(ShowChoiceEvent);
        return true;
    }
    if (daemon >= 0 || restoreChild >= 0 || pfChild >= 0 || wifiChild >= 0) return true;
    int pipeFd[2];
    if (pipe(pipeFd) != 0) return false;
    if (fcntl(pipeFd[0], F_SETFL, O_NONBLOCK) < 0 || fcntl(pipeFd[1], F_SETFL, O_NONBLOCK) < 0) {
        close(pipeFd[0]);
        close(pipeFd[1]);
        return false;
    }
    daemon = launch("--auto", pipeFd[1]);
    close(pipeFd[1]);
    if (daemon < 0) {
        close(pipeFd[0]);
        projection = carlife::DisplayUnavailable;
    } else {
        readFd = pipeFd[0];
        watchdog.start(carlife::now());
        automaticRecovery = needsDisplayRestore = restoreAttempted = false;
        projection = carlife::Searching;
    }
    update();
    return true;
}

bool show(void*, const bmw::Parcel&) {
    if (daemon >= 0 && !watchdog.stopping() && projection == carlife::Hidden) {
        resume = true;
        kill(daemon, SIGUSR1);
    }
    return true;
}

bool stop(void*, const bmw::Parcel&) {
    if (daemon >= 0 && !watchdog.stopping()) {
        beginStop(false);
        update();
    }
    return true;
}

bool firewall(const char* option) {
    if (pfChild >= 0 || wifiChild >= 0) return true;
    if (daemon >= 0 && !strcmp(option, "--pf-install")) {
        pfState = PfStopFirst;
        update();
        return true;
    }
    pfChild = launch(option);
    pfState = pfChild >= 0 ? PfWorking : PfFailed;
    update();
    return true;
}
bool check(void*, const bmw::Parcel&) { return firewall("--pf-check"); }
bool install(void*, const bmw::Parcel&) { return firewall("--pf-install"); }

bool wifiJob(bool reset) {
    if (wifiChild >= 0) return true;
    clearWifiText();
    if (reset && (daemon >= 0 || pfChild >= 0)) {
        showWifiState(WifiResultBase + carlife::WifiBusy);
        return true;
    }
    int reply[2];
    if (pipe(reply) != 0) {
        showWifiState(WifiResultBase + carlife::WifiFailed);
        return false;
    }
    if (fcntl(reply[0], F_SETFL, O_NONBLOCK) < 0) {
        close(reply[0]);
        close(reply[1]);
        showWifiState(WifiResultBase + carlife::WifiFailed);
        return false;
    }
    wifiChild = reset ? launch("--wifi-reset", reply[1], "--confirm", "--menu")
                      : launch("--wifi-key", reply[1], "--menu");
    close(reply[1]);
    if (wifiChild < 0) {
        close(reply[0]);
        showWifiState(WifiResultBase + carlife::WifiFailed);
    } else {
        wifiFd = reply[0];
        showWifiState(reset ? WifiResetting : WifiReading);
    }
    return true;
}

bool wifiView(void*, const bmw::Parcel&) { return wifiJob(false); }

bool wifiReset(void*, const bmw::Parcel&) {
    app->triggerEvent(ShowWifiEvent);
    return wifiJob(true);
}

bool focusWifi(void*, const bmw::Parcel& payload) {
    payload.readBool(wifiFocused);
    if (wifiFocused) app->updateModel(WifiStatusModel, bmw::TextID(wifiState));
    else {
        clearWifiText();
        if (wifiChild < 0 && wifiState >= WifiResultBase && wifiState < WifiResultBase + carlife::WifiUnavailable)
            wifiState = WifiHidden;
    }
    return true;
}

bool focusMenu(void*, const bmw::Parcel& payload) {
    payload.readBool(menuFocused);
    return true;
}

bool focusBlank(void*, const bmw::Parcel& payload) {
    const bool wasFocused = blankFocused;
    payload.readBool(blankFocused);
    if (wasFocused && !blankFocused && daemon >= 0 && !watchdog.stopping())
        kill(daemon, SIGUSR2);
    return true;
}

bool focusChoice(void*, const bmw::Parcel& payload) {
    payload.readBool(choiceFocused);
    return true;
}

template<unsigned Index> bool choose(void*, const bmw::Parcel&) {
    if (daemon >= 0 && !watchdog.stopping() && projection == carlife::ChoosePhone && Index < choices) {
        union sigval selection;
        selection.sival_int = Index + 1;
        if (sigqueue(daemon, SIGUSR1, selection) == 0) {
            resume = true;
            projection = carlife::Connecting;
            update();
        }
    }
    return true;
}

bool init(bmw::PluginApp* self) {
    self->addHandler(30, bmw::Action{self, start});
    self->addHandler(31, bmw::Action{self, show});
    self->addHandler(32, bmw::Action{self, stop});
    self->addHandler(33, bmw::Action{self, check});
    self->addHandler(34, bmw::Action{self, install});
    self->addHandler(35, bmw::Action{self, choose<0>});
    self->addHandler(36, bmw::Action{self, choose<1>});
    self->addHandler(37, bmw::Action{self, choose<2>});
    self->addHandler(38, bmw::Action{self, wifiView});
    self->addHandler(39, bmw::Action{self, wifiReset});
    self->addHandler(MenuState, 1, bmw::Action{self, focusMenu});
    self->addHandler(BlankState, 1, bmw::Action{self, focusBlank});
    self->addHandler(ChoiceState, 1, bmw::Action{self, focusChoice});
    self->addHandler(WifiPage, 1, bmw::Action{self, focusWifi});
    running = self->onInit();
    if (running) {
        update();
        self->post(reinterpret_cast<bmw::Runnable*>(&poller), 250);
    }
    return running;
}

void finalize(bmw::PluginApp* self) {
    running = false;
    if (daemon >= 0) kill(daemon, SIGTERM);
    if (readFd >= 0) {
        close(readFd);
        readFd = -1;
    }
    if (wifiFd >= 0) {
        close(wifiFd);
        wifiFd = -1;
    }
    self->onFinalize();
}

bmw::PluginApp* create(bmw::IDispatcher& dispatcher, const char* name,
                       const bmw::Bundle& params, const bmw::Bundle& config) {
    if (app) return NULL;
    void* memory = ::operator new(sizeof(bmw::PluginApp));
    memset(memory, 0, sizeof(bmw::PluginApp));
    app = new (memory) bmw::PluginApp(dispatcher, bmw::BasicString<char>(name ? name : "NbtCarLife"),
                                      1, 0, bmw::Version1, params, config);
    void** vtable = *reinterpret_cast<void***>(app);
    memcpy(pluginVtable, vtable - 2, sizeof(pluginVtable));
    pluginVtable[2 + InitSlot] = reinterpret_cast<void*>(&init);
    pluginVtable[2 + FinalizeSlot] = reinterpret_cast<void*>(&finalize);
    *reinterpret_cast<void***>(app) = pluginVtable + 2;
    return app;
}

void destroy(void* object) {
    if (!object) return;
    void** vtable = *reinterpret_cast<void***>(object);
    reinterpret_cast<void (*)(void*)>(vtable[1])(object);
    app = NULL;
}

}

extern "C" {
struct CarLifePluginInfo {
    uint16_t major, minor;
    const char* name;
    const char* vendor;
    uint32_t interfaceVersion;
    bmw::PluginApp* (*create)(bmw::IDispatcher&, const char*, const bmw::Bundle&, const bmw::Bundle&);
    void (*destroy)(void*);
};
__attribute__((visibility("default"))) CarLifePluginInfo pluginAppInfo = {
    1, 0, "CarLife", "NBT CarLife", 1, create, destroy
};
}
