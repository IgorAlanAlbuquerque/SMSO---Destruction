#pragma once
#include "Config.h"
#include "SKSEMenuFramework.h"

namespace ImGui = ImGuiMCP;
using ImGuiMCP::ImVec2;

namespace ERF_UI {
    void __stdcall DrawGeneral();
    void __stdcall DrawHUD();
    void __stdcall DrawEditGauge();
    void Register();
}
