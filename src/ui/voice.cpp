#include "ui/voice.h"

#include "core/config.h"
#include "game/voice_chat.h"
#include "imgui.h"
#include "ui/overlay.h"

namespace mecmp::ui::voice {
void draw() {
    if (!config::get()->voiceEnabled || menuOpen() || overlayHidden()) return;
    const auto status = game::voice_chat::status();
    bool speaking = status.transmitting;
    for (const auto& peer : status.peers) speaking = speaking || peer.speaking;
    if (!speaking) return;
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(size.x - 18.f, size.y * .72f), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(.6f);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("##proximity_voice", nullptr, flags)) {
        if (status.transmitting) {
            ImGui::TextColored(ImVec4(.45f, 1.f, .6f, 1.f), "Microphone on");
            ImGui::ProgressBar(status.microphoneLevel, ImVec2(ImGui::GetFontSize() * 10, 0), "");
        }
        for (const auto& peer : status.peers)
            if (peer.speaking) ImGui::Text("%s (speaking)", peer.name.c_str());
    }
    ImGui::End();
}
} // namespace mecmp::ui::voice
