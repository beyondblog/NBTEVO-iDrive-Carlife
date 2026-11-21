#include "wifi_credentials.hpp"
#include "renderer_guard.hpp"
#include "wfd_own.hpp"
#include "gcf_client.hpp"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

namespace carlife {
namespace {

const char* const StateDir = "/var/opt/nbt-carlife";
const char* const SavedKey = "/var/opt/nbt-carlife/wfd-passphrase";
const char* const AppliedKey = "/dev/shmem/nbt-carlife-wfd-applied";
const char* const Group = "interfaceName='wfd0'";

bool privateDir() {
    struct stat directory;
    return lstat(StateDir, &directory) == 0 && S_ISDIR(directory.st_mode) &&
           directory.st_uid == 0 && (directory.st_mode & 077) == 0;
}

bool savedKey(std::string& secret) {
    struct stat file;
    if (!privateDir() || lstat(SavedKey, &file) != 0 || !S_ISREG(file.st_mode) ||
        file.st_uid != 0 || (file.st_mode & 077) || file.st_size > 64) return false;
    std::string text;
    return nbt::readWholeFile(SavedKey, text) && nbt::wfdOwnPassFromText(text, secret);
}

std::string keyGeneration() {
    struct stat file;
    if (lstat(SavedKey, &file) != 0 || !S_ISREG(file.st_mode)) return "";
    char id[112];
    snprintf(id, sizeof(id), "%llu:%llu:%llu\n",
             (unsigned long long)file.st_dev, (unsigned long long)file.st_ino,
             (unsigned long long)file.st_mtime);
    return id;
}

bool appliedThisBoot() {
    const std::string expected = keyGeneration();
    struct stat file;
    if (expected.empty() || lstat(AppliedKey, &file) != 0 || !S_ISREG(file.st_mode) ||
        file.st_uid != 0 || (file.st_mode & 077) || file.st_size != static_cast<off_t>(expected.size())) return false;
    std::string marker;
    return nbt::readWholeFile(AppliedKey, marker) && marker == expected;
}

void recordApplied() {
    const std::string id = keyGeneration();
    if (id.empty()) return;
    // /dev/shmem on NBT Evo is flat. Exclusive creation also prevents following a symlink.
    unlink(AppliedKey);
    const int fd = open(AppliedKey, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return;
    const bool saved = write(fd, id.data(), id.size()) == static_cast<ssize_t>(id.size());
    close(fd);
    if (!saved) unlink(AppliedKey);
}

bool factoryKey(std::string& key) {
    nbt::ApInfo factory;
    std::string path;
    if (!nbt::loadWfdAp(nbt::wfdPassPaths(), nbt::wfdPassPathCount(), factory, path, true)) return false;
    key = factory.key;
    return true;
}

WifiKeyReply reply(WifiKeyState state, const std::string& ssid = "", const std::string& password = "") {
    WifiKeyReply value = {};
    value.state = state;
    snprintf(value.ssid, sizeof(value.ssid), "%s", ssid.c_str());
    snprintf(value.password, sizeof(value.password), "%s", password.c_str());
    return value;
}

bool projectionIdle() {
    DIR* contexts = opendir("/pps/services/multimedia/renderer/context");
    if (!contexts) return false;
    bool idle = true, carplay = false;
    while (struct dirent* entry = readdir(contexts)) {
        const char* name = entry->d_name;
        if (!strncmp(name, "aadvideoctx", 11) || !strncmp(name, "clvideoctx", 10)) idle = false;
        if (!strncmp(name, "cpvideoctx", 10)) carplay = true;
    }
    closedir(contexts);
    return idle && (!carplay || factoryProjectionHidden());
}

int lockChanges() {
    if (geteuid() != 0 || !privateDir()) return -1;
    const int fd = open("/var/opt/nbt-carlife/wfd.lock", O_WRONLY | O_CREAT, 0600);
    if (fd < 0) return -1;
    struct flock lock = {};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &lock) == 0) return fd;
    close(fd);
    return -1;
}

int isEnabled(nbt::GcfClient& client) {
    const std::string result = client.call("Wlan_ConnMngr_WFD_isWFDEnable", Group, 3);
    std::string on;
    if (nbt::gcfResult(result) != 0 || !nbt::gcfField(result, "wfd_on", on)) return -1;
    return on == "1" ? 1 : on == "0" ? 0 : -1;
}

bool enable(nbt::GcfClient& client, const std::string& ssid, const std::string& device,
            const std::string& password) {
    const std::string params = std::string(Group) + " device_name='" + device + "' ssid='" + ssid +
        "' number_of_sec_devices=2 secondary_device_type={ 6, 4 } passphrase='" + password + "'";
    if (nbt::gcfResult(client.call("Wlan_ConnMngr_WFD_enable", params, 12)) != 0) return false;
    for (unsigned i = 0; i < 3; ++i) {
        if (isEnabled(client) == 1) return true;
        sleep(1);
    }
    return false;
}

bool disable(nbt::GcfClient& client) {
    const int result = nbt::gcfResult(client.call("Wlan_ConnMngr_WFD_disable", Group, 6));
    if (result == 0 || result == 7) {
        sleep(1);
        return true;
    }
    return false;
}

WifiKeyReply apply(bool reapply) {
    std::string oldKey, override, password;
    if (!projectionIdle()) return reply(WifiBusy);
    if (!factoryKey(oldKey) || !nbt::wfdOwnPassUsable(oldKey)) return reply(WifiUnavailable);
    const bool haveOverride = savedKey(override);
    const bool previouslyApplied = haveOverride && appliedThisBoot();
    if (reapply && (!haveOverride || previouslyApplied)) return wifiCredentials();
    if (previouslyApplied) oldKey = override;
    if (reapply) {
        password = override;
    } else {
        const int random = open("/dev/urandom", O_RDONLY);
        if (random < 0) return reply(WifiFailed);
        for (unsigned attempt = 0; attempt < 3 && (password.empty() || password == oldKey); ++attempt)
            password = makeWifiPassword(random);
        close(random);
        if (password.size() != 16 || password == oldKey) return reply(WifiFailed);
    }

    nbt::GcfClient client;
    std::string reason, ssid, device;
    if (!client.open("nbtcarlife_wfd", reason)) return reply(WifiFailed);
    const std::string stored = client.call("Wlan_ConnMngr_WFD_getPersistedGroupList", Group, 5);
    if (!nbt::wfdPersistedGoSsid(stored, ssid) && !nbt::resolveWfdSsid("wfd0", ssid))
        return reply(WifiUnavailable);
    if (!validWifiGroup(ssid) || !nbt::wfdDeviceNameFromSsid(ssid, device) ||
        isEnabled(client) != 1 || !projectionIdle()) return reply(WifiUnavailable);

    char stage[] = "/var/opt/nbt-carlife/wfd-next.XXXXXX";
    if (!reapply) {
        const int fd = mkstemp(stage);
        if (fd < 0) return reply(WifiFailed);
        const bool staged = write(fd, password.data(), password.size()) ==
                            static_cast<ssize_t>(password.size()) && fsync(fd) == 0;
        const bool closed = close(fd) == 0;
        if (!staged || !closed) { unlink(stage); return reply(WifiFailed); }
    }
    const bool off = disable(client);
    const bool changed = off && enable(client, ssid, device, password);
    if (changed && (reapply || rename(stage, SavedKey) == 0)) {
        recordApplied();
        return reply(WifiApplied, ssid, password);
    }
    if (!reapply) unlink(stage);
    // Even an unanswered disable can have changed the group. Restore the known
    // previous key whenever its state cannot be established.
    if (!off && isEnabled(client) == 1) return reply(WifiFailed);
    if (changed || isEnabled(client) != 0) disable(client);
    if (enable(client, ssid, device, oldKey)) {
        if (previouslyApplied) recordApplied();
        else unlink(AppliedKey);
        return reply(WifiFailed);
    }
    unlink(AppliedKey);
    return reply(WifiRollbackFailed);
}

}

WifiKeyReply wifiCredentials() {
    std::string ssid, secret;
    nbt::resolveWfdSsid("wfd0", ssid);
    if (savedKey(secret)) return reply(appliedThisBoot() ? WifiApplied : WifiPending, ssid, secret);
    if (factoryKey(secret)) return reply(WifiFactory, ssid, secret);
    return reply(WifiUnavailable, ssid);
}

bool savedWifiPasswordNeedsApply() {
    std::string secret;
    return savedKey(secret) && !appliedThisBoot();
}

WifiKeyReply changeWifiPassword(bool reapply) {
    const int lock = lockChanges();
    if (lock < 0) return reply(WifiBusy);
    const WifiKeyReply result = apply(reapply);
    close(lock);
    return result;
}

}
