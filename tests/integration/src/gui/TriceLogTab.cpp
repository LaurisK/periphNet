#include "gui/TriceLogTab.h"
#include "gui/App.h"
#include "imgui.h"

#include <algorithm>

void TriceLogTab::render()
{
    // Toolbar
    ImGui::Checkbox("Auto-scroll", &autoScroll_);
    ImGui::SameLine();
    ImGui::InputText("Filter", filterBuf_, sizeof(filterBuf_));
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        app_.logBuffer.clear();
        lastIndex_ = 0;
    }

    ImGui::Separator();

    // Log area
    ImGui::BeginChild("LogScroll", ImVec2(0, 0), false,
                      ImGuiWindowFlags_HorizontalScrollbar);

    auto lines = app_.logBuffer.allLines();
    std::string filter(filterBuf_);

    for (const auto& line : lines) {
        if (!filter.empty() && line.find(filter) == std::string::npos) {
            continue;
        }
        ImGui::TextUnformatted(line.c_str());
    }

    if (autoScroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.0f) {
        ImGui::SetScrollHereY(1.0f);
    }

    ImGui::EndChild();
}
