#include "wifi_credentials.hpp"
#include <errno.h>
#include <string.h>
#include <unistd.h>

namespace carlife {

bool validWifiGroup(const std::string& ssid) {
    return ssid.size() >= 11 && ssid.size() <= 32 && ssid.compare(0, 7, "DIRECT-") == 0 &&
           ssid[9] == '-' &&
           ssid.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") == std::string::npos;
}

std::string makeWifiPassword(int randomFd) {
    // Unambiguous characters for entering a password manually from the car screen.
    const char alphabet[] = "abcdefghijkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    const unsigned length = sizeof(alphabet) - 1;
    const unsigned limit = 256 - 256 % length;
    std::string secret;
    for (unsigned i = 0; i < 128 && secret.size() < 16; ++i) {
        unsigned char byte;
        const ssize_t result = read(randomFd, &byte, 1);
        if (result < 0 && errno == EINTR) continue;
        if (result != 1) return "";
        if (byte < limit) secret += alphabet[byte % length];
    }
    return secret.size() == 16 ? secret : "";
}

}
