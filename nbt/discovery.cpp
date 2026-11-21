#include "discovery.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace carlife {
namespace {
const char* const PeerFile = "/var/opt/nbt-carlife/last-peer";
const char* const WfdAddress = "172.16.222.1";
bool privateDir() {
    struct stat dir;
    return lstat("/var/opt/nbt-carlife", &dir) == 0 && S_ISDIR(dir.st_mode) &&
           dir.st_uid == geteuid() && (dir.st_mode & 077) == 0;
}
}

bool isWfdPeer(const std::string& address) {
    struct in_addr ip;
    if (inet_pton(AF_INET, address.c_str(), &ip) != 1) return false;
    const uint32_t host = ntohl(ip.s_addr);
    return (host & 0xffffff00u) == 0xac10de00u && (host & 255u) > 1 && (host & 255u) < 255;
}

std::vector<std::string> wfdPeers(const std::string& neighbors, const std::string& preferred) {
    std::vector<std::string> peers;
    for (size_t p = 0; p < neighbors.size();) {
        const size_t end = neighbors.find('\n', p);
        const std::string line = neighbors.substr(p, end == std::string::npos ? end : end - p);
        p = end == std::string::npos ? neighbors.size() : end + 1;
        if (line.find(" on wfd0") == std::string::npos || line.find("(incomplete)") != std::string::npos)
            continue;
        const size_t start = line.find('(');
        const size_t close = line.find(')', start);
        if (start == std::string::npos || close == std::string::npos) continue;
        const size_t iface = line.find(" on wfd0") + strlen(" on wfd0");
        if (iface < line.size() && line[iface] != ' ' && line[iface] != '\r') continue;
        const std::string ip = line.substr(start + 1, close - start - 1);
        if (isWfdPeer(ip) && std::find(peers.begin(), peers.end(), ip) == peers.end())
            peers.push_back(ip);
    }
    const std::vector<std::string>::iterator last = std::find(peers.begin(), peers.end(), preferred);
    if (last != peers.end()) std::rotate(peers.begin(), last, last + 1);
    return peers;
}

bool wfdReady() {
    struct ifaddrs* interfaces = NULL;
    if (getifaddrs(&interfaces) != 0) return false;
    bool ready = false;
    for (const struct ifaddrs* i = interfaces; i; i = i->ifa_next) {
        if (!i->ifa_addr || !i->ifa_netmask || i->ifa_addr->sa_family != AF_INET ||
            strcmp(i->ifa_name, "wfd0") || !(i->ifa_flags & IFF_UP)) continue;
        const struct sockaddr_in* addr = reinterpret_cast<const struct sockaddr_in*>(i->ifa_addr);
        const struct sockaddr_in* mask = reinterpret_cast<const struct sockaddr_in*>(i->ifa_netmask);
        ready = addr->sin_addr.s_addr == inet_addr(WfdAddress) && ntohl(mask->sin_addr.s_addr) == 0xffffff00u;
        if (ready) break;
    }
    freeifaddrs(interfaces);
    return ready;
}

std::vector<std::string> discoverWfdPeers(size_t& associated) {
    associated = 0;
    if (!wfdReady()) return std::vector<std::string>();
    std::string arp, preferred;
    FILE* pipe = popen("LC_ALL=C arp -an 2>/dev/null", "r");
    char line[512];
    if (pipe) {
        while (fgets(line, sizeof(line), pipe)) {
            if (arp.size() < 65536) arp += line;
        }
        pclose(pipe);
    }
    struct stat saved;
    const int fd = privateDir() && lstat(PeerFile, &saved) == 0 && S_ISREG(saved.st_mode)
                 ? open(PeerFile, O_RDONLY) : -1;
    if (fd >= 0) {
        const ssize_t bytes = read(fd, line, 16);
        close(fd);
        if (bytes > 0) preferred.assign(line, bytes);
    }
    std::vector<std::string> peers = wfdPeers(arp, preferred);
    associated = peers.size();
    // New clients can have a DHCP lease before emitting traffic visible to ARP.
    // The factory wfd0 pool is .100 through .110. Probe only CarLife's command port there.
    if (peers.empty() && isWfdPeer(preferred)) peers.push_back(preferred);
    for (unsigned i = 100; i <= 110; ++i) {
        snprintf(line, sizeof(line), "172.16.222.%u", i);
        if (std::find(peers.begin(), peers.end(), line) == peers.end()) peers.push_back(line);
    }
    return peers;
}

void rememberWfdPeer(const std::string& address) {
    if (!isWfdPeer(address) || !privateDir()) return;
    char temporary[] = "/var/opt/nbt-carlife/last-peer.XXXXXX";
    const int fd = mkstemp(temporary);
    if (fd < 0) return;
    const ssize_t result = write(fd, address.data(), address.size());
    close(fd);
    if (result != static_cast<ssize_t>(address.size()) || rename(temporary, PeerFile) != 0)
        unlink(temporary);
}

}
