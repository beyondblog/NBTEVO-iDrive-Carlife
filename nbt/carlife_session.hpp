#ifndef NBT_CARLIFE_SESSION_HPP
#define NBT_CARLIFE_SESSION_HPP

#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace carlife {

typedef std::vector<uint8_t> Bytes;

class Sink {
public:
    virtual ~Sink() {}
    virtual bool send(const Bytes& frame) = 0;
    virtual void versionAccepted() {}
    virtual bool prepareVideo(uint32_t, uint32_t, uint32_t) { return true; }
    virtual bool video(Bytes& annex_b, uint32_t timestamp) = 0;
    virtual void audioState(unsigned, uint32_t, uint32_t, uint32_t, bool) {}
    virtual void audio(unsigned, const Bytes&, uint32_t) {}
};

class Session {
public:
    Session(Sink& sink, uint32_t width, uint32_t height, uint32_t fps,
            bool allow_encoder_adjustments = false, bool wireless = false);
    bool start();
    bool receive(const uint8_t* data, size_t size);
    bool receiveChannel(unsigned channel, const uint8_t* data, size_t size);
    bool touch(uint32_t action, uint32_t x, uint32_t y);
    bool hardKey(uint32_t code);
    bool pause();
    bool releaseTouch();
    bool streaming() const { return state_ == Streaming; }
    const char* error() const { return error_; }

private:
    enum State { New, Version, VideoMode, Streaming, Failed };
    bool receiveInner(unsigned channel, const uint8_t* body, size_t size);
    bool dispatch(uint32_t message, uint32_t timestamp, Bytes& payload);
    bool send(uint32_t message, const Bytes& payload);
    bool sendWirelessFeatures();
    bool fail(const char* reason);

    Sink& sink_;
    uint32_t width_, height_, fps_;
    bool allow_encoder_adjustments_;
    bool wireless_;
    State state_;
    bool contact_;
    uint32_t touch_x_, touch_y_;
    struct AudioLane { uint32_t rate, channels, bits; bool active; };
    AudioLane audio_[3];
    const char* error_;
    char mode_error_[144];
    Bytes pending_;
    Bytes channels_[6];
};

}
#endif
