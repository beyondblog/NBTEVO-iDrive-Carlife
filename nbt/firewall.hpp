#ifndef CARLIFE_FIREWALL_HPP
#define CARLIFE_FIREWALL_HPP

#include <string>

namespace carlife {
enum PfStatus {
    PfActive = 0, PfError = 1, PfInactive = 10, PfMissing = 11,
    PfDisabled = 12, PfDisabledAndMissing = 13
};
bool prepareFirewallConfig(const std::string& current, std::string& candidate, bool& present);
int firewall(bool install);
}
#endif
