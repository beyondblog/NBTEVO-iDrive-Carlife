#ifndef CARLIFE_DISCOVERY_HPP
#define CARLIFE_DISCOVERY_HPP

#include <string>
#include <vector>

namespace carlife {

bool isWfdPeer(const std::string& address);
std::vector<std::string> wfdPeers(const std::string& neighbors, const std::string& preferred);
bool wfdReady();
std::vector<std::string> discoverWfdPeers(size_t& associated);
void rememberWfdPeer(const std::string& address);

}
#endif
