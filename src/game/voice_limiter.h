#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace mecmp::game::voice_limiter {
// Whole-block peak limiting: immediate attack, 80 ms release. The gain is
// constant within a block, preserving its waveform instead of hard clipping.
// Storage and gain belong to the caller; no allocation or buffering is needed.
inline void apply(float* pcm, size_t count, float rate, float& gain) {
    constexpr float ceiling = .98f;
    float peak = 0.f;
    for (size_t i = 0; i < count; ++i) peak = std::max(peak, std::fabs(pcm[i]));
    const float target = peak > ceiling ? ceiling / peak : 1.f;
    const float release = 1.f - std::exp(-static_cast<float>(count) / (rate * .08f));
    gain = target < gain ? target : gain + (target - gain) * release;
    for (size_t i = 0; i < count; ++i) pcm[i] *= gain;
}
} // namespace mecmp::game::voice_limiter
