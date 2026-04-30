#include "ElementalGaugesHook.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <optional>
#include <shared_mutex>
#include <span>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "../Config.h"
#include "../common/Helpers.h"
#include "../hud/HUDTick.h"
#include "ElementalGauges.h"
#include "ElementalStates.h"
#include "erf_element.h"

using namespace std::chrono;

namespace {
    template <class K, class V, std::size_t N = 64>
    struct StripedMap {
        static_assert((N & (N - 1)) == 0, "N must be power of two");
        struct Stripe {
            std::unordered_map<K, V> map;
            mutable std::shared_mutex mx;
        };
        std::array<Stripe, N> stripes;

        static constexpr std::size_t idx(const K& k) noexcept { return std::hash<K>{}(k) & (N - 1); }

        std::optional<V> get(const K& k) const {
            auto& s = stripes[idx(k)];
            std::shared_lock lk(s.mx);
            auto it = s.map.find(k);
            if (it == s.map.end()) return std::nullopt;
            return it->second;
        }

        template <class F>
        void upsert(const K& k, F&& f) {
            auto& s = stripes[idx(k)];
            std::unique_lock lk(s.mx);
            if (s.map.empty()) s.map.reserve(64);
            f(s.map);
        }

        void erase(const K& k) {
            auto& s = stripes[idx(k)];
            std::unique_lock lk(s.mx);
            s.map.erase(k);
        }

        template <class Pred>
        void erase_if(Pred&& p) {
            for (auto& s : stripes) {
                std::unique_lock lk(s.mx);
                std::erase_if(s.map, std::forward<Pred>(p));
            }
        }
    };

    inline bool IsDisabled() noexcept { return !ERF::GetConfig().enabled.load(std::memory_order_relaxed); }
}

namespace GaugesHook {
    using Elem = ERF_ElementHandle;

    struct EffCtx {
        RE::ActorHandle target;
        std::vector<Elem> elems;
        std::uint16_t uid;
    };

    static StripedMap<std::uint64_t, double, 64> g_lastAccHint;
    static StripedMap<std::uint64_t, std::uint16_t, 64> g_lastAccCarrierUID;
    static StripedMap<const RE::ActiveEffect*, float, 64> g_baseHealthMag;
    static StripedMap<const RE::ActiveEffect*, EffCtx, 64> g_ctx;
    static StripedMap<std::uint64_t, double, 64> g_accum;

    static std::uint64_t KeyActorOnly(const RE::Actor* a) noexcept {
        return static_cast<std::uint64_t>(a ? a->GetFormID() : 0);
    }

    static RE::EffectSetting* LookupMGEF(const char* edid, std::uint32_t formID = 0) {
        if (auto* m = RE::TESForm::LookupByEditorID<RE::EffectSetting>(edid)) return m;
        if (formID) return RE::TESForm::LookupByID<RE::EffectSetting>(formID);
        return nullptr;
    }

    static bool IsGaugeAccCarrier(const RE::EffectSetting* mgef) {
        if (!mgef) return false;

        auto* carrier = ElementalGaugesHook::GaugeAccEffect();
        return carrier && (mgef == carrier);
    }

    static std::vector<Elem> ClassifyElements(const RE::EffectSetting* mgef) {
        std::vector<Elem> out;
        if (!mgef || IsGaugeAccCarrier(mgef)) return out;

        const auto kws = mgef->GetKeywords();
        for (RE::BGSKeyword const* kw : kws) {
            if (!kw) continue;
            if (auto h = ElementRegistry::get().findByKeyword(kw)) {
                if (std::find(out.begin(), out.end(), *h) == out.end()) {
                    out.push_back(*h);
                }
            }
        }
        return out;
    }

    static std::uint64_t MakeKey(const RE::Actor* a, Elem e) {
        const auto hi = static_cast<std::uint64_t>(a ? a->GetFormID() : 0);
        const auto lo = static_cast<std::uint64_t>(e);
        return (hi << 32) | lo;
    }

    class AEApplyRemoveSink final : public RE::BSTEventSink<RE::TESActiveEffectApplyRemoveEvent> {
    public:
        static AEApplyRemoveSink* GetSingleton() {
            static AEApplyRemoveSink s;
            return std::addressof(s);
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESActiveEffectApplyRemoveEvent* e,
                                              RE::BSTEventSource<RE::TESActiveEffectApplyRemoveEvent>*) override {
            if (!e) return RE::BSEventNotifyControl::kContinue;

            const auto uid = e->activeEffectUniqueID;
            const RE::Actor* tgt = e->target ? e->target->As<RE::Actor>() : nullptr;

            if (!e->isApplied) {
                g_ctx.erase_if([&](auto& kv) {
                    const auto* ae = kv.first;
                    const auto& ctx = kv.second;
                    if (ctx.uid != uid) return false;
                    if (tgt) {
                        const RE::Actor* a = ctx.target.get().get();
                        if (!a || a != tgt) return false;
                    }

                    g_baseHealthMag.erase(ae);
                    return true;
                });

                if (tgt) {
                    const auto key = KeyActorOnly(tgt);
                    const auto removedUID = uid;
                    if (auto carrierUID = g_lastAccCarrierUID.get(key); carrierUID && *carrierUID == removedUID) {
                        g_lastAccHint.erase(key);
                        g_lastAccCarrierUID.erase(key);
                    }
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    inline void RegisterAEEventSinkImpl() {
        if (auto* src = RE::ScriptEventSourceHolder::GetSingleton()) {
            src->AddEventSink<RE::TESActiveEffectApplyRemoveEvent>(AEApplyRemoveSink::GetSingleton());
        }
    }

    template <class T>
    struct StartHook {
        static constexpr std::size_t kIndex = 0x14;
        using Fn = void(T*, RE::MagicTarget*);
        static inline Fn* _orig{};

        static bool IsInstantaneous(const RE::EffectSetting* mgef, const T* self) {
            const bool noDurationFlag =
                (mgef && (mgef->data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kNoDuration)));
            const bool zeroDur = (self && self->duration <= 0.01f);
            return noDurationFlag || zeroDur;
        }

        static void thunk(T* self, RE::MagicTarget* mt) {
            if (!IsDisabled() && self) {
                const RE::EffectSetting* mgefPre = self->GetBaseObject();
                RE::Actor* actorPre = AsActor(self->target);
                if (mgefPre && actorPre && !IsGaugeAccCarrier(mgefPre)) {
                    if (auto elemsPre = ClassifyElements(mgefPre); !elemsPre.empty()) {
                        if (mgefPre->data.primaryAV == RE::ActorValue::kHealth &&
                            mgefPre->data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kDetrimental) &&
                            IsInstantaneous(mgefPre, self)) {
                            double healthMult = 1.0;
                            for (auto elem : elemsPre) {
                                if (!elem) continue;
                                healthMult *= ElementalStates::GetHealthMultiplierFor(actorPre, elem);
                            }

                            if (std::abs(healthMult - 1.0) > 1e-6) {
                                self->magnitude *= static_cast<float>(healthMult);
                            }
                        }
                    }
                }
            }
            _orig(self, mt);

            if (IsDisabled()) return;

            const RE::EffectSetting* mgef = self ? self->GetBaseObject() : nullptr;
            RE::Actor* actor = AsActor(self ? self->target : nullptr);
            if (!mgef || !actor) return;

            if (IsGaugeAccCarrier(mgef)) {
                auto acc = static_cast<double>(self->magnitude);
                if (acc <= 0.001 && self->effect) {
                    acc = static_cast<double>(self->effect->effectItem.magnitude);
                }
                if (acc <= 0.001) acc = 1.0;
                const auto k = KeyActorOnly(actor);
                g_lastAccHint.upsert(k, [&](auto& mp) { mp[k] = acc; });
                g_lastAccCarrierUID.upsert(k, [&](auto& mp) { mp[k] = self->usUniqueID; });
                return;
            }

            if (auto elems = ClassifyElements(mgef); !elems.empty()) {
                g_ctx.upsert(self,
                             [&](auto& mp) { mp[self] = EffCtx{actor->CreateRefHandle(), elems, self->usUniqueID}; });

                if (IsInstantaneous(mgef, self)) {
                    double acc = g_lastAccHint.get(KeyActorOnly(actor)).value_or(0.0);
                    if (acc > 0.001) {
                        const int inc = std::max(1, (int)std::lround(acc));
                        ElementalGaugesHook::StartHUDTick();
                        for (auto elem : elems) {
                            ElementalGauges::Add(actor, elem, inc);
                        }
                    }
                }
            }
        }

        static void Install(const char*) {
            REL::Relocation<std::uintptr_t> vtbl{T::VTABLE[0]};
            const auto old = vtbl.write_vfunc(kIndex, &thunk);
            _orig = reinterpret_cast<Fn*>(old);
        }
    };

    template <class T>
    struct UpdateHook {
        static constexpr std::size_t kIndex = 0x04;
        using Fn = void(T*, float);
        static inline Fn* _orig{};

        static void thunk(T* self, float dt) {
            if (IsDisabled()) {
                _orig(self, dt);
                return;
            }

            const auto* mgef = self->GetBaseObject();
            if (mgef && IsGaugeAccCarrier(mgef)) {
                _orig(self, dt);
                return;
            }

            RE::Actor* target = nullptr;
            std::vector<Elem> elems;
            bool haveCtx = false;

            if (auto c = g_ctx.get(self)) {
                haveCtx = true;
                target = c->target.get().get();
                elems = c->elems;
            }

            if (!haveCtx) {
                RE::Actor* actor = AsActor(self->target);
                if (mgef && actor) {
                    auto ce = ClassifyElements(mgef);
                    if (!ce.empty()) {
                        g_ctx.upsert(
                            self, [&](auto& mp) { mp[self] = EffCtx{actor->CreateRefHandle(), ce, self->usUniqueID}; });
                        target = actor;
                        elems = std::move(ce);
                        haveCtx = true;
                    }
                }
            }

            if (!haveCtx || !target || elems.empty()) {
                _orig(self, dt);
                return;
            }

            if (mgef && mgef->data.primaryAV == RE::ActorValue::kHealth &&
                mgef->data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kDetrimental)) {
                const bool noDurationFlag =
                    mgef->data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kNoDuration);
                const bool zeroDur = (self && self->duration <= 0.01f);
                const bool isInstant = noDurationFlag || zeroDur;

                if (!isInstant) {
                    float baseMag = self->magnitude;
                    g_baseHealthMag.upsert(self, [&](auto& mp) {
                        auto it = mp.find(self);
                        if (it == mp.end()) {
                            mp[self] = baseMag;
                        } else {
                            baseMag = it->second;
                        }
                    });

                    double healthMult = 1.0;
                    for (auto elem : elems) {
                        if (elem == 0) {
                            continue;
                        }
                        healthMult *= ElementalStates::GetHealthMultiplierFor(target, elem);
                    }

                    self->magnitude = baseMag * static_cast<float>(healthMult);
                }
            }

            const double accPerSec = g_lastAccHint.get(KeyActorOnly(target)).value_or(0.0);
            if (accPerSec <= 0.001) {
                _orig(self, dt);
                return;
            }

            int totalInc = 0;

            if (dt > 0.0f) {
                for (auto elem : elems) {
                    int inc = 0;

                    const auto key = MakeKey(target, elem);
                    g_accum.upsert(key, [&](auto& mp) {
                        double& acc = mp[key];
                        if (mp.size() == 1) mp.reserve(64);
                        acc += accPerSec * static_cast<double>(dt);
                        const auto whole = static_cast<int>(std::floor(acc));
                        if (whole >= 1) {
                            inc = whole;
                            acc -= whole;
                        }
                    });

                    if (inc > 0) {
                        spdlog::info(
                            "[UpdateHook tick] mgef '{}' (FormID {:08X}) elem={} target={:08X} inc={} accPerSec={:.2f}",
                            mgef->GetName() ? mgef->GetName() : "?", mgef->GetFormID(), elem, target->GetFormID(), inc,
                            accPerSec);
                        totalInc += inc;
                        ElementalGauges::Add(target, elem, inc);
                    }
                }
            }

            if (totalInc > 0) {
                ElementalGaugesHook::StartHUDTick();
            }

            _orig(self, dt);
        }

        static void Install(const char*) {
            REL::Relocation<std::uintptr_t> vtbl{T::VTABLE[0]};
            const auto old = vtbl.write_vfunc(kIndex, &thunk);
            _orig = reinterpret_cast<Fn*>(old);
        }
    };

    using VME = RE::ValueModifierEffect;
    using DVME = RE::DualValueModifierEffect;
    using PVME = RE::PeakValueModifierEffect;

    static void InstallAll() {
        StartHook<VME>::Install("ValueModifierEffect");
        UpdateHook<VME>::Install("ValueModifierEffect");
        StartHook<DVME>::Install("DualValueModifierEffect");
        UpdateHook<DVME>::Install("DualValueModifierEffect");
        StartHook<PVME>::Install("PeakValueModifierEffect");
        UpdateHook<PVME>::Install("PeakValueModifierEffect");
        StartHook<RE::ActiveEffect>::Install("ActiveEffect");
        UpdateHook<RE::ActiveEffect>::Install("ActiveEffect");
    }
}

namespace HookThread {
    static std::atomic<long long> g_lastHookMs{0};
    static std::atomic_bool g_monitorRunning{false};

    static void EnsureWatchdog() {
        if (bool expected = false; !g_monitorRunning.compare_exchange_strong(expected, true)) return;

        std::thread([] {
            const auto timeout = 15s;
            for (;;) {
                std::this_thread::sleep_for(1s);
                const auto last_local = milliseconds(g_lastHookMs.load(std::memory_order_relaxed));
                const auto now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch());
                if (now - last_local > timeout) {
                    HUD::StopHUDTick();
                    g_monitorRunning.store(false, std::memory_order_release);
                    break;
                }
            }
        }).detach();
    }
}

std::atomic_bool& ElementalGaugesHook::AllowHudTickFlag() noexcept {
    static std::atomic_bool flag{false};
    return flag;
}

RE::EffectSetting*& ElementalGaugesHook::GaugeAccEffect() noexcept {
    static RE::EffectSetting* mgef = nullptr;
    return mgef;
}

void ElementalGaugesHook::StartHUDTick() {
    if (!ERF::GetConfig().hudEnabled.load(std::memory_order_relaxed)) {
        return;
    }
    if (!AllowHudTickFlag().load(std::memory_order_acquire)) return;

    const auto now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    HookThread::g_lastHookMs.store(now, std::memory_order_relaxed);
    HUD::StartHUDTick();
    HookThread::EnsureWatchdog();
}

void ElementalGaugesHook::StopHUDTick() { HUD::StopHUDTick(); }
void ElementalGaugesHook::Install() { GaugesHook::InstallAll(); }
void ElementalGaugesHook::RegisterAEEventSink() { GaugesHook::RegisterAEEventSinkImpl(); }

void ElementalGaugesHook::InitCarrierRefs() {
    using namespace GaugesHook;
    static constexpr const char* kPlugin = "ERF_Keywords.esp";
    constexpr std::uint32_t kMgefLocalID = 0x0000C804;

    auto* dh = RE::TESDataHandler::GetSingleton();
    auto*& mgefGaugeAcc = GaugeAccEffect();

    mgefGaugeAcc = dh ? dh->LookupForm<RE::EffectSetting>(kMgefLocalID, kPlugin) : nullptr;
    if (!mgefGaugeAcc) {
        mgefGaugeAcc = LookupMGEF("ERF_GaugeAccEffect");
    }
}