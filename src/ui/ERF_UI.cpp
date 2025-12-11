#include "ERF_UI.h"

#include <fstream>

#include "../overrides/Overrides.h"

void __stdcall ERF_UI::DrawGeneral() {
    ImGui::TextUnformatted("Elemental Reactions Framework");
    ImGui::Separator();

    bool enabled = ERF::GetConfig().enabled.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Enable framework", &enabled)) {
        ERF::GetConfig().enabled.store(enabled, std::memory_order_relaxed);
        ERF::GetConfig().Save();
    }

    ImGui::SameLine();
    ImGui::TextDisabled(enabled ? "(enabled)" : "(disabled)");

    ImGui::Separator();

    if (bool hud = ERF::GetConfig().hudEnabled.load(std::memory_order_relaxed); ImGui::Checkbox("Show HUD", &hud)) {
        ERF::GetConfig().hudEnabled.store(hud, std::memory_order_relaxed);
        ERF::GetConfig().Save();
    }

    int modeIndex = ERF::GetConfig().isSingle.load(std::memory_order_relaxed) ? 0 : 1;
    const char* kModes[] = {"Single", "Mixed"};
    auto count = (int)(sizeof(kModes) / sizeof(kModes[0]));
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::Combo("Gauge mode", &modeIndex, kModes, count)) {
        ERF::GetConfig().isSingle.store(modeIndex == 0, std::memory_order_relaxed);
        ERF::GetConfig().Save();
    }

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Single: An accumulator for each element.\n"
            "Mixed: Combines different elements into the same accumulator.");
    }

    ImGui::Separator();

    float pm = ERF::GetConfig().playerMult.load(std::memory_order_relaxed);
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputFloat("Player gauge multiplier", &pm, 0.1f, 1.0f, "%.3f")) {
        if (pm < 0.f) pm = 0.f;
        ERF::GetConfig().playerMult.store(pm, std::memory_order_relaxed);
        ERF::GetConfig().Save();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Multiplies the player's gauge gain/loss. E.g.: 2.0 = doubles, 0.5 = halves.");

    float nm = ERF::GetConfig().npcMult.load(std::memory_order_relaxed);
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputFloat("NPC gauge multiplier", &nm, 0.1f, 1.0f, "%.3f")) {
        if (nm < 0.f) nm = 0.f;
        ERF::GetConfig().npcMult.store(nm, std::memory_order_relaxed);
        ERF::GetConfig().Save();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Multiplies the NPCs' gauge gain/loss.");

    int maxR = ERF::GetConfig().maxReactionsPerTrigger.load(std::memory_order_relaxed);
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputInt("Max reactions per trigger", &maxR)) {
        if (maxR < 1) maxR = 1;
        ERF::GetConfig().maxReactionsPerTrigger.store(maxR, std::memory_order_relaxed);
        ERF::GetConfig().Save();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("How many reactions can activate at once when the gauge reaches full.");
    }
}

void __stdcall ERF_UI::DrawHUD() {
    ImGui::TextUnformatted("HUD Settings");
    ImGui::Separator();

    ImGui::TextUnformatted("Player");
    ImGui::Separator();

    float x = ERF::GetConfig().playerXPosition.load(std::memory_order_relaxed);
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputFloat("X Position (px)##player", &x, 1.0f, 10.0f, "%.2f")) {
        ERF::GetConfig().playerXPosition.store(x, std::memory_order_relaxed);
        ERF::GetConfig().Save();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Positive values move the gauge to the right. Negative values move it to the left.");

    float y = ERF::GetConfig().playerYPosition.load(std::memory_order_relaxed);
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputFloat("Y Position (px)##player", &y, 1.0f, 10.0f, "%.2f")) {
        ERF::GetConfig().playerYPosition.store(y, std::memory_order_relaxed);
        ERF::GetConfig().Save();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Positive values move the gauge up. Negative values move it down.");

    float sc = ERF::GetConfig().playerScale.load(std::memory_order_relaxed);
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputFloat("Scale##player", &sc, 0.05f, 0.25f, "%.3f")) {
        if (sc < 0.f) sc = 0.f;
        ERF::GetConfig().playerScale.store(sc, std::memory_order_relaxed);
        ERF::GetConfig().Save();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Gauge size.");

    bool horiz = ERF::GetConfig().playerHorizontal.load(std::memory_order_relaxed);
    int idx = horiz ? 0 : 1;
    const char* opts[] = {"horizontally", "vertically"};
    ImGui::SetNextItemWidth(260.0f);
    if (ImGui::Combo("Align gauges:##player", &idx, opts, (int)(sizeof(opts) / sizeof(opts[0])))) {
        ERF::GetConfig().playerHorizontal.store(idx == 0, std::memory_order_relaxed);
        ERF::GetConfig().Save();
    }

    float psp = ERF::GetConfig().playerSpacing.load(std::memory_order_relaxed);
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputFloat("Spacing (px)##player", &psp, 1.0f, 5.0f, "%.1f")) {
        if (psp < 0.f) psp = 0.f;
        ERF::GetConfig().playerSpacing.store(psp, std::memory_order_relaxed);
        ERF::GetConfig().Save();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Distance between the player's gauges in pixels.");
    }

    ImGui::Spacing();
    ImGui::Separator();

    ImGui::TextUnformatted("NPC");
    ImGui::Separator();

    float nx = ERF::GetConfig().npcXPosition.load(std::memory_order_relaxed);
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputFloat("X Position (px)##npc", &nx, 1.0f, 10.0f, "%.2f")) {
        ERF::GetConfig().npcXPosition.store(nx, std::memory_order_relaxed);
        ERF::GetConfig().Save();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Positive values move the gauge to the right. Negative values move it to the left.");

    float ny = ERF::GetConfig().npcYPosition.load(std::memory_order_relaxed);
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputFloat("Y Position (px)##npc", &ny, 1.0f, 10.0f, "%.2f")) {
        ERF::GetConfig().npcYPosition.store(ny, std::memory_order_relaxed);
        ERF::GetConfig().Save();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Positive values move the gauge up. Negative values move it down.");

    float nsc = ERF::GetConfig().npcScale.load(std::memory_order_relaxed);
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputFloat("Scale##npc", &nsc, 0.05f, 0.25f, "%.3f")) {
        if (nsc < 0.f) nsc = 0.f;
        ERF::GetConfig().npcScale.store(nsc, std::memory_order_relaxed);
        ERF::GetConfig().Save();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Gauge size.");

    bool nhoriz = ERF::GetConfig().npcHorizontal.load(std::memory_order_relaxed);
    int nidx = nhoriz ? 0 : 1;
    const char* nopts[] = {"horizontally", "vertically"};
    ImGui::SetNextItemWidth(260.0f);
    if (ImGui::Combo("Align gauges:##npc", &nidx, nopts, (int)(sizeof(nopts) / sizeof(nopts[0])))) {
        ERF::GetConfig().npcHorizontal.store(nidx == 0, std::memory_order_relaxed);
        ERF::GetConfig().Save();
    }

    float nsp = ERF::GetConfig().npcSpacing.load(std::memory_order_relaxed);
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputFloat("Spacing (px)##npc", &nsp, 1.0f, 5.0f, "%.1f")) {
        if (nsp < 0.f) nsp = 0.f;
        ERF::GetConfig().npcSpacing.store(nsp, std::memory_order_relaxed);
        ERF::GetConfig().Save();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Distance between the npc's gauges in pixels.");
    }
}

struct _SpellRow {
    RE::SpellItem* sp{};
    std::string editorID;
    std::string plugin;
    std::string formHex;
    std::string name;
    float magnitude{};
};

static void _SaveSpellOverridesJSON(const std::vector<_SpellRow>& rows, float defaultMagnitude) {
    if (!ERF::Overrides::EnsureOverridesFolder()) return;

    const auto& path = ERF::Overrides::OverridesPath();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return;

    auto esc = [](const std::string& s) {
        std::string r;
        r.reserve(s.size() + 8);
        for (unsigned char c : s) {
            if (c == '\\' || c == '\"') {
                r.push_back('\\');
                r.push_back(char(c));
            } else if (c == '\n') {
                r += "\\n";
            } else {
                r.push_back(char(c));
            }
        }
        return r;
    };

    out << "{\n";
    out << "  \"version\": 1,\n";
    out << "  \"defaultMagnitude\": " << defaultMagnitude << ",\n";
    out << "  \"spells\": [\n";

    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        out << "    {"
            << "\"plugin\":\"" << esc(r.plugin) << "\","
            << "\"formID\":\"" << r.formHex << "\","
            << "\"magnitude\":" << r.magnitude << ","
            << "\"editorID\":\"" << esc(r.editorID) << "\","
            << "\"name\":\"" << esc(r.name) << "\""
            << "}";
        if (i + 1 < rows.size()) out << ",";
        out << "\n";
    }

    out << "  ]\n"
        << "}\n";
}

void __stdcall ERF_UI::DrawEditGauge() {
    static bool initialized = false;
    static std::vector<_SpellRow> rows;
    static char filterBuf[96]{};
    static float defaultMagnitude = 10.0f;

    if (ImGui::Button("Refresh list")) initialized = false;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##filter", "filter by name or formID...", filterBuf, sizeof(filterBuf));

    ImGui::Separator();

    if (!initialized) {
        initialized = true;
        rows.clear();

        auto list = ERF::Overrides::ScanAllSpellsWithKeyword();
        rows.reserve(list.size());

        for (auto* sp : list) {
            if (!sp) continue;

            ERF::Overrides::EnsureGaugeEffect(sp, defaultMagnitude);

            _SpellRow r;
            r.sp = sp;
            r.editorID = ERF::Overrides::GetEditorID(sp);
            r.plugin = ERF::Overrides::OwningPlugin(sp);
            r.formHex = ERF::Overrides::FormIDHex(ERF::Overrides::RawFormID(sp));
            r.name = ERF::Overrides::GetDisplayName(sp);

            if (auto const* eff = ERF::Overrides::FindGaugeEffect(sp))
                r.magnitude = eff->effectItem.magnitude;
            else
                r.magnitude = defaultMagnitude;

            rows.push_back(std::move(r));
        }
    }

    auto passFilter = [&](const _SpellRow& r) {
        if (filterBuf[0] == '\0') return true;

        auto toLowerInPlace = [](std::string& s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        };

        std::string f = filterBuf;
        std::string edid = r.editorID;
        std::string plugin = r.plugin;
        std::string form = r.formHex;
        std::string name = r.name;

        toLowerInPlace(f);
        toLowerInPlace(edid);
        toLowerInPlace(plugin);
        toLowerInPlace(form);
        toLowerInPlace(name);

        if (edid.contains(f)) return true;
        if (plugin.contains(f)) return true;
        if (form.contains(f)) return true;
        if (!name.empty() && name.contains(f)) return true;

        return false;
    };

    if (ImGui::BeginTable("##erf_spells", 3,
                          ImGuiMCP::ImGuiTableFlags_Resizable | ImGuiMCP::ImGuiTableFlags_Borders |
                              ImGuiMCP::ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("EditorID", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Plugin", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 200.0f);
        ImGui::TableSetupColumn("Magnitude", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 200.0f);
        ImGui::TableHeadersRow();

        for (auto& r : rows) {
            if (!passFilter(r)) continue;
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(r.editorID.empty() ? "<no EDID>" : r.editorID.c_str());
            if (!r.name.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("  ⓘ");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.name.c_str());
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(r.plugin.c_str());

            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-1.0f);
            float m = r.magnitude;
            if (ImGui::InputFloat(std::string("##mag_").append(r.formHex).c_str(), &m, 0.1f, 1.0f, "%.3f")) {
                if (m < 0.f) m = 0.f;
                r.magnitude = m;
                ERF::Overrides::SetGaugeMagnitude(r.sp, r.magnitude);
            }
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    if (ImGui::Button("Save to JSON")) {
        _SaveSpellOverridesJSON(rows, defaultMagnitude);
        ImGui::SameLine();
        ImGui::TextDisabled("Saved to: %s", ERF::Overrides::OverridesPath().string().c_str());
    }
}

void ERF_UI::Register() {
    if (!SKSEMenuFramework::IsInstalled()) return;

    SKSEMenuFramework::SetSection("Elemental Reactions");

    SKSEMenuFramework::AddSectionItem("General", ERF_UI::DrawGeneral);
    SKSEMenuFramework::AddSectionItem("HUD", ERF_UI::DrawHUD);
    SKSEMenuFramework::AddSectionItem("Edit Gauges", ERF_UI::DrawEditGauge);
}
