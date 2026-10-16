#pragma once

#include <cstddef>
#include <cstdint>
#include "net/voice.h"

namespace mecmp::game::native_voice {

struct Mix { uint32_t peer = 0; float gain = 0; float position[3] = {}; };
bool install();
bool available();
struct PlaybackStatus { uint32_t sources = 0; uint64_t created = 0, blocks = 0; };
PlaybackStatus playbackStatus();
void clientTick(); // native source creation, placement and release
void reset();
void scene(const Mix* mixes, size_t count);
bool submit(const Mix& mix, const net::VoiceFrame& frame);

} // namespace mecmp::game::native_voice
