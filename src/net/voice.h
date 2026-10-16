#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace mecmp::net {

// One independent 20 ms block of 16 kHz mono speech, mu-law companded to 8 bits.
// Fixed size, no fragmentation, native pointers, external codec or Steam dependency.
constexpr unsigned kVoiceRate = 16000;
constexpr size_t kVoiceSamples = 320;
constexpr size_t kVoicePayloadBytes = 8 + kVoiceSamples;
struct VoiceFrame {
    uint32_t stream = 0, sequence = 0;
    std::array<uint8_t, kVoiceSamples> samples{};
};

inline uint8_t encodeVoiceSample(int16_t sample) {
    int magnitude = sample;
    const int sign = magnitude < 0 ? 0x80 : 0;
    if (magnitude < 0) magnitude = -magnitude;
    magnitude = magnitude > 32635 ? 32767 : magnitude + 132;
    int exponent = 7;
    for (int bit = 0x4000; exponent > 0 && !(magnitude & bit); bit >>= 1) --exponent;
    const int mantissa = (magnitude >> (exponent + 3)) & 15;
    return static_cast<uint8_t>(~(sign | (exponent << 4) | mantissa));
}
inline int16_t decodeVoiceSample(uint8_t encoded) {
    const int value = static_cast<uint8_t>(~encoded);
    const int magnitude = (((value & 15) << 3) + 132) << ((value >> 4) & 7);
    return static_cast<int16_t>((value & 0x80) ? 132 - magnitude : magnitude - 132);
}

// Independent from the movement/event budget: at most ~19 KiB/s per sender.
// A small burst permits normal UDP bunching without building a voice backlog.
struct VoiceBucket {
    double tokens = 12;
    uint32_t at = 0;
    bool take(uint32_t now) {
        const double refill = static_cast<uint32_t>(now - at) * 55.0 / 1000.0;
        tokens = tokens + refill > 12.0 ? 12.0 : tokens + refill;
        at = now;
        if (tokens < 1.0) return false;
        tokens -= 1.0;
        return true;
    }
};

} // namespace mecmp::net
