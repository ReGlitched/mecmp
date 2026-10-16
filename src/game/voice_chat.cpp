#include "game/voice_chat.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include "core/config.h"
#include "core/log.h"
#include "game/camera.h"
#include "game/hooks.h"
#include "game/level.h"
#include "game/native_voice.h"
#include "net/net.h"
#include "ui/chat.h"
#include "ui/gamepad.h"
#include "ui/overlay.h"

namespace mecmp::game::voice_chat {
namespace {
// Receive-side balance applies to existing saved voice-volume settings too.
constexpr float kPlaybackGain = 1.6f;
std::atomic<bool> g_run{false};
std::atomic<uint32_t> g_retry{0};
HANDLE g_thread = nullptr;
std::mutex g_mutex;
Status g_status;
std::set<uint32_t> g_muted;

struct Microphone {
    IMMDeviceEnumerator* devices = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    bool recording = false;
    std::array<int16_t, net::kVoiceSamples> partial{};
    size_t used = 0;
    ~Microphone() { close(); }
    void close() {
        if (client && recording) client->Stop();
        recording = false; used = 0;
        if (capture) { capture->Release(); capture = nullptr; }
        if (client) { client->Release(); client = nullptr; }
        if (device) { device->Release(); device = nullptr; }
        if (devices) { devices->Release(); devices = nullptr; }
    }
    HRESULT open() {
        close();
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
            __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&devices));
        if (SUCCEEDED(hr)) hr = devices->GetDefaultAudioEndpoint(eCapture, eCommunications, &device);
        if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
        // Windows performs channel conversion and resampling in shared mode.
        // No loopback flag: only the microphone is ever captured.
        WAVEFORMATEX format{WAVE_FORMAT_PCM, 1, net::kVoiceRate, net::kVoiceRate * 2, 2, 16, 0};
        if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | AUDCLNT_STREAMFLAGS_NOPERSIST,
            2000000, 0, &format, nullptr);
        if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture));
        if (SUCCEEDED(hr)) hr = client->Start();
        recording = SUCCEEDED(hr);
        if (!recording) close();
        return hr;
    }
    HRESULT read(uint32_t stream, uint32_t& sequence, uint32_t generation, float gain, float& peak, uint64_t& sent) {
        // A stalled capture thread discards old speech instead of sending a long
        // burst. Six blocks are the entire outgoing latency budget.
        std::array<net::VoiceFrame, 6> recent;
        size_t count = 0;
        HRESULT hr = S_OK;
        for (int pass = 0; pass < 32; ++pass) {
            UINT32 waiting = 0;
            hr = capture->GetNextPacketSize(&waiting);
            if (FAILED(hr) || !waiting) break;
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;
            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) used = 0;
            if (frames > net::kVoiceRate) { used = 0; capture->ReleaseBuffer(frames); continue; }
            const auto* input = reinterpret_cast<const int16_t*>(data);
            for (UINT32 i = 0; i < frames; ++i) {
                const float sample = (flags & AUDCLNT_BUFFERFLAGS_SILENT) || !input ? 0.f : input[i] * gain;
                partial[used++] = static_cast<int16_t>(std::clamp(sample, -32768.f, 32767.f));
                peak = std::max(peak, std::fabs(sample) / 32768.f);
                if (used == partial.size()) {
                    auto& frame = recent[count++ % recent.size()];
                    frame.stream = stream; frame.sequence = sequence++;
                    for (size_t n = 0; n < partial.size(); ++n) frame.samples[n] = net::encodeVoiceSample(partial[n]);
                    used = 0;
                }
            }
            const HRESULT released = capture->ReleaseBuffer(frames);
            if (FAILED(released)) { hr = released; break; }
        }
        if (SUCCEEDED(hr)) {
            const size_t begin = count > recent.size() ? count - recent.size() : 0;
            for (size_t i = begin; i < count; ++i) { net::publishVoice(recent[i % recent.size()], generation); ++sent; }
        }
        return hr;
    }
};

DWORD WINAPI worker(void*) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        Microphone mic;
        uint32_t session = 0, sequence = 0, stream = GetTickCount() | 1, retryRevision = g_retry;
        uint32_t generation = 0;
        uintptr_t world = 0;
        DWORD retryAt = 0;
        HRESULT failure = S_OK;
        uint64_t sent = 0, received = 0;
        std::map<uint32_t, DWORD> speaking;
        bool wasActive = false;
        while (g_run) {
            const auto cfg = config::get();
            const DWORD now = GetTickCount();
            const auto key = net::sessionKey();
            const auto nextGeneration = net::sessionGeneration();
            const auto currentWorld = level::serverWorld();
            if (key != session || currentWorld != world || nextGeneration != generation) {
                native_voice::reset();
                mic.close(); speaking.clear();
                { std::lock_guard<std::mutex> lock(g_mutex); g_muted.clear(); }
                session = key; world = currentWorld;
                generation = nextGeneration;
                failure = S_OK; retryAt = 0;
            }
            if (retryRevision != g_retry.load()) {
                retryRevision = g_retry; mic.close(); failure = S_OK; retryAt = 0;
            }
            Status state;
            state.nativeReady = native_voice::available();
            net::PlayerState local;
            CameraView view{};
            const bool active = cfg->voiceEnabled && key && currentWorld && !level::inFrontEnd() &&
                net::localState(local) && (local.flags & net::kStateInLevel) && camera::read(view);
            if (!active && wasActive) { native_voice::reset(); mic.close(); speaking.clear(); }
            wasActive = active;
            std::set<uint32_t> muted;
            { std::lock_guard<std::mutex> lock(g_mutex); muted = g_muted; }
            std::array<native_voice::Mix, 15> mixes{};
            size_t mixCount = 0;
            for (const auto& remote : net::remotes()) {
                if (remote.loopback) continue;
                Peer peer{remote.id, remote.name, muted.count(remote.id) != 0, false};
                const auto last = speaking.find(remote.id);
                peer.speaking = last != speaking.end() && now - last->second < 250 && !peer.muted;
                state.peers.push_back(peer);
                net::PlayerState position;
                uint32_t age = 0;
                if (!active || peer.muted || mixCount >= mixes.size() ||
                    !net::remoteState(remote.id, position, age) || age > 1000 || !(position.flags & net::kStateInLevel)) continue;
                float delta[3];
                float distance2 = 0;
                for (int i = 0; i < 3; ++i) {
                    delta[i] = position.pos[i] - local.pos[i];
                    distance2 += delta[i] * delta[i];
                }
                const float distance = std::sqrt(distance2);
                const float range = std::clamp(cfg->voiceDistance, 5.f, 100.f);
                if (!std::isfinite(distance) || distance >= range) continue;
                const float fade = std::clamp((range - distance) / (range - 2.f), 0.f, 1.f);
                // fade^1.5 drops speech faster while keeping full near gain within 2 m.
                const float distanceGain = fade * std::sqrt(fade);
                const float volume = std::clamp(
                    std::clamp(cfg->voiceVolume, 0.f, 2.f) * kPlaybackGain * distanceGain, 0.f, 2.f);
                native_voice::Mix mix;
                mix.peer = remote.id; mix.gain = volume;
                std::copy_n(position.pos, 3, mix.position);
                mix.position[1] += remote.crouched ? .85f : 1.55f;
                mixes[mixCount++] = mix;
            }
            native_voice::scene(mixes.data(), mixCount);
            // Drain even while disabled: enabling voice never plays old speech.
            for (const auto& packet : net::takeVoice()) {
                if (!active || GetTickCount() - packet.receivedAt > 200) continue;
                for (size_t i = 0; i < mixCount; ++i) if (mixes[i].peer == packet.peer) {
                    if (native_voice::submit(mixes[i], packet.frame)) {
                        ++received;
                        bool audible = false;
                        for (uint8_t sample : packet.frame.samples)
                            if (std::abs(static_cast<int>(net::decodeVoiceSample(sample))) > 256) { audible = true; break; }
                        if (audible) speaking[packet.peer] = now;
                    }
                    break;
                }
            }
            for (auto i = speaking.begin(); i != speaking.end();)
                if (now - i->second > 1000) i = speaking.erase(i); else ++i;
            const bool foreground = ui::gameFocused() && !ui::menuOpen() && !ui::chat::isOpen() && !hooks::gameMenuOpen();
            const bool held = ui::voiceKeyHeld() || ui::gamepad::held(ui::gamepad::Action::Voice);
            const bool wanted = active && cfg->voiceMicrophone && foreground &&
                cfg->voiceMicGain > 0 && (cfg->voiceOpenMic || held) && SUCCEEDED(com);
            if (!wanted) {
                mic.close();
                if (!cfg->voiceEnabled) { failure = S_OK; retryAt = 0; }
            } else {
                if (!mic.recording && (!retryAt || static_cast<int32_t>(now - retryAt) >= 0)) {
                    failure = mic.open();
                    if (FAILED(failure)) {
                        retryAt = now + 3000;
                        MLOG("voice: Windows microphone could not start (0x%08lx)", static_cast<unsigned long>(failure));
                    } else { retryAt = 0; sequence = 0; if (!++stream) ++stream; }
                }
                if (mic.recording) {
                    float peak = 0;
                    failure = mic.read(stream, sequence, generation, std::clamp(cfg->voiceMicGain, 0.f, 4.f), peak, sent);
                    state.microphoneLevel = std::min(1.f, peak);
                    if (FAILED(failure)) { mic.close(); retryAt = now + 3000; }
                }
            }
            state.transmitting = mic.recording && wanted;
            state.sent = sent; state.received = received;
            if (!cfg->voiceEnabled) state.detail = "Proximity chat is off.";
            else if (!active) state.detail = "Join multiplayer and enter free roam to use proximity chat.";
            else if (FAILED(com)) state.detail = "Windows microphone initialization failed; receiving voice still works.";
            else if (!cfg->voiceMicrophone) state.detail = "Listening. Your microphone is muted.";
            else if (cfg->voiceMicGain <= 0) state.detail = "Listening. Microphone gain is zero.";
            else if (FAILED(failure)) {
                char error[160];
                snprintf(error, sizeof(error), "Microphone unavailable (0x%08lx). Check Windows input/privacy settings or retry.",
                    static_cast<unsigned long>(failure));
                state.detail = error;
            } else if (!foreground) state.detail = "Listening. Microphone paused while menus/chat are open or the game is unfocused.";
            else if (state.transmitting) state.detail = "Transmitting microphone audio.";
            else if (!cfg->voiceOpenMic && cfg->voiceKey.empty() && cfg->voicePad.empty())
                state.detail = "Assign a talk key or gamepad combination in Controls > Voice chat.";
            else state.detail = "Push-to-talk ready.";
            if (cfg->voiceEnabled && active && !state.nativeReady)
                state.detail += " Native playback is waiting for the game's spatial voice source.";
            { std::lock_guard<std::mutex> lock(g_mutex); g_status = std::move(state); }
            Sleep(10);
        }
        mic.close();
    }
    native_voice::reset();
    if (SUCCEEDED(com)) CoUninitialize();
    return 0;
}
} // namespace

void start() {
    if (g_run.exchange(true)) return;
    native_voice::install();
    g_thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    if (!g_thread) {
        g_run = false;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_status.detail = "Voice worker could not start.";
    }
}
void shutdown() {
    g_run = false;
    if (g_thread) { WaitForSingleObject(g_thread, 2000); CloseHandle(g_thread); g_thread = nullptr; }
}
void retryMicrophone() { ++g_retry; }
void mute(uint32_t id, bool muted) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (muted && g_muted.size() < 64) g_muted.insert(id);
    else if (!muted) g_muted.erase(id);
}
Status status() { std::lock_guard<std::mutex> lock(g_mutex); return g_status; }
} // namespace mecmp::game::voice_chat
