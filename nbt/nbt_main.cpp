#include "carlife_session.hpp"
#include "discovery.hpp"
#include "firewall.hpp"
#include "lifecycle.hpp"
#include "wifi_transport.hpp"
#include "wifi_credentials.hpp"
#include "renderer_guard.hpp"

#include "car_config.hpp"
#include "car/ccs_input.hpp"
#include "car/volume_input.hpp"
#include "aa_keys.hpp"
#include "wfd_own.hpp"
#include "touch_probe.hpp"
#include "usb_aoap.hpp"
#include "usb_direct.hpp"
#include "video_manager.hpp"
#include "volume.hpp"
#include "audio/browser_audio.hpp"

#include <deque>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/slog.h>
#include <unistd.h>

extern "C" int screen_get_display_property_iv(screen_display_t display, int property, int* values);

int carlife::nativeVideoWidth = 852;

namespace {

volatile sig_atomic_t stopping = 0;
enum VisibilityRequest { ShowProjection = 1, HideProjection = 2 };
volatile sig_atomic_t visibility_request = 0;
volatile sig_atomic_t peer_choice = 0;
bool managed = false;
const char* executablePath = NULL;
const size_t RxLimit = 8 * 1024 * 1024;

void stop(int) { stopping = 1; }
void show(int, siginfo_t* signal, void*) {
    if (signal && signal->si_code == SI_QUEUE) peer_choice = signal->si_value.sival_int;
    else visibility_request = ShowProjection;
}
void hide(int) { visibility_request = HideProjection; }

bool sendStatus(const carlife::StatusEvent& event) {
    return !managed || write(carlife::StatusFd, &event, sizeof(event)) == sizeof(event);
}

bool status(carlife::Status state) {
    if (managed && (state == carlife::PreparingDisplay || state == carlife::Projecting ||
                    state == carlife::Hidden || state == carlife::DisplayUnavailable))
        slogf(0, _SLOG_INFO, "CarLife display state %d", static_cast<int>(state));
    const carlife::StatusEvent event = {static_cast<unsigned char>(state), 0, {0, 0, 0}};
    return sendStatus(event);
}

int printPanelSize() {
    // QNX Screen 6.5 display property SIZE. Use the same first display as VideoManager.
    const int displaySizeProperty = 40;
    screen_context_t screen = NULL;
    if (screen_create_context(&screen, SCREEN_APPLICATION_CONTEXT) != 0) return 1;
    int count = 0, size[2] = {};
    if (screen_get_context_property_iv(screen, SCREEN_PROPERTY_DISPLAY_COUNT, &count) == 0 && count > 0) {
        std::vector<screen_display_t> displays(count);
        if (screen_get_context_property_pv(screen, SCREEN_PROPERTY_DISPLAYS, &displays[0]) == 0 &&
            screen_get_display_property_iv(displays[0], displaySizeProperty, size) != 0)
            size[0] = size[1] = 0;
    }
    screen_destroy_context(screen);
    if (!nbt::carcfg_detail::sane(size[0], size[1])) return 1;
    printf("%dx%d\n", size[0], size[1]);
    return 0;
}

bool printWifiDirectInfo() {
    const carlife::WifiKeyReply info = carlife::wifiCredentials();
    if (info.state == carlife::WifiApplied || info.state == carlife::WifiPending) {
        printf("CarLife saved Wi-Fi Direct key (%s): %s\n",
               info.state == carlife::WifiApplied ? "last applied this boot" : "apply when CarLife starts",
               info.password);
    }
    nbt::ApInfo factory;
    std::string source, ssid;
    if (nbt::resolveWfdSsid("wfd0", ssid))
        printf("Wi-Fi Direct SSID: %s\n", ssid.c_str());
    else
        puts("Wi-Fi Direct SSID unknown; look for the car's DIRECT network.");

    const bool have_factory = nbt::loadWfdAp(
        nbt::wfdPassPaths(), nbt::wfdPassPathCount(), factory, source, true);
    if (have_factory)
        printf("BMW stored WPA2 password (%s): %s\n", source.c_str(), factory.key.c_str());
    else
        puts("BMW stored WPA2 password unavailable (no usable WFDPass.conf).");

    const char* supra_paths[] = {
        NBT_HAND_DATA_DIR "/wfd-own-pass",
        NBT_PKG_DATA_DIR "/wfd-own-pass",
    };
    bool have_supra = false;
    for (size_t i = 0; i < sizeof(supra_paths) / sizeof(supra_paths[0]); ++i) {
        std::string text, pass;
        if (!nbt::readWholeFile(supra_paths[i], text) || !nbt::wfdOwnPassFromText(text, pass))
            continue;
        printf("Supra saved WPA2 password (%s; active only if applied): %s\n",
               supra_paths[i], pass.c_str());
        have_supra = true;
    }
    fflush(stdout);
    return have_factory || have_supra || info.password[0];
}

int wifiKeyCommand(bool reset, bool reapply, bool forMenu) {
    const carlife::WifiKeyReply info = reset || reapply ? carlife::changeWifiPassword(reapply)
                                                       : carlife::wifiCredentials();
    if (forMenu) {
        if (write(carlife::StatusFd, &info, sizeof(info)) != sizeof(info)) return 1;
    } else if (reset || reapply) {
        switch (info.state) {
        case carlife::WifiApplied:
            puts("CarLife Wi-Fi key applied. Rejoin Wi-Fi Direct using the key from the password menu.");
            break;
        case carlife::WifiBusy:
            puts("Stop projection and run on hu-omap as root with a private CarLife install.");
            break;
        case carlife::WifiRollbackFailed:
            fputs("Could not restart the previous Wi-Fi Direct group. Check it after the head unit restarts.\n", stderr);
            break;
        default:
            fputs("CarLife Wi-Fi key was not changed.\n", stderr);
            break;
        }
    }
    return info.state <= carlife::WifiPending ? 0 : 1;
}

using carlife::now;

struct Touch {
    unsigned action, x, y;
};

struct Inbox {
    Inbox() : rx_size(0), back_down(0), volume(-1), ended(false), visible(false), touch_owned(false), contact(false), hide(false) {}
    std::deque<carlife::Bytes> rx;
    std::deque<Touch> touch;
    std::deque<unsigned> keys;
    size_t rx_size;
    double back_down;
    int volume;
    bool ended, visible, touch_owned, contact, hide;
};

pthread_mutex_t inbox_lock = PTHREAD_MUTEX_INITIALIZER;
Inbox inbox;

void onVolume(int level, int max, void*) {
    pthread_mutex_lock(&inbox_lock);
    inbox.volume = nbt::car::VolumeLevel::percent(level, max);
    pthread_mutex_unlock(&inbox_lock);
}

bool onUsb(const uint8_t* data, size_t size, void*) {
    pthread_mutex_lock(&inbox_lock);
    if (!data || size > RxLimit - inbox.rx_size) inbox.ended = true;
    else if (size && !inbox.ended) {
        inbox.rx.push_back(carlife::Bytes(data, data + size));
        inbox.rx_size += size;
    }
    const bool keep = !inbox.ended;
    pthread_mutex_unlock(&inbox_lock);
    return keep;
}

void onTouch(const nbt::TouchEventOut& event, void*) {
    if (event.n < 1 || event.action < 0 || event.action > 2 ||
        event.p[0].x < 0 || event.p[0].y < 0) return;
    int x = event.p[0].x;
    int y = event.p[0].y;
    bool inside = true;
    if (carlife::liveDisplayRequested()) {
        x -= carlife::outputX();
        y -= carlife::outputY();
        const int width = carlife::videoWidth(), height = carlife::videoHeight();
        inside = x >= 0 && x < width && y >= 0 && y < height;
        x = x < 0 ? 0 : x >= width ? width - 1 : x;
        y = y < 0 ? 0 : y >= height ? height - 1 : y;
    }
    Touch touch = {unsigned(event.action), unsigned(x), unsigned(y)};
    pthread_mutex_lock(&inbox_lock);
    if (inbox.touch_owned) {
        if (touch.action == 0) inbox.contact = inside;
        if (inbox.contact) {
            if (touch.action == 2 && !inbox.touch.empty() && inbox.touch.back().action == 2)
                inbox.touch.pop_back();
            if (inbox.touch.size() == 128) inbox.ended = true;
            else inbox.touch.push_back(touch);
            if (touch.action == 1) inbox.contact = false;
        }
    }
    pthread_mutex_unlock(&inbox_lock);
}

void onController(const nbt::car::CcsEvent& event, void*) {
    if (event.kind == 1 && (event.keycode == nbt::AAK_HOME || event.keycode == nbt::AAK_BACK)) {
        pthread_mutex_lock(&inbox_lock);
        if (event.keycode == nbt::AAK_HOME) {
            if (inbox.visible && event.down && !event.cancel) inbox.hide = true;
        } else if (event.cancel || !inbox.visible) {
            inbox.back_down = 0;
        } else if (event.down) {
            if (!inbox.back_down) inbox.back_down = now();
        } else if (inbox.back_down) {
            if (now() - inbox.back_down >= 0.8) inbox.hide = true;
            else if (inbox.keys.size() < 64) inbox.keys.push_back(0x0e);
            inbox.back_down = 0;
        }
        pthread_mutex_unlock(&inbox_lock);
        return;
    }
    if (event.cancel) return;
    unsigned code = 0, count = 1;
    if (event.kind == 2 && event.keycode == nbt::AAK_ROTARY_CONTROLLER && event.delta) {
        code = event.delta > 0 ? 0x06 : 0x07;
        const int delta = event.delta > 8 ? 8 : event.delta < -8 ? -8 : event.delta;
        count = unsigned(delta < 0 ? -delta : delta);
    } else if (event.kind == 1 && event.down) {
        switch (event.keycode) {
        case nbt::AAK_DPAD_CENTER: code = 0x14; break;
        case nbt::AAK_DPAD_LEFT: code = 0x15; break;
        case nbt::AAK_DPAD_RIGHT: code = 0x16; break;
        case nbt::AAK_DPAD_UP: code = 0x17; break;
        case nbt::AAK_DPAD_DOWN: code = 0x18; break;
        }
    }
    if (!code) return;
    pthread_mutex_lock(&inbox_lock);
    if (inbox.visible && inbox.keys.size() + count <= 64)
        for (unsigned i = 0; i < count; ++i) inbox.keys.push_back(code);
    pthread_mutex_unlock(&inbox_lock);
}

void ownInput(bool visible, bool touch) {
    pthread_mutex_lock(&inbox_lock);
    inbox.visible = visible;
    inbox.touch_owned = touch;
    if (!touch) inbox.contact = false;
    if (!visible) {
        inbox.touch.clear();
        inbox.keys.clear();
        inbox.back_down = 0;
        inbox.hide = false;
    }
    pthread_mutex_unlock(&inbox_lock);
    nbt::setTouchOwned(touch);
}

class Output : public carlife::Sink {
public:
    Output(nbt::UsbDirectLink* usb, carlife::WifiTransport* wifi, nbt::VideoManager* video)
        : usb_(usb), wifi_(wifi), video_(video), frames_(0), last_frame_(0),
          deadline_(0), touch_available_(false), version_(false), audio_ready_(false),
          audio_failed_(false), audio_playing_(false), formats_(), pcm_seen_(),
          audio_teardown_(false), audio_teardown_done_(false), audio_thread_(),
          volume_(-1), media_gain_(1.0f) {}

    ~Output() {
        if (audio_teardown_) pthread_join(audio_thread_, NULL);
    }

    void versionAccepted() {
        version_ = true;
        deadline_ = now() + carlife::HandshakeSeconds;
        if (wifi_) wifi_->setDeadline(deadline_);
        status(carlife::Connecting);
    }

    bool send(const carlife::Bytes& frame) {
        if (wifi_) return wifi_->send(frame);
        return usb_->write(&frame[0], 8) && usb_->write(&frame[8], frame.size() - 8);
    }

    bool prepareVideo(uint32_t width, uint32_t height, uint32_t fps) {
        printf("CarLife phone confirmed %ux%u at %u fps\n",
               static_cast<unsigned>(width), static_cast<unsigned>(height), static_cast<unsigned>(fps));
        if (now() >= deadline_) {
            status(carlife::HandshakeTimeout);
            return false;
        }
        if (!video_) return true;
        if (!status(carlife::PreparingDisplay)) return false;
        if (carlife::liveDisplayRequested()) carlife::nativeVideoWidth = width;
        carlife::configureDisplaySize(executablePath);
        const nbt::CarConfig& car = nbt::carConfig();
        if (stopping || !video_->init(car.screenW, car.screenH, fps)) {
            status(carlife::DisplayUnavailable);
            return false;
        }
        video_->start();
        if (!video_->prepareForSession()) {
            status(carlife::DisplayUnavailable);
            return false;
        }
        ownInput(false, false);
        static nbt::most::Client most;
        static nbt::car::CcsInput controller;
        static nbt::car::VolumeInput volume;
        if (!controller.attach(most, nbt::car::readCcsConfig(), &onController, NULL))
            fprintf(stderr, "iDrive controller unavailable on this firmware\n");
        if (!volume.attach(most, &onVolume, NULL))
            fputs("CarLife volume observer unavailable on this firmware\n", stderr);
        if (!most.start("NBTCARLIFE_MOST"))
            fprintf(stderr, "iDrive controller and volume connection unavailable\n");
        touch_available_ = nbt::startTouchForward(30567, &onTouch, NULL);
        return true;
    }

    void audioState(unsigned lane, uint32_t rate, uint32_t channels, uint32_t bits, bool active) {
        formats_[lane] = Format{rate, channels, bits, active};
        if (active) {
            printf("CarLife audio lane %u: %u Hz, %u channels, %u-bit PCM\n", lane, rate, channels, bits);
            if (managed) slogf(0, _SLOG_INFO, "CarLife audio lane %u: %u Hz, %u channels, %u-bit PCM",
                               lane, rate, channels, bits);
        }
        if (audio_playing_)
            sound_.onStreamState(audioChannel(lane), active, rate, lane == 0 ? 2 : 1);
    }

    void audio(unsigned lane, const carlife::Bytes& data, uint32_t stamp) {
        if (!pcm_seen_[lane]) {
            pcm_seen_[lane] = true;
            printf("CarLife received PCM on audio lane %u\n", lane);
            if (managed) slogf(0, _SLOG_INFO, "CarLife received PCM on audio lane %u", lane);
        }
        if (!audio_playing_ || !formats_[lane].active) return;
        // Reapply after a PCM device opens again, since the mixer resets its gain at open.
        if (lane == 0 && volume_ >= 0) sound_.setMediaGain(media_gain_);
        const Format& format = formats_[lane];
        const unsigned sample_bytes = format.bits / 8;
        const unsigned target_channels = lane == 0 ? 2 : 1;
        if (format.bits == 16 && format.channels == target_channels) {
            sound_.onFrame(audioChannel(lane), &data[0], data.size(), uint64_t(stamp) * 1000);
            return;
        }
        pcm_.clear();
        const unsigned step = sample_bytes * format.channels;
        for (size_t i = 0; i < data.size(); i += step) {
            const int left = readSample(&data[i], sample_bytes);
            const int right = format.channels == 1 ? left : readSample(&data[i + sample_bytes], sample_bytes);
            pcm_.push_back(target_channels == 1 ? (left + right) / 2 : left);
            if (target_channels == 2) pcm_.push_back(right);
        }
        sound_.onFrame(audioChannel(lane), reinterpret_cast<const uint8_t*>(&pcm_[0]),
                       pcm_.size() * sizeof(pcm_[0]), uint64_t(stamp) * 1000);
    }

    void volume(int percent) {
        if (percent < 0 || percent == volume_) return;
        volume_ = percent;
        media_gain_ = percent == 0 ? 0.0f : nbt::volumePctToGain(percent);
        if (audio_playing_) sound_.setMediaGain(media_gain_);
        if (managed) slogf(0, _SLOG_INFO, "CarLife media volume %d%%", percent);
    }

    void setAudio(bool visible) {
        if (!visible && audio_playing_) {
            audio_playing_ = false;
            audio_teardown_done_ = 0;
            audio_teardown_ = pthread_create(&audio_thread_, NULL, &Output::endAudio, this) == 0;
            if (!audio_teardown_) {
                for (unsigned lane = 0; lane < 3; ++lane)
                    sound_.onStreamState(audioChannel(lane), false, 0, 0);
            }
            return;
        }
        if (!visible || audio_playing_ || audio_failed_) return;
        if (audio_teardown_) {
            if (!__sync_val_compare_and_swap(&audio_teardown_done_, 1, 1)) return;
            pthread_join(audio_thread_, NULL);
            audio_teardown_ = false;
        }
        if (!audio_ready_) {
            const char* enabled = getenv("NBT_CARLIFE_AUDIO");
            if (enabled && !strcmp(enabled, "0")) {
                audio_failed_ = true;
                return;
            }
            nbt::BrowserConfig config = nbt::readBrowserConfig();
            config.speech.enabled = false;
            config.resampler_file = "";
            audio_ready_ = sound_.init(config);
            audio_failed_ = !audio_ready_;
            if (managed) slogf(0, _SLOG_INFO, "CarLife Browser audio driver %s",
                               audio_ready_ ? "loaded" : "failed to load");
            if (!audio_ready_) {
                fputs("CarLife audio device unavailable; continuing video.\n", stderr);
                return;
            }
        }
        audio_playing_ = true;
        for (unsigned lane = 0; lane < 3; ++lane)
            if (formats_[lane].active)
                sound_.onStreamState(audioChannel(lane), true, formats_[lane].rate, lane == 0 ? 2 : 1);
    }

    bool video(carlife::Bytes& frame, uint32_t timestamp) {
        if (!video_) {
            printf("CarLife Wi-Fi first video packet: %u bytes (network-only test)\n",
                   static_cast<unsigned>(frame.size()));
            last_frame_ = now();
            return true;
        }
        // First CarLife access unit may carry SPS/PPS and IDR together.
        // Give mdev explicit codec config as well as the original access unit.
        carlife::Bytes config;
        size_t start = frame.size();
        unsigned nal = 0;
        for (size_t i = 0; i + 3 < frame.size(); ++i) {
            if (frame[i] || frame[i + 1] || frame[i + 2] != 1) continue;
            const size_t boundary = i > 0 && frame[i - 1] == 0 ? i - 1 : i;
            if (nal == 7 || nal == 8)
                config.insert(config.end(), frame.begin() + start, frame.begin() + boundary);
            start = boundary;
            nal = frame[i + 3] & 31;
            i += 2;
        }
        if (nal == 7 || nal == 8)
            config.insert(config.end(), frame.begin() + start, frame.end());
        if (!config.empty()) video_->postCodecConfig(&config[0], config.size());

        const size_t bytes = frame.size();
        if (video_->postVideoFrame(frame, 0, bytes, uint64_t(timestamp) * 1000)) {
            video_->onVideoFrameReceived(++frames_, bytes);
            last_frame_ = now();
        }
        return true;
    }

    double lastFrame() const { return last_frame_; }
    bool touchAvailable() const { return touch_available_; }
    bool version() const { return version_; }
    double deadline() const { return deadline_; }

private:
    static void* endAudio(void* context) {
        Output* output = static_cast<Output*>(context);
        output->sound_.onSessionEnded();
        __sync_bool_compare_and_swap(&output->audio_teardown_done_, 0, 1);
        return NULL;
    }

    struct Format { uint32_t rate, channels, bits; bool active; };
    static unsigned audioChannel(unsigned lane) {
        const unsigned map[] = {nbt::AA_CH_MEDIA_AUDIO, nbt::AA_CH_GUIDANCE_AUDIO, nbt::AA_CH_SYSTEM_AUDIO};
        return map[lane];
    }
    static int readSample(const uint8_t* bytes, unsigned sample_bytes) {
        if (sample_bytes == 1) return (int(bytes[0]) - 128) * 256;
        return static_cast<int16_t>(unsigned(bytes[0]) | unsigned(bytes[1]) << 8);
    }
    nbt::UsbDirectLink* usb_;
    carlife::WifiTransport* wifi_;
    nbt::VideoManager* video_;
    uint32_t frames_;
    double last_frame_;
    double deadline_;
    bool touch_available_, version_, audio_ready_, audio_failed_, audio_playing_;
    Format formats_[3];
    nbt::BrowserAudio sound_;
    std::vector<int16_t> pcm_;
    bool pcm_seen_[3];
    bool audio_teardown_;
    int audio_teardown_done_;
    pthread_t audio_thread_;
    int volume_;
    float media_gain_;
};

nbt::UsbDirectLink* waitForPhone(nbt::UsbDirect& usb) {
    nbt::aoap::Identity identity = {};
    identity.protocol = 1;
    nbt::aoap::copyField(identity.manufacturer, sizeof(identity.manufacturer), "Baidu");
    nbt::aoap::copyField(identity.model, sizeof(identity.model), "CarLife");
    nbt::aoap::copyField(identity.description, sizeof(identity.description), "Baidu CarLife");
    nbt::aoap::copyField(identity.version, sizeof(identity.version), "1.0.0");
    nbt::aoap::copyField(identity.url, sizeof(identity.url), "http://carlife.baidu.com/");
    nbt::aoap::copyField(identity.serial, sizeof(identity.serial), "0720SerialNo.");
    bool requested = false;
    while (!stopping) {
        const std::vector<std::string> ports = usb.accessories(2.0);
        if (requested && ports.size() == 1) {
            nbt::UsbDirectLink* link = usb.open(ports[0]);
            if (link) return link;
        }
        if (ports.empty() && !requested) requested = usb.switchPhones(identity, 0.5) == 1;
        usleep(250000);
    }
    return NULL;
}

int run(nbt::UsbDirectLink* link, carlife::WifiTransport* wifi,
        nbt::VideoManager* video, unsigned fps, double version_deadline, bool& matched,
        const std::string& peer = std::string()) {
    Output output(link, wifi, video);
    printf("Requesting CarLife %dx%d at %u fps\n", carlife::videoWidth(), carlife::videoHeight(), fps);
    carlife::Session session(output, carlife::videoWidth(), carlife::videoHeight(), fps,
                             carlife::liveDisplayRequested(), wifi != NULL);
    if ((link && !link->startRx(&onUsb, NULL)) || !session.start()) {
        fprintf(stderr, "CarLife session start failed: %s\n", session.error());
        return 1;
    }

    bool visible = false, receivedVideo = false;
    bool ok = true;
    double nextSurfaceCheck = 0;
    double nextHeartbeat = 0;
    while (!stopping && ok) {
        if (!output.lastFrame() && now() >= (matched ? output.deadline() : version_deadline)) {
            fprintf(stderr, "CarLife video timed out; reconnect the phone\n");
            if (matched) status(carlife::HandshakeTimeout);
            ok = false;
            break;
        }
        Inbox batch;
        pthread_mutex_lock(&inbox_lock);
        batch.rx.swap(inbox.rx);
        batch.touch.swap(inbox.touch);
        batch.keys.swap(inbox.keys);
        inbox.rx_size = 0;
        batch.ended = inbox.ended;
        batch.volume = inbox.volume;
        batch.hide = inbox.hide || (inbox.back_down && now() - inbox.back_down >= 0.8);
        inbox.hide = false;
        pthread_mutex_unlock(&inbox_lock);
        if (batch.ended) { ok = false; break; }
        output.volume(batch.volume);
        if (wifi && !wifi->receive(session)) {
            fprintf(stderr, "CarLife Wi-Fi: %s\n",
                    *wifi->error() ? wifi->error() : "stream closed or invalid");
            ok = false;
        }
        matched = output.version();

        for (size_t i = 0; ok && i < batch.rx.size(); ++i)
            ok = session.receive(&batch.rx[i][0], batch.rx[i].size());
        if (visible && (batch.hide || visibility_request == HideProjection)) {
            visibility_request = HideProjection;
            ownInput(false, false);
            output.setAudio(false);
            const bool displayHidden = carlife::setProjectionVisibility(false);
            video->setVisible(false);
            visible = !displayHidden;   // Retry restoring iDrive during cleanup if the switch failed.
            if (!displayHidden) {
                status(carlife::DisplayUnavailable);
                ok = false;
                break;
            }
            ok = ok && session.releaseTouch();
            status(carlife::Hidden);
        }

        for (size_t i = 0; ok && visible && i < batch.touch.size(); ++i) {
            const Touch& touch = batch.touch[i];
            ok = session.touch(touch.action, touch.x, touch.y);
        }

        for (size_t i = 0; ok && visible && i < batch.keys.size(); ++i)
            ok = session.hardKey(batch.keys[i]);

        if (ok && !video && output.lastFrame()) {
            session.pause();
            return 0;
        }
        if (ok && video && !visible && output.lastFrame() &&
            (!receivedVideo || visibility_request == ShowProjection)) {
            if (!receivedVideo) {
                if (wifi) wifi->setDeadline(0);
                if (!peer.empty()) carlife::rememberWfdPeer(peer);
            }
            receivedVideo = true;
            if (stopping || !status(carlife::CoveringDisplay)) break;
            output.setAudio(true);
            video->setVisible(true);
            visible = true;  // Cleanup must restore iDrive if either visibility operation fails.
            if (stopping || !carlife::setProjectionVisibility(true)) {
                status(carlife::DisplayUnavailable);
                ok = false;
                break;
            }
            ownInput(true, output.touchAvailable());
            status(carlife::Projecting);
            puts("CarLife video started");
        }
        if (visible) {
            output.setAudio(true);
            if (now() >= nextSurfaceCheck) {
                video->serviceAaSurface();
                nextSurfaceCheck = now() + 0.25;
            }
        }
        if (receivedVideo) video->pollScreenEvents();
        const double tick = now();
        if (receivedVideo && tick >= nextHeartbeat) {
            if (!status(carlife::Heartbeat)) break;
            nextHeartbeat = tick + 1;
        }
        if (receivedVideo && tick - output.lastFrame() > 15) {
            fprintf(stderr, "CarLife video stream stopped\n");
            status(carlife::Disconnected);
            ok = false;
        }
        usleep(10000);
    }
    if (video) status(carlife::Finishing);
    ownInput(false, false);
    if (visible) {
        bool restored = carlife::setProjectionVisibility(false);
        if (!restored) {
            usleep(350000);
            restored = carlife::setProjectionVisibility(false);
        }
        if (restored) status(carlife::DisplayReleased);
        video->setVisible(false);
    }
    if (stopping && ok) {
        if (wifi) wifi->allowShutdown(now() + 0.25);
        session.releaseTouch();
        session.pause();
    }
    if (wifi) wifi->close();
    output.setAudio(false);
    if (!ok && *session.error()) fprintf(stderr, "%s\n", session.error());
    return ok ? 0 : 1;
}

int tryPhone(carlife::WifiTransport& wifi, const std::string& peer, unsigned fps,
             double deadline, bool& matched) {
    if (!carlife::rendererAvailable()) {
        status(carlife::DisplayUnavailable);
        matched = true;
        return 1;
    }
    const double versionDeadline = now() + 1.5;
    const double until = versionDeadline < deadline ? versionDeadline : deadline;
    wifi.setDeadline(until);
    nbt::VideoManager video;
    const int result = run(NULL, &wifi, &video, fps, until, matched, peer);
    video.stop();
    return result;
}

struct Candidate {
    carlife::WifiTransport connection;
    std::string address;
};

int selectPhone(const Candidate* phones, unsigned count, double deadline) {
    carlife::StatusEvent event = {carlife::ChoosePhone, static_cast<unsigned char>(count), {0, 0, 0}};
    for (unsigned i = 0; i < count; ++i)
        event.peers[i] = atoi(phones[i].address.c_str() + strlen("172.16.222."));
    peer_choice = 0;
    if (!sendStatus(event)) return -1;
    while (!stopping && now() < deadline && !peer_choice) usleep(50000);
    const int selected = peer_choice - 1;
    return selected >= 0 && static_cast<unsigned>(selected) < count ? selected : -1;
}

int autoConnect(unsigned fps, double deadline) {
    status(carlife::Searching);
    while (!stopping && now() < deadline) {
        size_t associated;
        const std::vector<std::string> peers = carlife::discoverWfdPeers(associated);
        size_t first = 0;
        if (managed && associated > 1) {
            Candidate phones[3];
            unsigned count = 0;
            for (; first < associated && count < 3 && !stopping && now() < deadline; ++first) {
                const double attempt = now() + 0.35;
                if (phones[count].connection.connectPhone(peers[first].c_str(),
                        attempt < deadline ? attempt : deadline, &stopping, true))
                    phones[count++].address = peers[first];
            }
            if (count) {
                const int selected = count == 1 ? 0 : selectPhone(phones, count, deadline);
                if (selected < 0) break;
                for (unsigned i = 0; i < count; ++i)
                    if (i != static_cast<unsigned>(selected)) phones[i].connection.close();
                bool matched = false;
                const int result = tryPhone(phones[selected].connection, phones[selected].address, fps,
                                            deadline, matched);
                if (matched || stopping) return result;
                status(carlife::Searching);
            }
        }
        for (size_t i = first; i < peers.size() && !stopping && now() < deadline; ++i) {
            carlife::WifiTransport wifi;
            const double tcpDeadline = now() + 0.35;
            if (!wifi.connectPhone(peers[i].c_str(), tcpDeadline < deadline ? tcpDeadline : deadline,
                                   &stopping, true)) continue;
            bool matched = false;
            const int result = tryPhone(wifi, peers[i], fps, deadline, matched);
            if (matched || stopping) return result;
        }
        if (!stopping && now() < deadline) usleep(250000);
    }
    if (!stopping) {
        puts("No CarLife phone found within 60 seconds. Open CarLife+ on the phone and retry.");
        status(carlife::NotFound);
    }
    return stopping ? 0 : 1;
}

bool applySavedWifiKey(const char* executable) {
    if (!carlife::savedWifiPasswordNeedsApply()) return true;
    if (!status(carlife::PreparingDisplay) || !status(carlife::ApplyingWifiKey)) return false;
    const pid_t child = fork();
    if (child == 0) {
        execlp(executable, executable, "--wifi-reapply", static_cast<char*>(NULL));
        _exit(127);
    }
    if (child < 0) return false;
    int result;
    pid_t exited;
    do { exited = waitpid(child, &result, 0); } while (exited < 0 && errno == EINTR);
    return exited == child && WIFEXITED(result) && WEXITSTATUS(result) == 0;
}

}

bool carlife::beginFactoryBorrow() {
    return !stopping && status(carlife::FactoryBorrowing) && !stopping;
}

void carlife::factoryRestored() {
    status(carlife::FactoryRestored);
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    unsetenv("LD_PRELOAD");  // The HMI graphics preload is specific to its own executable.
    executablePath = argv[0];
    if (argc == 2 && !strcmp(argv[1], "--panel-size")) return printPanelSize();
    if (argc == 2 && !strcmp(argv[1], "--restore-idrive")) return carlife::setProjectionVisibility(false) ? 0 : 1;
    if (argc == 2 && strcmp(argv[1], "--wifi-direct-info") == 0)
        return printWifiDirectInfo() ? 0 : 1;
    if (argc == 3 && !strcmp(argv[1], "--wifi-key") && !strcmp(argv[2], "--menu"))
        return wifiKeyCommand(false, false, true);
    const bool resetWifi = (argc == 3 || argc == 4) && !strcmp(argv[1], "--wifi-reset") &&
                           !strcmp(argv[2], "--confirm") && (argc == 3 || !strcmp(argv[3], "--menu"));
    const bool reapplyWifi = argc == 2 && !strcmp(argv[1], "--wifi-reapply");
    if (resetWifi || reapplyWifi) {
        signal(SIGPIPE, SIG_IGN);
        const int result = wifiKeyCommand(resetWifi, reapplyWifi, argc == 4);
        fflush(stdout);
        _exit(result);   // The GCF reply thread may remain blocked inside the vehicle library.
    }
    if (argc == 2 && strcmp(argv[1], "--pf-check") == 0) return carlife::firewall(false);
    if (argc == 2 && strcmp(argv[1], "--pf-install") == 0) return carlife::firewall(true);
    const bool auto_mode = (argc == 2 || (argc == 3 && !strcmp(argv[2], "--managed"))) &&
                           !strcmp(argv[1], "--auto");
    managed = auto_mode && argc == 3;
    const bool usb_mode = argc == 2 && strcmp(argv[1], "--bench") == 0;
    const bool wifi_display = (argc == 3 || argc == 4) && strcmp(argv[1], "--wifi-render") == 0;
    const bool wifi_mode = wifi_display || (argc == 3 && strcmp(argv[1], "--wifi") == 0);
    if (!auto_mode && !usb_mode && !wifi_mode) {
        fprintf(stderr, "Usage: %s --auto | --wifi-render PHONE_IPV4 [FPS] | --wifi PHONE_IPV4 | --wifi-direct-info | --wifi-reset --confirm | --pf-check | --pf-install | --bench\n",
                argv[0]);
        return 2;
    }
    unsigned fps = wifi_display || auto_mode ? 60 : 30;
    if (wifi_display && argc == 4) {
        char* end = NULL;
        const unsigned long requested = strtoul(argv[3], &end, 10);
        if (*end || requested < 1 || requested > 60) {
            fputs("CarLife FPS must be an integer from 1 to 60.\n", stderr);
            return 2;
        }
        fps = static_cast<unsigned>(requested);
    }
    signal(SIGINT, &stop);
    signal(SIGTERM, &stop);
    signal(SIGHUP, &stop);
    struct sigaction wake = {};
    wake.sa_sigaction = &show;
    wake.sa_flags = SA_SIGINFO;
    sigemptyset(&wake.sa_mask);
    sigaction(SIGUSR1, &wake, NULL);
    signal(SIGUSR2, &hide);
    signal(SIGPIPE, SIG_IGN);
    // Use a different LayerManager surface from CarPlay (1280) and Supra (1282).
    setenv("NBT_AA_SURFACE", carlife::ProjectionSurface, 1);
    if (wifi_display || auto_mode) setenv("NBT_CARLIFE_LIVE_DISPLAY", "1", 1);
    else unsetenv("NBT_CARLIFE_LIVE_DISPLAY");
    setenv("NBT_CP_PURGE", "0", 1);
    setenv("NBT_CP_HANDOFF", "1", 1);
    setenv("NBT_MMR_SPAWN", "0", 1);
    setenv("NBT_MMR_WAIT_S", "2", 1);
    unsetenv("NBT_PURGE_ALL");
    unsetenv("NBT_VIDEO_DUMP");
    unsetenv("NBT_VID_SRC_H");
    if (wifi_display || auto_mode) {
        unsetenv("NBT_VID_DST_W");
        unsetenv("NBT_VID_DST_H");
    }
    setenv("NBT_VID_RES", "2", 1);
    if (auto_mode) {
        if (!status(carlife::WatchdogReady)) return 1;
        const double deadline = now() + carlife::SearchSeconds;
        if (!applySavedWifiKey(argv[0])) {
            status(carlife::WifiKeyApplyFailed);
            return 1;
        }
        return autoConnect(fps, deadline);
    }

    nbt::UsbDirect usb;
    nbt::UsbDirectLink* link = NULL;
    carlife::WifiTransport wifi;
    if (usb_mode) {
        if (!usb.start() || !usb.claimAoapDriver()) {
            fprintf(stderr, "Cannot acquire exclusive QNX AOA transport\n");
            return 1;
        }
        puts("Waiting for one Android phone; unplug/replug if already in accessory mode.");
        link = waitForPhone(usb);
        if (!link) return stopping ? 0 : 1;
    } else {
        if (!wifi_display && isatty(STDOUT_FILENO)) printWifiDirectInfo();
        printf("Connecting to CarLife over Wi-Fi at %s\n", argv[2]);
        if (!wifi.connectPhone(argv[2], now() + 1.5, &stopping)) {
            fprintf(stderr, "CarLife Wi-Fi: %s\n", wifi.error());
            return 1;
        }
    }

    if (stopping) return 0;
    const double versionDeadline = now() + carlife::HandshakeSeconds;
    wifi.setDeadline(versionDeadline);
    bool matched = false;
    if (wifi_mode && !carlife::rendererAvailable()) {
        if (wifi_display) return 1;
        puts("Testing CarLife network and video packets only.");
        return run(NULL, &wifi, NULL, fps, versionDeadline, matched);
    }
    nbt::VideoManager video;
    const int result = run(link, wifi_mode ? &wifi : NULL, &video, fps, versionDeadline, matched);
    if (link) {
        link->abort();
        link->close();
    }
    video.stop();
    usb.stop();
    return result;
}
