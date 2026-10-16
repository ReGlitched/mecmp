#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mecmp::game::voice_chat {
struct Peer { uint32_t id = 0; std::string name; bool muted = false, speaking = false; };
struct Status {
    bool nativeReady = false, transmitting = false;
    float microphoneLevel = 0;
    uint64_t sent = 0, received = 0;
    std::string detail;
    std::vector<Peer> peers;
};
void start();
void shutdown();
void retryMicrophone();
void mute(uint32_t id, bool muted);
Status status();
} // namespace mecmp::game::voice_chat
