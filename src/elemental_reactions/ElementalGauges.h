#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"
#include "erf_element.h"

namespace ElementalGauges {
    struct HudGaugeBundle {
        std::span<const char* const> icons;
        std::span<const std::uint32_t> values;
        std::span<const std::uint32_t> colors;
        std::uint32_t singlesBefore{0};
        std::uint32_t singlesAfter{0};
    };

    struct TotalsView {
        std::span<const std::uint8_t> values;
        bool any() const {
            return std::ranges::any_of(values, [](auto v) { return v > 0; });
        }
        std::uint32_t sum() const {
            std::uint32_t s = 0;
            for (auto v : values) s += v;
            return s;
        }
        std::size_t size() const { return values.size(); }
    };

    void Add(RE::Actor* a, ERF_ElementHandle elem, int delta);
    void RegisterStore();
    void ForEachDecayed(const std::function<void(RE::FormID, TotalsView)>& fn);
    std::optional<HudGaugeBundle> PickHudDecayed(RE::FormID id, double nowRt, float nowH);
    void InvalidateStateMultipliers(RE::Actor* a);
    void BuildColorLUTOnce();
}

namespace ElementalGaugesDecay {
    inline constexpr float kGraceSec = 5.0f;
    inline constexpr float kRealDecayPerSec = 15.0f;

    inline float NowHours() { return RE::Calendar::GetSingleton()->GetHoursPassed(); }
    inline float Timescale() {
        float ts = 20.0f;
        if (auto* gs = RE::GameSettingCollection::GetSingleton()) {
            if (auto* s = gs->GetSetting("fTimescale")) {  // NOSONAR: No const assinatura
                ts = s->GetFloat();
            }
        }
        if (ts < 0.001f) ts = 0.001f;
        return ts;
    }
    inline float DecayPerGameHour() { return kRealDecayPerSec * 3600.0f / Timescale(); }
    inline float GraceGameHours() { return (kGraceSec * Timescale()) / 3600.0f; }
}
