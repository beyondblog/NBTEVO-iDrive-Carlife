#include "carlife_session.hpp"
#include "discovery.hpp"
#include "display_commands.hpp"
#include "firewall.hpp"
#include "lifecycle.hpp"
#include "wifi_credentials.hpp"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

namespace {

struct Car : carlife::Sink {
    std::vector<carlife::Bytes> sent;
    unsigned version = 0, videoFrames = 0, pcmFrames = 0;
    bool active = false;

    bool send(const carlife::Bytes& packet) { sent.push_back(packet); return true; }
    void versionAccepted() { ++version; }
    bool prepareVideo(uint32_t w, uint32_t h, uint32_t fps) {
        return w == 848 && h == 480 && fps == 60;
    }
    bool video(carlife::Bytes&, uint32_t) { ++videoFrames; return true; }
    void audioState(unsigned lane, uint32_t rate, uint32_t channels, uint32_t bits, bool playing) {
        assert(lane == 0);
        if (playing) assert(rate == 48000 && channels == 2 && bits == 16);
        active = playing;
    }
    void audio(unsigned lane, const carlife::Bytes& pcm, uint32_t) {
        assert(active && lane == 0 && pcm.size() == 4);
        ++pcmFrames;
    }
};

void session() {
    Car car;
    carlife::Session s(car, 852, 480, 60, true, true);
    const uint8_t version[] = {0, 2, 0, 0, 0, 1, 0, 2, 8, 1};
    const uint8_t mode[] = {0, 8, 0, 0, 0, 1, 0, 8, 8, 0xd0, 6, 16, 0xe0, 3, 24, 60};
    const uint8_t h264[] = {0, 0, 0, 5, 0, 0, 0, 1, 0, 2, 0, 1, 0, 0, 0, 1, 0x65};
    assert(s.start());
    assert(s.receiveChannel(1, version, 5) && car.version == 0);
    assert(s.receiveChannel(1, version + 5, sizeof(version) - 5) && car.version == 1);
    assert(car.sent.size() == 4 && car.sent[1][15] == 3 && car.sent[2][15] == 0x52 && car.sent[3][15] == 7);
    const std::string features(car.sent[2].begin() + 16, car.sent[2].end());
    assert(features.find("CONNECT_TYPE") != std::string::npos);
    assert(features.find("MEDIA_SAMPLE_RATE") != std::string::npos);
    const uint8_t requestFeatures[] = {0, 0, 0, 0, 0, 1, 0, 0x51};
    assert(s.receiveChannel(1, requestFeatures, sizeof(requestFeatures)) && car.sent.back() == car.sent[2]);
    const uint8_t earlyMedia[] = {0, 0, 0, 8, 0, 0, 0, 0, 0, 3, 0, 1, 8, 0x80, 0xf7, 2, 16, 2, 24, 16};
    const uint8_t pcm[] = {0, 0, 0, 4, 0, 0, 0, 33, 0, 3, 0, 6, 0, 0, 0xff, 0x7f};
    assert(s.receiveChannel(3, earlyMedia, sizeof(earlyMedia)) && car.active);
    assert(s.receiveChannel(3, pcm, sizeof(pcm)) && !car.pcmFrames);
    assert(s.receiveChannel(1, mode, sizeof(mode)) && s.streaming());
    assert(s.receiveChannel(2, h264, sizeof(h264)) && car.videoFrames == 1);

    assert(s.touch(0, 420, 230) && car.sent.back()[16] == 8 && car.sent.back()[17] == 0);
    assert(s.releaseTouch() && car.sent.back()[17] == 1);
    assert(s.pause() && car.sent.back()[15] == 0x0a);

    const uint8_t stop[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 2};
    const uint8_t pause[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 3};
    const uint8_t resume[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 4};
    assert(s.receiveChannel(3, pcm, sizeof(pcm)) && car.pcmFrames == 1);
    assert(s.receiveChannel(3, pause, sizeof(pause)) && !car.active);
    assert(s.receiveChannel(3, pcm, sizeof(pcm)) && car.pcmFrames == 1);
    assert(s.receiveChannel(3, resume, sizeof(resume)) && car.active);
    assert(s.receiveChannel(3, pcm, sizeof(pcm)) && car.pcmFrames == 2);
    assert(s.receiveChannel(3, stop, sizeof(stop)) && !car.active);
    assert(s.receiveChannel(3, resume, sizeof(resume)) && !car.active);
}

void displayCommands() {
    FILE* output = carlife::displayCapture("echo ready");
    assert(output);
    char line[32];
    assert(fgets(line, sizeof(line), output) && std::string(line) == "ready\n");
    assert(fclose(output) == 0);
    output = carlife::displayCaptureProgram("/bin/echo", "literal;command");
    assert(output);
    assert(fgets(line, sizeof(line), output) && std::string(line) == "literal;command\n");
    assert(fclose(output) == 0);
    assert(carlife::displayRun("exit 7") == 7);
    const double start = carlife::now();
    assert(!carlife::displayCapture("sleep 20 & wait"));
    assert(carlife::now() - start < 3.0);
    assert(carlife::displayRun("exit 0") == 0);
}

void projectionRecovery() {
    using namespace carlife;
    ProjectionWatchdog watchdog;
    watchdog.start(0);
    watchdog.observe(WatchdogReady, 0);
    watchdog.observe(PreparingDisplay, 5);
    assert(watchdog.poll(26.9) == ProjectionWatchdog::Wait);
    assert(watchdog.poll(27) == ProjectionWatchdog::Recover);
    watchdog.stop(27);
    assert(!watchdog.restoreDue(30));
    assert(watchdog.poll(30) == ProjectionWatchdog::Wait);
    assert(watchdog.restoreDue(31));
    assert(watchdog.poll(31) == ProjectionWatchdog::ForceStop);
    assert(watchdog.poll(32) == ProjectionWatchdog::Wait);

    watchdog.start(40);
    watchdog.observe(WatchdogReady, 40);
    watchdog.observe(PreparingDisplay, 44);
    watchdog.observe(FactoryBorrowing, 47);
    watchdog.observe(Projecting, 50);
    watchdog.observe(Heartbeat, 56);
    assert(watchdog.poll(62.9) == ProjectionWatchdog::Wait);
    assert(watchdog.poll(63) == ProjectionWatchdog::Recover);
    watchdog.stop(63);
    assert(watchdog.restoreDue(67) && watchdog.poll(67) == ProjectionWatchdog::Wait);
    assert(watchdog.poll(73) == ProjectionWatchdog::VehicleBusy);
    watchdog.observe(FactoryRestored, 74);
    assert(watchdog.poll(74) == ProjectionWatchdog::ForceStop);

    watchdog.start(80);
    watchdog.observe(PreparingDisplay, 82);
    watchdog.stop(90);
    assert(watchdog.poll(100) == ProjectionWatchdog::VehicleBusy);  // Older daemon.

    watchdog.start(110);
    watchdog.observe(WatchdogReady, 110);
    watchdog.observe(ApplyingWifiKey, 111);
    watchdog.stop(112);
    assert(watchdog.poll(122) == ProjectionWatchdog::VehicleBusy);
    watchdog.observe(Searching, 123);
    assert(watchdog.poll(123) == ProjectionWatchdog::ForceStop);
}

void discovery() {
    const char* arp = "? (172.16.222.101) at aa on wfd0\n"
                      "? (172.16.222.105) at bb on wfd0\n"
                      "? (172.16.222.101) at aa on wfd0\n"
                      "? (172.16.222.106) at (incomplete) on wfd0\n"
                      "? (172.16.222.108) at dd on wfd01\n"
                      "? (172.16.222.112) at ee on en0\n"
                      "? (192.168.1.3) at ff on wfd0\n";
    const std::vector<std::string> peers = carlife::wfdPeers(arp, "172.16.222.105");
    assert(peers.size() == 2 && peers[0] == "172.16.222.105" && peers[1] == "172.16.222.101");
    assert(!carlife::isWfdPeer("172.16.222.1") && !carlife::isWfdPeer("172.16.222.255"));
    assert(carlife::SearchSeconds == 60 && carlife::HandshakeSeconds == 15);
}

void firewall() {
    const std::string stock = "block drop all\npass quick on wfd0 proto tcp to any port 5277 keep state\n";
    const std::string old = stock + "# nbt-carlife: outbound CarLife to the vivo phone over Wi-Fi Direct\n"
                            "pass out quick on wfd0 inet proto tcp from 172.16.222.1 to 172.16.222.101 "
                            "port { 7240, 8240, 9240, 9241, 9242, 9340 } keep state label \"nbt-carlife\"\n";
    std::string updated, again;
    bool present = true;
    assert(carlife::prepareFirewallConfig(old, updated, present) && !present);
    assert(updated.compare(0, stock.size(), stock) == 0);
    assert(updated.find("172.16.222.101") == std::string::npos);
    assert(updated.find("to 172.16.222.0/24 port { 7240, 8240, 9240, 9241, 9242, 9340 }") != std::string::npos);
    assert(carlife::prepareFirewallConfig(updated, again, present) && present && again == updated);
    assert(!carlife::prepareFirewallConfig("pass in quick all label \"nbt-carlife\"\n", again, present));
}

void wifiPassword() {
    assert(carlife::validWifiGroup("DIRECT-03-BMW12345"));
    assert(!carlife::validWifiGroup("DIRECT-03-BMW'12345"));
    assert(!carlife::validWifiGroup("HOME-NETWORK"));
    assert(!carlife::validWifiGroup(std::string("DIRECT-03-") + std::string(24, 'A')));
    int entropy[2];
    assert(pipe(entropy) == 0);
    const unsigned char sample[] = {255, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    assert(write(entropy[1], sample, sizeof(sample)) == sizeof(sample));
    close(entropy[1]);
    assert(carlife::makeWifiPassword(entropy[0]) == "abcdefghijkmnpqr");
    assert(carlife::makeWifiPassword(entropy[0]).empty());
    close(entropy[0]);
}

}

int main() {
    session();
    displayCommands();
    projectionRecovery();
    discovery();
    firewall();
    wifiPassword();
    puts("PASS: version, native video, display timeout and recovery, touch release, PCM, Wi-Fi discovery, PF migration and Wi-Fi password generation.");
}
