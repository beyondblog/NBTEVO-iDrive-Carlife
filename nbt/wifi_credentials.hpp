#ifndef CARLIFE_WIFI_CREDENTIALS_HPP
#define CARLIFE_WIFI_CREDENTIALS_HPP

#include <string>

namespace carlife {

enum WifiKeyState {
    WifiFactory = 0, WifiApplied, WifiPending, WifiUnavailable, WifiBusy,
    WifiFailed, WifiRollbackFailed
};

struct WifiKeyReply {
    unsigned char state;
    char ssid[33];
    char password[64];
};

WifiKeyReply wifiCredentials();
WifiKeyReply changeWifiPassword(bool reapply);
bool savedWifiPasswordNeedsApply();
std::string makeWifiPassword(int randomFd);
bool validWifiGroup(const std::string& ssid);

}
#endif
