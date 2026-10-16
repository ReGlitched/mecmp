#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace mecmp::net {

// Independent 20 ms blocks of 16 kHz mono speech, mu-law companded to 8 bits.
// The 344-byte complete packet fits the existing 1024-byte transport buffer.
constexpr unsigned kVoiceRate = 16000, kVoiceFrameMs = 20;
constexpr size_t kVoiceSamples = kVoiceRate * kVoiceFrameMs / 1000;
constexpr size_t kVoiceSampleBytes = kVoiceSamples * sizeof(uint8_t);
constexpr size_t kVoicePayloadBytes = 8 + kVoiceSampleBytes;
constexpr size_t kVoiceCaptureFrames = 120 / kVoiceFrameMs;
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

// Independent from movement/event traffic: 50 packets/s with 10% slack.
// Keep the same 120 ms burst budget at the lower packet rate.
struct VoiceBucket {
    static constexpr double kBurst = static_cast<double>(kVoiceCaptureFrames);
    double tokens = kBurst;
    uint32_t at = 0;
    bool take(uint32_t now) {
        constexpr double rate = (1000.0 / kVoiceFrameMs) * 1.1;
        const double refill = static_cast<uint32_t>(now - at) * rate / 1000.0;
        tokens = tokens + refill > kBurst ? kBurst : tokens + refill;
        at = now;
        if (tokens < 1.0) return false;
        tokens -= 1.0;
        return true;
    }
};

} // namespace mecmp::net
