#include "gui/TestsTab.h"
#include "gui/App.h"
#include "imgui.h"

void TestsTab::render()
{
    // Run button
    bool canRun = app_.connected && !app_.testsRunning;
    if (!canRun) ImGui::BeginDisabled();
    if (ImGui::Button("Run All Tests")) {
        app_.runTests();
    }
    if (!canRun) ImGui::EndDisabled();

    ImGui::SameLine();
    if (app_.testsRunning) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Running...");
    } else if (!app_.connected) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Connect to device first");
    }

    // Join completed test thread
    if (!app_.testsRunning && app_.testThread.joinable()) {
        app_.testThread.join();
    }

    ImGui::Separator();

    // Results table
    if (ImGui::BeginTable("TestResults", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {

        ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("Test", ImGuiTableColumnFlags_WidthFixed, 200.0f);
        ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Message");
        ImGui::TableHeadersRow();

        for (const auto& r : app_.testResults) {
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            switch (r.result) {
                case TestResult::Pass:
                    ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "PASS");
                    break;
                case TestResult::Fail:
                    ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.2f, 1.0f), "FAIL");
                    break;
                case TestResult::Skip:
                    ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f), "SKIP");
                    break;
            }

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.testName.c_str());

            ImGui::TableNextColumn();
            ImGui::Text("%.0f ms", r.durationMs);

            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", r.message.c_str());
        }

        // Show registered tests if no results yet
        if (app_.testResults.empty()) {
            for (const auto& tc : app_.testRunner.tests()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "--");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(tc.name.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("--");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(tc.description.c_str());
            }
        }

        ImGui::EndTable();
    }

    // Summary
    if (!app_.testResults.empty()) {
        int pass = 0, fail = 0, skip = 0;
        for (const auto& r : app_.testResults) {
            switch (r.result) {
                case TestResult::Pass: pass++; break;
                case TestResult::Fail: fail++; break;
                case TestResult::Skip: skip++; break;
            }
        }
        ImGui::Text("%d passed, %d failed, %d skipped", pass, fail, skip);
    }
}
