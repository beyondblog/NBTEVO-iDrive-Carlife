#include "firewall.hpp"
#include <errno.h>
#include <fcntl.h>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace carlife {
namespace {

const char* const PfFile = "/fs/sda0/etc/pf.conf";
const char* const Prefix = "pass out quick on wfd0 inet proto tcp from 172.16.222.1 to ";
const char* const Subnet = "172.16.222.0/24";
const char* const Ports = " port { 7240, 8240, 9240, 9241, 9242, 9340 } keep state label \"nbt-carlife\"";
const char* const Label = "label \"nbt-carlife\"";
const char* const Marker = "# nbt-carlife: outbound CarLife over Wi-Fi Direct";
const char* const OldMarker = "# nbt-carlife: outbound CarLife to the vivo phone over Wi-Fi Direct";
const char* const StateDir = "/var/opt/nbt-carlife";

bool capture(const std::string& command, std::string& output) {
    output.clear();
    FILE* file = popen(command.c_str(), "r");
    if (!file) return false;
    char buf[1024];
    bool valid = true;
    while (fgets(buf, sizeof(buf), file)) {
        if (output.size() > 256 * 1024) valid = false;
        else output += buf;
    }
    return pclose(file) == 0 && valid;
}

int openConfig(const char* path, int flags) {
    struct stat before, opened;
    if (lstat(path, &before) != 0 || !S_ISREG(before.st_mode)) return -1;
    const int fd = open(path, flags);
    if (fd >= 0 && (fstat(fd, &opened) != 0 || before.st_ino != opened.st_ino ||
                    before.st_dev != opened.st_dev)) {
        close(fd);
        return -1;
    }
    return fd;
}

bool readFile(const char* path, std::string& result) {
    const int fd = openConfig(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat metadata;
    if (fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_size > 256 * 1024) {
        close(fd);
        return false;
    }
    char bytes[1024];
    result.clear();
    ssize_t n;
    while ((n = read(fd, bytes, sizeof(bytes))) > 0 && result.size() <= 256 * 1024) result.append(bytes, n);
    close(fd);
    return n == 0 && result.size() <= 256 * 1024;
}

bool writeAll(int fd, const std::string& bytes) {
    size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t n = write(fd, bytes.data() + offset, bytes.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        offset += n;
    }
    return fsync(fd) == 0;
}

bool overwrite(const std::string& bytes) {
    const int fd = openConfig(PfFile, O_WRONLY);
    if (fd < 0) return false;
    const bool ok = ftruncate(fd, 0) == 0 && writeAll(fd, bytes);
    const bool closed = close(fd) == 0;
    return ok && closed;
}

std::string withoutCarlife(const std::string& rules) {
    std::istringstream lines(rules);
    std::string line, result;
    while (std::getline(lines, line)) {
        if (line.find(Label) == std::string::npos && line.find(" = \"") == std::string::npos &&
            line.find("carip = ") != 0 && line.find("igmp = ") != 0 && !line.empty())
            result += line + "\n";
    }
    return result;
}

}

bool prepareFirewallConfig(const std::string& current, std::string& candidate, bool& present) {
    std::istringstream lines(current);
    const std::string desired = std::string(Prefix) + Subnet + Ports;
    std::string line;
    candidate.clear();
    present = false;
    unsigned managed = 0;
    while (std::getline(lines, line)) {
        if (line == Marker || line == OldMarker) continue;
        if (line.find(Label) != std::string::npos) {
            if (++managed > 1) return false;
            const size_t start = strlen(Prefix);
            const size_t suffix = line.find(Ports);
            if (line.compare(0, start, Prefix) || suffix == std::string::npos ||
                line.size() != suffix + strlen(Ports) || suffix < start) return false;
            const std::string dest = line.substr(start, suffix - start);
            if (dest != Subnet) {
                unsigned peer;
                char extra;
                if (sscanf(dest.c_str(), "172.16.222.%u%c", &peer, &extra) != 1 ||
                    peer < 2 || peer > 254) return false;
            }
            present = line == desired;
            continue;
        }
        candidate += line + "\n";
    }
    candidate += std::string(Marker) + "\n" + desired + "\n";
    return true;
}

namespace {
bool liveRule(const std::string& rules) {
    const unsigned ports[] = {7240, 8240, 9240, 9241, 9242, 9340};
    for (size_t i = 0; i < sizeof(ports) / sizeof(*ports); ++i) {
        char expected[180];
        snprintf(expected, sizeof(expected), "%s%s port = %u keep state %s\n", Prefix, Subnet, ports[i], Label);
        if (rules.find(expected) == std::string::npos) return false;
    }
    return true;
}

int inspect(bool present, bool enabled, const std::string& rules) {
    if (!enabled && !present) {
        puts("PF is disabled and the CarLife subnet rule is missing.");
        return PfDisabledAndMissing;
    }
    if (!present) { puts("CarLife subnet rule missing."); return PfMissing; }
    if (!enabled) { puts("CarLife rule saved. PF is disabled."); return PfDisabled; }
    if (!liveRule(rules)) { puts("CarLife rule saved but not active."); return PfInactive; }
    puts("CarLife Wi-Fi Direct firewall rules active (six TCP ports).");
    return PfActive;
}

bool privateStateDir() {
    struct stat dir;
    return lstat(StateDir, &dir) == 0 && S_ISDIR(dir.st_mode) &&
           dir.st_uid == 0 && (dir.st_mode & 077) == 0;
}

}

int firewall(bool install) {
    std::string original, candidate, live, info, nat, anchors, scratch;
    bool present;
    if (!readFile(PfFile, original) || !prepareFirewallConfig(original, candidate, present) ||
        !capture("/usr/bin/pfctl -s info", info) || !capture("/usr/bin/pfctl -s rules", live)) {
        fputs("Could not read PF configuration or identify the managed rule.\n", stderr);
        return PfError;
    }
    const bool enabled = info.find("Status: Enabled") != std::string::npos;
    if (!install || (present && liveRule(live))) return inspect(present, enabled, live);
    if (geteuid() != 0 || !privateStateDir()) {
        fputs("Install CarLife into its private data directory before editing PF.\n", stderr);
        return PfError;
    }
    const int lock = open("/var/opt/nbt-carlife/pf.lock", O_CREAT | O_WRONLY, 0600);
    struct flock exclusive = {};
    exclusive.l_type = F_WRLCK;
    exclusive.l_whence = SEEK_SET;
    if (lock < 0 || fcntl(lock, F_SETLK, &exclusive) != 0) {
        if (lock >= 0) close(lock);
        return PfError;
    }
    char backup[] = "/var/opt/nbt-carlife/pf.before.XXXXXX";
    char staged[] = "/var/opt/nbt-carlife/pf.staged.XXXXXX";
    umask(077);
    int previous = mkstemp(backup), next = mkstemp(staged);
    bool ok = previous >= 0 && next >= 0 && writeAll(previous, original) && writeAll(next, candidate);
    if (previous >= 0) close(previous);
    if (next >= 0) close(next);
    const std::string validate = "/usr/bin/pfctl -n -f ";
    const std::string parse = "/usr/bin/pfctl -n -v -R -f ";
    const std::string activate = "/usr/bin/pfctl -R -f ";
    std::string originalParsed, expected, installed;
    struct statvfs volume;
    ok = ok && capture(validate + staged, scratch) &&
         capture(parse + backup, originalParsed) && capture(parse + staged, expected) &&
         liveRule(expected) && withoutCarlife(originalParsed) == withoutCarlife(live) &&
         withoutCarlife(expected) == withoutCarlife(live) &&
         capture("/usr/bin/pfctl -s nat", nat) && capture("/usr/bin/pfctl -v -s Anchors", anchors) &&
         readFile(PfFile, scratch) && scratch == original &&
         capture("/usr/bin/pfctl -s rules", scratch) && scratch == live &&
         statvfs("/fs/sda0", &volume) == 0;
    bool remounted = false, changed = false;
    if (ok && !present && (volume.f_flag & ST_RDONLY)) {
        remounted = capture("/usr/bin/mount -uw /fs/sda0", scratch);
        ok = remounted;
    }
    if (ok && !present) {
        changed = true;
        ok = overwrite(candidate) && readFile(PfFile, scratch) && scratch == candidate;
    }
    const bool reload = ok;
    if (ok) {
        ok = capture(validate + PfFile, scratch) && capture(activate + PfFile, scratch) &&
             capture("/usr/bin/pfctl -s rules", installed) && liveRule(installed) &&
             withoutCarlife(installed) == withoutCarlife(live) &&
             capture("/usr/bin/pfctl -s nat", scratch) && scratch == nat &&
             capture("/usr/bin/pfctl -v -s Anchors", scratch) && scratch == anchors &&
             capture("/usr/bin/pfctl -s info", scratch) &&
             (scratch.find("Status: Enabled") != std::string::npos) == enabled;
    }
    if (!ok && changed && !overwrite(original))
        fputs("Could not restore pf.conf from backup.\n", stderr);
    if (!ok && reload && !capture(activate + backup, scratch))
        fputs("Could not restore active PF rules from backup.\n", stderr);
    if (remounted && !capture("/usr/bin/mount -ur /fs/sda0", scratch)) {
        fputs("Could not restore the read-only mount.\n", stderr);
        if (ok && (!overwrite(original) || !capture(activate + backup, scratch)))
            fputs("Could not roll back the firewall after remount failure.\n", stderr);
        capture("/usr/bin/mount -ur /fs/sda0", scratch);
        ok = false;
    }
    if (next >= 0) unlink(staged);
    if (previous >= 0) printf("PF backup: %s\n", backup);
    close(lock);
    if (!ok) {
        fputs("CarLife PF update failed. Check the configuration and backup.\n", stderr);
        return PfError;
    }
    return inspect(true, enabled, installed);
}
}
