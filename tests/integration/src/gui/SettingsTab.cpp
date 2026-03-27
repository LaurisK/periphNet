#include "gui/SettingsTab.h"
#include "gui/App.h"
#include "imgui.h"

void SettingsTab::render()
{
    ImGui::Text("Transport Mode:");
    ImGui::RadioButton("USB (CDC)", &modeIdx_, 0); ImGui::SameLine();
    ImGui::RadioButton("UDP", &modeIdx_, 1); ImGui::SameLine();
    ImGui::RadioButton("Serial", &modeIdx_, 2);

    ImGui::Separator();

    switch (modeIdx_) {
    case 0: // USB
        ImGui::InputText("USB Port", usbPort_, sizeof(usbPort_));
        break;
    case 1: // UDP
        ImGui::InputInt("UDP Port", &udpPort_);
        ImGui::InputText("Command Port", udpCmdPort_, sizeof(udpCmdPort_));
        break;
    case 2: // Serial
        ImGui::InputText("Trice Port", serialTricePort_, sizeof(serialTricePort_));
        ImGui::InputInt("Trice Baud", &serialTriceBaud_);
        ImGui::InputText("Command Port", serialCmdPort_, sizeof(serialCmdPort_));
        ImGui::InputInt("Command Baud", &serialCmdBaud_);
        break;
    }

    ImGui::Separator();

    // Apply config before connect
    auto applyConfig = [&]() {
        switch (modeIdx_) {
        case 0:
            app_.config.mode = TransportMode::Usb;
            app_.config.usbPort = usbPort_;
            break;
        case 1:
            app_.config.mode = TransportMode::Udp;
            app_.config.udpPort = static_cast<uint16_t>(udpPort_);
            app_.config.udpCommandPort = udpCmdPort_;
            break;
        case 2:
            app_.config.mode = TransportMode::Serial;
            app_.config.serialTricePort = serialTricePort_;
            app_.config.serialTriceBaud = serialTriceBaud_;
            app_.config.serialCommandPort = serialCmdPort_;
            app_.config.serialCommandBaud = serialCmdBaud_;
            break;
        }
    };

    if (!app_.connected) {
        if (ImGui::Button("Connect")) {
            applyConfig();
            app_.connect();
        }
    } else {
        if (ImGui::Button("Disconnect")) {
            app_.disconnect();
        }
    }

    ImGui::SameLine();

    if (app_.connected) {
        ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "%s", app_.statusMessage.c_str());
    } else {
        ImGui::TextColored(ImVec4(0.8f, 0.2f, 0.2f, 1.0f), "%s", app_.statusMessage.c_str());
    }
}
