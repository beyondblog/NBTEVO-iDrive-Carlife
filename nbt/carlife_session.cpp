#include "carlife_session.hpp"
#include <stdio.h>
#include <string.h>

namespace carlife {
namespace {

const size_t MaxInner = 4 * 1024 * 1024;

uint32_t be32(const uint8_t* data) {
    return uint32_t(data[0]) << 24 | uint32_t(data[1]) << 16 |
           uint32_t(data[2]) << 8 | data[3];
}

void append32(Bytes& data, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        data.push_back(uint8_t(value >> shift));
}

void appendVarint(Bytes& data, uint32_t value) {
    while (value >= 128) {
        data.push_back(uint8_t((value & 127) | 128));
        value >>= 7;
    }
    data.push_back(uint8_t(value));
}

void field(Bytes& data, unsigned number, uint32_t value) {
    appendVarint(data, number << 3);
    appendVarint(data, value);
}

void stringField(Bytes& data, unsigned number, const char* value, size_t size) {
    appendVarint(data, (number << 3) | 2);
    appendVarint(data, uint32_t(size));
    data.insert(data.end(), value, value + size);
}

bool readVarint(const Bytes& data, size_t& pos, uint64_t& value) {
    value = 0;
    for (unsigned shift = 0; shift <= 63 && pos < data.size(); shift += 7) {
        uint8_t byte = data[pos++];
        if (shift == 63 && byte > 1) return false;
        value |= uint64_t(byte & 127) << shift;
        if (!(byte & 128)) return true;
    }
    return false;
}

// Version and video negotiation use fields 1–3. Skip other valid wire fields.
bool readFields(const Bytes& data, uint32_t* values) {
    size_t pos = 0;
    while (pos < data.size()) {
        uint64_t tag, value, skip = 0;
        if (!readVarint(data, pos, tag) || tag < 8 || tag > 0xffffffffu) return false;
        switch (tag & 7) {
        case 0:
            if (!readVarint(data, pos, value)) return false;
            if ((tag >> 3) <= 3) {
                if (value > 0xffffffffu) return false;
                values[(tag >> 3) - 1] = uint32_t(value);
            }
            break;
        case 1: skip = 8; break;
        case 2: if (!readVarint(data, pos, skip)) return false; break;
        case 5: skip = 4; break;
        default: return false;
        }
        if (skip > data.size() - pos) return false;
        pos += size_t(skip);
    }
    return true;
}

}

Session::Session(Sink& sink, uint32_t width, uint32_t height, uint32_t fps,
                 bool allow_encoder_adjustments, bool wireless)
    : sink_(sink), width_(width), height_(height), fps_(fps), allow_encoder_adjustments_(allow_encoder_adjustments),
      wireless_(wireless), state_(New), contact_(false), touch_x_(0), touch_y_(0), audio_(), error_("") {}

bool Session::fail(const char* reason) {
    state_ = Failed;
    error_ = reason;
    return false;
}

bool Session::send(uint32_t message, const Bytes& payload) {
    Bytes frame;
    append32(frame, message >> 16);
    append32(frame, uint32_t(payload.size()) + 8);
    append32(frame, uint32_t(payload.size()) << 16);
    append32(frame, message);
    frame.insert(frame.end(), payload.begin(), payload.end());
    return sink_.send(frame) || fail("transport write failed");
}

bool Session::sendWirelessFeatures() {
    // CarLife's feature value 0 selects 44.1 kHz by default. The actual incoming format may differ.
    const struct { const char* name; uint32_t value; } features[] = {
        {"CONNECT_TYPE", 5}, {"MEDIA_SAMPLE_RATE", 0},
    };
    Bytes list;
    field(list, 1, sizeof(features) / sizeof(features[0]));
    for (const auto& feature : features) {
        Bytes item;
        stringField(item, 1, feature.name, strlen(feature.name));
        field(item, 2, feature.value);
        stringField(list, 2, reinterpret_cast<const char*>(&item[0]), item.size());
    }
    return send(0x18052, list);
}

bool Session::start() {
    if (state_ != New || !width_ || !height_ || !fps_) return fail("invalid session start");
    Bytes version;
    field(version, 1, 1);
    field(version, 2, 0);
    state_ = Version;
    return send(0x18001, version);
}

bool Session::receive(const uint8_t* data, size_t size) {
    if (state_ == Failed) return false;
    if (!size) return true;
    if (size > MaxInner + 8) return fail("USB chunk exceeds frame limit");
    pending_.insert(pending_.end(), data, data + size);
    size_t offset = 0;
    while (pending_.size() - offset >= 8) {
        const uint8_t* header = &pending_[offset];
        const uint32_t channel = be32(header);
        const uint32_t size = be32(header + 4);
        const unsigned inner = channel == 1 || channel == 6 ? 8 : 12;
        if (channel < 1 || channel > 6 || size < inner || size > MaxInner)
            return fail("invalid USB frame header");
        if (pending_.size() - offset < size + 8) break;

        if (!receiveInner(channel, header + 8, size)) return false;
        offset += size + 8;
    }
    pending_.erase(pending_.begin(), pending_.begin() + offset);
    return true;
}

bool Session::receiveChannel(unsigned channel, const uint8_t* data, size_t size) {
    if (state_ == Failed) return false;
    if (channel < 1 || channel > 6 || size > MaxInner) return fail("invalid Wi-Fi channel input");
    if (!size) return true;
    Bytes& pending = channels_[channel - 1];
    pending.insert(pending.end(), data, data + size);
    const unsigned header = channel == 1 || channel == 6 ? 8 : 12;
    size_t offset = 0;
    while (pending.size() - offset >= header) {
        const uint8_t* body = &pending[offset];
        uint32_t length = be32(body);
        if (header == 8) length >>= 16;
        if (length > MaxInner - header) return fail("Wi-Fi frame exceeds limit");
        if (pending.size() - offset < header + length) break;
        if (!receiveInner(channel, body, header + length)) return false;
        offset += header + length;
    }
    pending.erase(pending.begin(), pending.begin() + offset);
    return true;
}

bool Session::receiveInner(unsigned channel, const uint8_t* body, size_t size) {
    const unsigned header = channel == 1 || channel == 6 ? 8 : 12;
    uint32_t length = be32(body);
    const uint32_t timestamp = header == 12 ? be32(body + 4) : 0;
    const uint32_t message = be32(body + header - 4);
    if (header == 8) {
        if (length & 0xffff) return fail("nonzero command reserved field");
        length >>= 16;
    }
    if (length != size - header || (message >> 16) != channel)
        return fail("CarLife inner frame mismatch");
    Bytes payload(body + header, body + size);
    return dispatch(message, timestamp, payload);
}

bool Session::dispatch(uint32_t message, uint32_t timestamp, Bytes& payload) {
    uint32_t values[3] = {0, 0, 0};
    switch (message) {
    case 0x18048: case 0x10049: case 0x1804a: case 0x1004b:
    case 0x1006a: case 0x1806b: case 0x1006c: case 0x1806d:
    case 0x1006e: case 0x18083:
        return fail("phone requires a CarLife authentication provider");
    case 0x10002: {
        if (state_ != Version) return true;
        if (!readFields(payload, values) || values[0] != 1)
            return fail("phone did not accept protocol version 1.0");
        sink_.versionAccepted();
        Bytes info;
        stringField(info, 1, "QNX", 3);
        stringField(info, 4, "NBT Evo", 7);
        stringField(info, 14, "Development HU", 14);
        stringField(info, 19, "1", 1);
        Bytes mode;
        field(mode, 1, width_);
        field(mode, 2, height_);
        field(mode, 3, fps_);
        state_ = VideoMode;
        return send(0x18003, info) && (!wireless_ || sendWirelessFeatures()) && send(0x18007, mode);
    }
    case 0x10051:
        return !wireless_ || (state_ != VideoMode && state_ != Streaming) || sendWirelessFeatures();
    case 0x10008: {
        if (state_ != VideoMode) return true;
        if (!readFields(payload, values)) return fail("invalid video mode reply");
        // vivo may round the requested width down to an H.264 macroblock.
        const bool aligned_width = allow_encoder_adjustments_ && values[0] && values[0] == (width_ & ~15u);
        const bool lower_fps = allow_encoder_adjustments_ && values[2] && values[2] < fps_;
        if ((values[0] != width_ && !aligned_width) || values[1] != height_ ||
            (values[2] != fps_ && !lower_fps)) {
            snprintf(mode_error_, sizeof(mode_error_),
                     "phone selected %ux%u at %u fps; requested %ux%u at %u fps",
                     (unsigned)values[0], (unsigned)values[1], (unsigned)values[2],
                     (unsigned)width_, (unsigned)height_, (unsigned)fps_);
            return fail(mode_error_);
        }
        width_ = values[0];
        fps_ = values[2];
        if (!sink_.prepareVideo(width_, height_, fps_)) return fail("could not prepare CarLife video output");
        state_ = Streaming;
        return send(0x18009, Bytes());
    }
    case 0x20001:
        if (state_ != Streaming || payload.empty()) return true;
        return sink_.video(payload, timestamp) || fail("video sink stopped");
    case 0x30001: case 0x40001: case 0x50002: {
        // The separate Wi-Fi audio lanes can deliver their format before the video reply.
        if (state_ != VideoMode && state_ != Streaming) return true;
        const unsigned lane = (message >> 16) - 3;
        AudioLane& stream = audio_[lane];
        stream.active = readFields(payload, values) && values[0] >= 8000 && values[0] <= 48000 &&
                        (values[1] == 1 || values[1] == 2) && (values[2] == 8 || values[2] == 16);
        stream.rate = stream.active ? values[0] : 0;
        stream.channels = values[1];
        stream.bits = values[2];
        if (!stream.active)
            fprintf(stderr, "CarLife rejected audio lane %u format: %u Hz, channels=%u, format=%u\n",
                    lane, values[0], values[1], values[2]);
        sink_.audioState(lane, stream.rate, stream.channels, stream.bits, stream.active);
        return true;
    }
    case 0x30002: case 0x30003: case 0x30004: case 0x40002: case 0x50004: {
        const unsigned lane = (message >> 16) - 3;
        AudioLane& stream = audio_[lane];
        stream.active = message == 0x30004 && stream.active;
        // Retain the format during a media pause so a resume can continue the stream.
        if (message == 0x30004) stream.active = stream.rate && stream.channels && stream.bits;
        if (message != 0x30003 && message != 0x30004) stream.rate = 0;
        sink_.audioState(lane, stream.rate, stream.channels, stream.bits, stream.active);
        return true;
    }
    case 0x30006: case 0x40003: case 0x50003: {
        const unsigned lane = (message >> 16) - 3;
        const AudioLane& stream = audio_[lane];
        if (state_ == Streaming && stream.active && !payload.empty() && payload.size() <= 65536 &&
            payload.size() % (stream.channels * (stream.bits / 8)) == 0)
            sink_.audio(lane, payload, timestamp);
        return true;
    }
    default:
        return true;
    }
}

bool Session::touch(uint32_t action, uint32_t x, uint32_t y) {
    if (!streaming() || x >= width_ || y >= height_ || action > 2) return true;
    if ((action == 0) == contact_) return true;
    Bytes payload;
    field(payload, 1, action);
    field(payload, 2, x);
    field(payload, 3, y);
    if (!send(0x68001, payload)) return false;
    contact_ = action != 1;
    touch_x_ = x;
    touch_y_ = y;
    return true;
}

bool Session::hardKey(uint32_t code) {
    if (!streaming()) return true;
    Bytes payload;
    field(payload, 1, code);
    return send(0x68008, payload);
}

bool Session::pause() {
    return !streaming() || send(0x1800a, Bytes());
}

bool Session::releaseTouch() { return !contact_ || touch(1, touch_x_, touch_y_); }

}
