#include "gui/ConsoleTab.h"
#include "gui/App.h"
#include "imgui.h"

void ConsoleTab::render()
{
    if (!app_.connected) {
        ImGui::TextColored(ImVec4(0.8f, 0.2f, 0.2f, 1.0f), "Not connected");
        return;
    }

    // Command history
    ImGui::BeginChild("ConsoleHistory", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 4), true);
    for (const auto& cmd : app_.consoleHistory) {
        ImGui::TextUnformatted(cmd.c_str());
    }
    if (!app_.consoleHistory.empty()) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    // Input line
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue;
    if (focusInput_) {
        ImGui::SetKeyboardFocusHere();
        focusInput_ = false;
    }

    bool sent = ImGui::InputText("Command", inputBuf_, sizeof(inputBuf_), flags);
    ImGui::SameLine();
    sent |= ImGui::Button("Send");

    if (sent && inputBuf_[0] != '\0') {
        std::string cmd(inputBuf_);
        app_.consoleHistory.push_back("> " + cmd);
        app_.device->sendCommand(cmd);
        inputBuf_[0] = '\0';
        focusInput_ = true;
    }
}
