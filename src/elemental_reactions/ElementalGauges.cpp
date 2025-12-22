#include "ElementalGauges.h"

#include <ankerl/unordered_dense.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <ranges>
#include <type_traits>
#include <utility>
#include <vector>

#include "../Config.h"
#include "../common/Helpers.h"
#include "../common/PluginSerialization.h"
#include "../hud/HUDTick.h"
#include "../hud/InjectHUD.h"
#include "ElementalStates.h"
#include "erf_preeffect.h"

using namespace ElementalGaugesDecay;
using Elem = ERF_ElementHandle;

namespace Gauges {
    inline constexpr std::uint32_t kRecordID = FOURCC('G', 'A', 'U', 'V');
    inline constexpr std::uint32_t kVersion = 4;
    constexpr bool kReserveZero = true;

    struct Entry {
        std::vector<std::uint8_t> v;
        std::vector<float> lastHitH;
        std::vector<float> lastEvalH;
        std::vector<float> blockUntilH;
        std::vector<double> blockUntilRtS;

        std::vector<double> reactCdRtS;
        std::vector<float> reactCdH;
        std::vector<std::uint8_t> inReaction;

        std::vector<std::uint8_t> preActive;
        std::vector<float> preIntensity;
        std::vector<double> preExpireRtS;
        std::vector<float> preExpireH;

        std::vector<double> effMult;
        bool effDirty = true;

        bool sized = false;
        std::uint64_t presentMask = 0;
        std::vector<ERF_ElementHandle> presentList;
        int sumAll = 0;
        int sumMix = 0;
        std::vector<std::uint16_t> posInList;
    };

    using Map = ankerl::unordered_dense::map<RE::FormID, Entry>;
    inline Map& state() noexcept {
        static Map m;
        if (m.bucket_count() == 0) m.reserve(512);
        return m;
    }

    inline std::size_t firstIndex() { return kReserveZero ? 1u : 0u; }
    inline std::size_t idx(ERF_ElementHandle h) { return static_cast<std::size_t>(h == 0 ? 1 : h); }
    inline std::size_t idxReact(ERF_ReactionHandle r) { return static_cast<std::size_t>(r == 0 ? 1 : r); }
    inline std::size_t idxPre(ERF_PreEffectHandle p) { return static_cast<std::size_t>(p == 0 ? 1 : p); }

    inline void initEntryDenseIfNeeded(Entry& e) {
        if (e.sized) return;
        const auto& caps = ERF::API::Caps();

        const std::size_t nE = static_cast<std::size_t>(caps.numElements) + 1;
        const std::size_t nR = static_cast<std::size_t>(caps.numReactions) + 1;
        const std::size_t nP = static_cast<std::size_t>(caps.numPreEffects) + 1;

        e.v.assign(nE, 0);
        e.lastHitH.assign(nE, 0.f);
        e.lastEvalH.assign(nE, 0.f);
        e.blockUntilH.assign(nE, 0.f);
        e.blockUntilRtS.assign(nE, 0.0);
        e.effMult.assign(nE, 1.0);

        e.reactCdRtS.assign(nR, 0.0);
        e.reactCdH.assign(nR, 0.f);
        e.inReaction.assign(nR, 0u);

        e.preActive.assign(nP, 0u);
        e.preIntensity.assign(nP, 0.f);
        e.preExpireRtS.assign(nP, 0.0);
        e.preExpireH.assign(nP, 0.f);

        e.effDirty = true;
        e.sized = true;

        e.presentMask = 0;
        e.presentList.clear();
        e.sumAll = 0;
        e.sumMix = 0;
        e.posInList.assign(nE, 0xFFFF);
    }

    struct DecaySnapshot {
        float ratePerHour;
        float graceHours;
    };

    inline DecaySnapshot SnapshotDecay() {
        const float ts = Timescale();
        DecaySnapshot s{kRealDecayPerSec * 3600.0f / ts, (kGraceSec * ts) / 3600.0f};
        return s;
    }

    inline ERF_ElementHandle handleFromIndex(std::size_t idx) { return static_cast<ERF_ElementHandle>(idx); }

    inline void makePresent(Entry& e, std::size_t i) {
        if (i < firstIndex()) return;
        if (i >= e.posInList.size()) return;
        if (e.posInList[i] != 0xFFFF) return;
        const ERF_ElementHandle h = handleFromIndex(i);

        if (const auto bit = static_cast<unsigned>(h - 1); bit < 64) e.presentMask |= (UINT64_C(1) << bit);
        e.posInList[i] = static_cast<std::uint16_t>(e.presentList.size());
        e.presentList.push_back(h);
    }

    inline void dropPresent(Entry& e, std::size_t i) {
        if (i < firstIndex()) return;
        if (i >= e.posInList.size()) return;
        auto pos = e.posInList[i];
        if (pos == 0xFFFF) return;
        const auto lastIdx = static_cast<std::uint16_t>(e.presentList.size() - 1);
        const ERF_ElementHandle h = handleFromIndex(i);

        if (const auto bit = static_cast<unsigned>(h - 1); bit < 64) e.presentMask &= ~(UINT64_C(1) << bit);
        if (pos != lastIdx) {
            const ERF_ElementHandle lastH = e.presentList[lastIdx];
            e.presentList[pos] = lastH;
            const std::size_t lastI = idx(lastH);
            if (lastI < e.posInList.size()) e.posInList[lastI] = pos;
        }
        e.presentList.pop_back();
        e.posInList[i] = 0xFFFF;
    }

    inline void onValChange(Entry& e, std::size_t i, int before, int after) {
        if (i < firstIndex()) return;

        const int delta = after - before;

        e.sumAll += delta;
        if (e.sumAll < 0) e.sumAll = 0;

        if (delta != 0) {
            const ERF_ElementHandle h = handleFromIndex(i);
            if (h != 0) {
                auto const& ER = ElementRegistry::get();
                if (const ERF_ElementDesc* d = ER.get(h)) {
                    if (!d->noMixInMixedMode) {
                        e.sumMix += delta;
                        if (e.sumMix < 0) e.sumMix = 0;
                    }
                } else {
                    e.sumMix += delta;
                    if (e.sumMix < 0) e.sumMix = 0;
                }
            }
        }

        const bool was = (before > 0);
        const bool is = (after > 0);
        if (was == is) return;
        if (is)
            makePresent(e, i);
        else
            dropPresent(e, i);
    }

    template <class F>
    inline void tickOne(Entry& e, std::size_t i, float nowH, const DecaySnapshot& snap, F&& onChanged) {
        auto& val = e.v[i];
        auto& eval = e.lastEvalH[i];
        const float hit = e.lastHitH[i];

        if (val == 0) {
            eval = nowH;
            return;
        }

        const float rate = snap.ratePerHour;
        if (rate <= 0.f) {
            eval = nowH;
            return;
        }

        const float graceEnd = hit + snap.graceHours;
        if (nowH <= graceEnd) return;

        const float startH = std::max(eval, graceEnd);
        const float elapsedH = nowH - startH;
        if (elapsedH <= 0.f) return;

        const float decF = elapsedH * rate;
        const auto decI = static_cast<int>(decF);
        if (decI <= 0) return;

        const int before = static_cast<int>(val);
        int next = before - decI;
        if (next < 0) next = 0;

        if (next < before) {  // <- aqui está o “new < old”
            val = static_cast<std::uint8_t>(next);
            onValChange(e, i, before, next);
            onChanged(i, static_cast<std::uint8_t>(before), static_cast<std::uint8_t>(next));
        }

        const float rem = decF - static_cast<float>(decI);
        eval = nowH - (rem / rate);
    }

    inline void tickOne(Entry& e, std::size_t i, float nowH, const DecaySnapshot& snap) {
        tickOne(e, i, nowH, snap, [](std::size_t, std::uint8_t, std::uint8_t) {});
    }

    template <class F>
    inline void tickAll(Entry& e, float nowH, const DecaySnapshot& snap, F&& onChanged) {
        const auto n = e.v.size();
        for (std::size_t i = firstIndex(); i < n; ++i) {
            tickOne(e, i, nowH, snap, onChanged);
        }
    }

    inline void tickAll(Entry& e, float nowH, const DecaySnapshot& snap) {
        tickAll(e, nowH, snap, [](std::size_t, std::uint8_t, std::uint8_t) {});
    }

    inline void rebuildPresence(Entry& e) {
        e.presentMask = 0;
        e.presentList.clear();
        e.sumAll = 0;
        e.sumMix = 0;
        e.posInList.assign(e.v.size(), 0xFFFF);

        auto const& ER = ElementRegistry::get();

        for (std::size_t i = firstIndex(); i < e.v.size(); ++i) {
            const int v = e.v[i];
            if (v > 0) {
                e.sumAll += v;

                const ERF_ElementHandle h = handleFromIndex(i);
                if (const ERF_ElementDesc* d = ER.get(h); !d || !d->noMixInMixedMode) {
                    e.sumMix += v;
                }

                makePresent(e, i);
            }
        }
    }
}

namespace {
    thread_local std::vector<std::uint32_t> TL_vals32;
    thread_local std::vector<std::uint32_t> TL_cols32;
    thread_local std::vector<const char*> TL_accumIcons;
    thread_local std::vector<ERF_ElementHandle> TL_elemsNZ;
    static std::vector<std::uint32_t> g_colorLUT;
    constexpr const char* kFallbackReactionIcon = "ERF_ICON__erf_core__fallback";

    inline double NowRealSeconds() {
        using clock = std::chrono::steady_clock;
        static const auto t0 = clock::now();
        return std::chrono::duration<double>(clock::now() - t0).count();
    }

    inline void ApplyElementLocksForReaction(Gauges::Entry& e, const std::vector<ERF_ElementHandle>& elems,
                                             float seconds) {
        if (seconds <= 0.f) return;

        const double nowRt = NowRealSeconds();
        const double untilRt = nowRt + seconds;

        for (ERF_ElementHandle h : elems) {
            const std::size_t idx = Gauges::idx(h);
            if (idx >= e.v.size()) continue;
            e.blockUntilRtS[idx] = std::max(e.blockUntilRtS[idx], untilRt);
        }
    }

    inline void SetReactionCooldown(Gauges::Entry& e, ERF_ReactionHandle rh, float cooldownSeconds) {
        if (cooldownSeconds <= 0.f || rh == 0) return;

        const std::size_t ri = Gauges::idxReact(rh);

        const double untilRt = NowRealSeconds() + static_cast<double>(cooldownSeconds);
        if (ri < e.reactCdRtS.size()) e.reactCdRtS[ri] = std::max(e.reactCdRtS[ri], untilRt);
    }

    bool MaybeTriggerPreEffectsFor(RE::Actor* a, Gauges::Entry& e, ERF_ElementHandle elem, std::uint8_t gaugeNow) {
        if (!a || elem == 0) return false;

        const auto list = PreEffectRegistry::get().listByElement(elem);
        if (list.empty()) return false;

        const double nowRt = NowRealSeconds();
        const float nowH = NowHours();

        bool any = false;

        for (auto ph : list) {
            const auto* pd = PreEffectRegistry::get().get(ph);
            if (!pd || pd->element != elem) continue;

            const std::size_t pi = Gauges::idxPre(ph);
            if (pi >= e.preActive.size()) continue;

            if (const bool above = (gaugeNow >= pd->minGauge); !above) {
                if (e.preActive[pi]) {
                    e.preActive[pi] = 0u;
                    e.preIntensity[pi] = 0.f;
                    e.preExpireRtS[pi] = 0.0;
                    e.preExpireH[pi] = 0.f;

                    if (pd->cb) {
                        if (auto* tasks = SKSE::GetTaskInterface()) {
                            RE::ActorHandle h = a->CreateRefHandle();
                            auto cb = pd->cb;
                            auto user = pd->user;
                            const auto passElem = elem;
                            tasks->AddTask([h, cb, user, passElem]() {
                                if (auto actorPtr = h.get().get()) {
                                    cb(actorPtr, passElem, 0, 0.0f, user);
                                }
                            });
                        }
                    }
                    any = true;
                }
                continue;
            }

            float intensity = pd->baseIntensity + pd->scalePerPoint * static_cast<float>(gaugeNow - pd->minGauge);
            if (intensity < pd->minIntensity) intensity = pd->minIntensity;
            if (intensity > pd->maxIntensity) intensity = pd->maxIntensity;

            bool needApply = false;

            if (!e.preActive[pi]) {
                needApply = true;
            } else {
                if (const float last = e.preIntensity[pi]; std::fabs(last - intensity) > 1e-3f) {
                    needApply = true;
                }

                if (!needApply && pd->durationSeconds > 0.0f) {
                    constexpr double marginRt = 0.20;
                    constexpr float marginH = 0.20f / 3600.0f;
                    if (pd->durationIsRealTime) {
                        if (nowRt + marginRt >= e.preExpireRtS[pi]) needApply = true;
                    } else {
                        if (nowH + marginH >= e.preExpireH[pi]) needApply = true;
                    }
                }
            }

            if (!needApply) continue;

            if (pd->cb) {
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    RE::ActorHandle h = a->CreateRefHandle();
                    auto cb = pd->cb;
                    auto user = pd->user;
                    const auto passElem = elem;
                    const auto passGauge = gaugeNow;
                    const auto passIntensity = intensity;
                    tasks->AddTask([h, cb, user, passElem, passGauge, passIntensity]() {
                        if (auto actorPtr = h.get().get()) {
                            cb(actorPtr, passElem, passGauge, passIntensity, user);
                        }
                    });
                } else {
                    pd->cb(a, elem, gaugeNow, intensity, pd->user);
                }
            }

            e.preActive[pi] = 1u;
            e.preIntensity[pi] = intensity;

            if (pd->durationSeconds > 0.0f) {
                if (pd->durationIsRealTime) {
                    const double untilRt = nowRt + static_cast<double>(pd->durationSeconds);
                    e.preExpireRtS[pi] = std::max(e.preExpireRtS[pi], untilRt);
                } else {
                    const float untilH = nowH + static_cast<float>(pd->durationSeconds / 3600.0);
                    e.preExpireH[pi] = std::max(e.preExpireH[pi], untilH);
                }
            }

            any = true;
        }

        return any;
    }

    bool TriggerReaction(RE::Actor* a, Gauges::Entry& e, ERF_ElementHandle elem) {
        if (!a) {
            return false;
        }
        if (e.sumAll <= 0) {
            return false;
        }

        auto const& RR = ReactionRegistry::get();

        int maxCount = ERF::GetConfig().maxReactionsPerTrigger.load(std::memory_order_relaxed);
        if (maxCount < 1) {
            maxCount = 1;
        }

        std::vector<ERF_PickBestInfo> picks;
        picks.reserve(static_cast<std::size_t>(maxCount));

        if (elem == 0) {
            const int sum = e.sumMix;
            if (sum <= 0) {
                return false;
            }

            auto const& ER = ElementRegistry::get();

            std::vector<std::uint8_t> totals(e.v.size(), 0);
            std::vector<ERF_ElementHandle> presentMix;
            presentMix.reserve(e.presentList.size());

            for (ERF_ElementHandle h : e.presentList) {
                if (const ERF_ElementDesc* d = ER.get(h); d && d->noMixInMixedMode) {
                    continue;
                }

                const std::size_t idx = Gauges::idx(h);
                if (idx >= e.v.size()) {
                    continue;
                }

                const std::uint8_t v = e.v[idx];
                if (!v) {
                    continue;
                }

                totals[idx] = v;
                presentMix.push_back(h);
            }

            if (presentMix.empty()) {
                return false;
            }

            const float invSum = 1.0f / static_cast<float>(sum);

            RR.pickBestFastMulti(totals, std::span<const ERF_ElementHandle>(presentMix.data(), presentMix.size()), sum,
                                 invSum, maxCount, picks);
            if (picks.empty()) {
                const float nowH = NowHours();
                auto const& ER2 = ElementRegistry::get();
                bool clearedAny = false;

                for (std::size_t i = Gauges::firstIndex(); i < e.v.size(); ++i) {
                    const int before = e.v[i];
                    if (!before) {
                        continue;
                    }

                    const ERF_ElementHandle h2 = Gauges::handleFromIndex(i);

                    if (const ERF_ElementDesc* d2 = ER2.get(h2); d2 && d2->noMixInMixedMode) {
                        continue;
                    }

                    e.v[i] = 0;
                    e.lastHitH[i] = nowH;
                    e.lastEvalH[i] = nowH;

                    onValChange(e, i, before, 0);
                    (void)MaybeTriggerPreEffectsFor(a, e, h2, 0);
                    clearedAny = true;
                }

                return clearedAny;
            }

            const float nowH = NowHours();

            bool clearedAll = false;
            bool any = false;

            for (const auto& info : picks) {
                const ERF_ReactionHandle rh = info.handle;
                if (!rh) {
                    continue;
                }

                const ERF_ReactionDesc* r = RR.get(rh);
                if (!r) {
                    continue;
                }

                if (!clearedAll) {
                    for (std::size_t i = Gauges::firstIndex(); i < e.v.size(); ++i) {
                        const int before = e.v[i];
                        if (!before) {
                            continue;
                        }

                        const ERF_ElementHandle h = Gauges::handleFromIndex(i);

                        if (const ERF_ElementDesc* d = ER.get(h); d && d->noMixInMixedMode) {
                            continue;
                        }

                        e.v[i] = 0;
                        e.lastHitH[i] = nowH;
                        e.lastEvalH[i] = nowH;

                        onValChange(e, i, before, 0);
                        (void)MaybeTriggerPreEffectsFor(a, e, h, 0);
                    }

                    clearedAll = true;
                }

                ApplyElementLocksForReaction(e, r->elements, r->elementLockoutSeconds);

                SetReactionCooldown(e, rh, r->cooldownSeconds);

                const float durS =
                    (r->elementLockoutSeconds > 0.f) ? r->elementLockoutSeconds : std::max(0.5f, r->cooldownSeconds);

                InjectHUD::BeginReaction(a, rh, durS);

                if (r->cb) {
                    if (auto* tasks = SKSE::GetTaskInterface()) {
                        RE::ActorHandle hActor = a->CreateRefHandle();
                        auto cb = r->cb;
                        void* user = r->user;
                        tasks->AddTask([hActor, cb, user]() {
                            if (auto actorNi = hActor.get()) {
                                if (auto* target = actorNi.get()) {
                                    ERF_ReactionContext ctx{};
                                    ctx.target = target;
                                    cb(ctx, user);
                                }
                            }
                        });
                    } else {
                        ERF_ReactionContext ctx{};
                        ctx.target = a;
                        r->cb(ctx, r->user);
                    }
                }

                any = true;
            }

            return any;
        }

        const std::size_t idx = Gauges::idx(elem);
        const int v = (idx < e.v.size()) ? static_cast<int>(e.v[idx]) : 0;
        if (v <= 0) {
            return false;
        }

        std::vector<std::uint8_t> totals(e.v.size(), 0);
        totals[idx] = static_cast<std::uint8_t>(v);

        std::vector<ERF_ElementHandle> present;
        present.reserve(1);
        present.push_back(elem);

        const int sum = v;
        const float invSum = 1.0f / static_cast<float>(sum);

        RR.pickBestFastMulti(totals, std::span<const ERF_ElementHandle>(present.data(), present.size()), sum, invSum,
                             maxCount, picks);
        const float nowH = NowHours();
        const int beforeVal = v;

        if (picks.empty()) {
            e.v[idx] = 0;
            e.lastHitH[idx] = nowH;
            e.lastEvalH[idx] = nowH;
            Gauges::onValChange(e, idx, beforeVal, 0);
            (void)MaybeTriggerPreEffectsFor(a, e, elem, 0);
            return true;
        }

        bool clearedElem = false;
        bool any = false;

        for (const auto& info : picks) {
            const ERF_ReactionHandle rh = info.handle;
            if (!rh) {
                continue;
            }

            const ERF_ReactionDesc* r = RR.get(rh);
            if (!r) {
                continue;
            }

            if (!clearedElem) {
                e.v[idx] = 0;
                e.lastHitH[idx] = nowH;
                e.lastEvalH[idx] = nowH;
                Gauges::onValChange(e, idx, beforeVal, 0);
                (void)MaybeTriggerPreEffectsFor(a, e, elem, 0);
                clearedElem = true;
            }

            ApplyElementLocksForReaction(e, r->elements, r->elementLockoutSeconds);

            SetReactionCooldown(e, rh, r->cooldownSeconds);

            const float durS =
                (r->elementLockoutSeconds > 0.f) ? r->elementLockoutSeconds : std::max(0.5f, r->cooldownSeconds);

            InjectHUD::BeginReaction(a, rh, durS);

            if (r->cb) {
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    RE::ActorHandle h = a->CreateRefHandle();
                    auto cb = r->cb;
                    void* user = r->user;
                    tasks->AddTask([h, cb, user]() {
                        if (auto actorNi = h.get()) {
                            if (auto* target = actorNi.get()) {
                                ERF_ReactionContext ctx{};
                                ctx.target = target;
                                cb(ctx, user);
                            }
                        }
                    });
                } else {
                    ERF_ReactionContext ctx{};
                    ctx.target = a;
                    r->cb(ctx, r->user);
                }
            }

            any = true;
        }

        return any;
    }

    inline void RecomputeEffMultipliers(RE::Actor* a, Gauges::Entry& e) {
        const auto begin = Gauges::firstIndex();
        const auto n = e.v.size();

        std::fill(e.effMult.begin() + begin, e.effMult.begin() + n, 1.0);

        if (!a) {
            e.effDirty = false;
            return;
        }

        for (std::size_t i = begin; i < n; ++i) {
            const auto elem = static_cast<ERF_ElementHandle>(i);
            const double m = ElementalStates::GetGaugeMultiplierFor(a, elem);
            e.effMult[i] = m;
        }

        e.effDirty = false;
    }

    std::span<const std::uint32_t> GetColorLUT() {
        return std::span<const std::uint32_t>(g_colorLUT.data(), g_colorLUT.size());
    }
}

namespace {
    bool Save(SKSE::SerializationInterface* ser, bool dryRun) {
        const auto& m = Gauges::state();
        if (dryRun) {
            return !m.empty();
        }

        if (const auto count = static_cast<std::uint32_t>(m.size()); !ser->WriteRecordData(&count, sizeof(count)))
            return false;

        auto writeVecU8 = [&](const std::vector<std::uint8_t>& v) {
            const auto n = static_cast<std::uint32_t>(v.size());
            return ser->WriteRecordData(&n, sizeof(n)) && (n == 0 || ser->WriteRecordData(v.data(), n * sizeof(v[0])));
        };
        auto writeVecF = [&](const std::vector<float>& v) {
            const auto n = static_cast<std::uint32_t>(v.size());
            return ser->WriteRecordData(&n, sizeof(n)) && (n == 0 || ser->WriteRecordData(v.data(), n * sizeof(v[0])));
        };
        auto writeVecD = [&](const std::vector<double>& v) {
            const auto n = static_cast<std::uint32_t>(v.size());
            return ser->WriteRecordData(&n, sizeof(n)) && (n == 0 || ser->WriteRecordData(v.data(), n * sizeof(v[0])));
        };

        for (const auto& [id, e] : m) {
            if (!ser->WriteRecordData(&id, sizeof(id))) return false;

            if (!writeVecU8(e.v)) return false;
            if (!writeVecF(e.lastHitH)) return false;
            if (!writeVecF(e.lastEvalH)) return false;
            if (!writeVecF(e.blockUntilH)) return false;
            if (!writeVecD(e.blockUntilRtS)) return false;

            if (!writeVecD(e.reactCdRtS)) return false;
            if (!writeVecF(e.reactCdH)) return false;
            if (!writeVecU8(e.inReaction)) return false;

            if (!writeVecU8(e.preActive)) return false;
            if (!writeVecF(e.preIntensity)) return false;
            if (!writeVecD(e.preExpireRtS)) return false;
            if (!writeVecF(e.preExpireH)) return false;
        }
        return true;
    }

    bool Load(SKSE::SerializationInterface* ser, std::uint32_t version, std::uint32_t) {
        if (version != Gauges::kVersion) return false;

        auto& m = Gauges::state();
        m.clear();

        std::uint32_t count{};
        if (!ser->ReadRecordData(&count, sizeof(count))) return false;

        auto readVecU8 = [&](std::vector<std::uint8_t>& v) {
            std::uint32_t n{};
            if (!ser->ReadRecordData(&n, sizeof(n))) return false;
            v.resize(n);
            return n == 0 || ser->ReadRecordData(v.data(), n * sizeof(v[0]));
        };
        auto readVecF = [&](std::vector<float>& v) {
            std::uint32_t n{};
            if (!ser->ReadRecordData(&n, sizeof(n))) return false;
            v.resize(n);
            return n == 0 || ser->ReadRecordData(v.data(), n * sizeof(v[0]));
        };
        auto readVecD = [&](std::vector<double>& v) {
            std::uint32_t n{};
            if (!ser->ReadRecordData(&n, sizeof(n))) return false;
            v.resize(n);
            return n == 0 || ser->ReadRecordData(v.data(), n * sizeof(v[0]));
        };

        const auto& caps = ERF::API::Caps();
        const std::size_t nE = static_cast<std::size_t>(caps.numElements) + 1;
        const std::size_t nR = static_cast<std::size_t>(caps.numReactions) + 1;
        const std::size_t nP = static_cast<std::size_t>(caps.numPreEffects) + 1;

        auto padU8 = [&](std::vector<std::uint8_t>& v, std::size_t need) {
            if (v.size() < need) v.resize(need, 0u);
        };
        auto padF = [&](std::vector<float>& v, std::size_t need) {
            if (v.size() < need) v.resize(need, 0.f);
        };
        auto padD = [&](std::vector<double>& v, std::size_t need) {
            if (v.size() < need) v.resize(need, 0.0);
        };

        for (std::uint32_t i = 0; i < count; ++i) {
            RE::FormID oldID{};
            RE::FormID newID{};
            if (!ser->ReadRecordData(&oldID, sizeof(oldID))) return false;

            if (!ser->ResolveFormID(oldID, newID)) {
                std::vector<std::uint8_t> u8;
                std::vector<float> f;
                std::vector<double> d;
                if (!readVecU8(u8)) return false;
                if (!readVecF(f)) return false;
                if (!readVecF(f)) return false;
                if (!readVecF(f)) return false;
                if (!readVecD(d)) return false;

                if (!readVecD(d)) return false;
                if (!readVecF(f)) return false;
                if (!readVecU8(u8)) return false;

                if (!readVecU8(u8)) return false;
                if (!readVecF(f)) return false;
                if (!readVecD(d)) return false;
                if (!readVecF(f)) return false;
                continue;
            }

            Gauges::Entry e{};

            if (!readVecU8(e.v)) return false;
            if (!readVecF(e.lastHitH)) return false;
            if (!readVecF(e.lastEvalH)) return false;
            if (!readVecF(e.blockUntilH)) return false;
            if (!readVecD(e.blockUntilRtS)) return false;

            if (!readVecD(e.reactCdRtS)) return false;
            if (!readVecF(e.reactCdH)) return false;
            if (!readVecU8(e.inReaction)) return false;

            if (!readVecU8(e.preActive)) return false;
            if (!readVecF(e.preIntensity)) return false;
            if (!readVecD(e.preExpireRtS)) return false;
            if (!readVecF(e.preExpireH)) return false;

            padU8(e.v, nE);
            padF(e.lastHitH, nE);
            padF(e.lastEvalH, nE);
            padF(e.blockUntilH, nE);
            padD(e.blockUntilRtS, nE);

            padD(e.reactCdRtS, nR);
            padF(e.reactCdH, nR);
            padU8(e.inReaction, nR);

            padU8(e.preActive, nP);
            padF(e.preIntensity, nP);
            padD(e.preExpireRtS, nP);
            padF(e.preExpireH, nP);

            e.effMult.assign(nE, 1.0);
            e.effDirty = true;
            e.sized = true;

            Gauges::rebuildPresence(e);

            auto [it, ins] = m.try_emplace(newID, std::move(e));
            if (!ins) it->second = std::move(e);
        }
        return true;
    }

    void Revert() { Gauges::state().clear(); }
}

void ElementalGauges::Add(RE::Actor* a, ERF_ElementHandle elem, int delta) {
    if (!a || delta <= 0) return;

    auto& M = Gauges::state();
    auto [__it, __inserted] = M.try_emplace(a->GetFormID());
    auto& e = __it->second;
    if (__inserted) Gauges::initEntryDenseIfNeeded(e);

    const auto i = Gauges::idx(elem);
    const float nowH = NowHours();
    const double nowRt = NowRealSeconds();

    const auto snap = Gauges::SnapshotDecay();
    Gauges::tickAll(e, nowH, snap);

    if (nowH < e.blockUntilH[i] || nowRt < e.blockUntilRtS[i]) {
        return;
    }
    if (e.effDirty) {
        RecomputeEffMultipliers(a, e);
    }

    const int before = e.v[i];
    const float mult = (a->IsPlayerRef() ? ERF::GetConfig().playerMult.load(std::memory_order_relaxed)
                                         : ERF::GetConfig().npcMult.load(std::memory_order_relaxed));
    const double scaled = std::round(static_cast<double>(delta) * mult * e.effMult[i]);
    const int adj = (scaled <= 0.0) ? 0 : static_cast<int>(scaled);
    const int afterI = std::clamp(before + adj, 0, 100);

    const int sumBeforeMix = e.sumMix;
    e.v[i] = static_cast<std::uint8_t>(afterI);
    e.lastHitH[i] = nowH;
    e.lastEvalH[i] = nowH;
    onValChange(e, i, before, afterI);

    if (ERF::GetConfig().hudEnabled.load(std::memory_order_relaxed)) {
        HUD::StartHUDTick();
    }

    const auto& ER = ElementRegistry::get();
    const ERF_ElementDesc* d = ER.get(elem);
    const bool isIsolatedInMixed = d && d->noMixInMixedMode;

    if (const bool singleMode = ERF::GetConfig().isSingle.load(std::memory_order_relaxed);
        singleMode || isIsolatedInMixed) {
        if (before < 100 && afterI >= 100) {
            TriggerReaction(a, e, elem);
        }
    } else {
        if (sumBeforeMix < 100 && e.sumMix >= 100) {
            TriggerReaction(a, e, 0);
        }
    }
    (void)MaybeTriggerPreEffectsFor(a, e, elem, static_cast<std::uint8_t>(afterI));
}

void ElementalGauges::ForEachDecayed(const std::function<void(RE::FormID, TotalsView)>& fn) {
    auto& m = Gauges::state();
    const float nowH = NowHours();
    const double nowRt = NowRealSeconds();

    const auto snap = Gauges::SnapshotDecay();
    for (auto it = m.begin(); it != m.end();) {
        auto& e = it->second;

        RE::Actor* actor = nullptr;
        if (auto* f = RE::TESForm::LookupByID(it->first)) {
            actor = f->As<RE::Actor>();
        }

        Gauges::tickAll(e, nowH, snap, [&](std::size_t idx, std::uint8_t, std::uint8_t after) {
            if (!actor) return;
            const auto h = Gauges::handleFromIndex(idx);
            (void)MaybeTriggerPreEffectsFor(actor, e, h, after);
        });

        const std::size_t beginE = Gauges::firstIndex();
        const std::size_t nE = e.v.size();
        const std::size_t countE = (nE > beginE) ? (nE - beginE) : 0;

        bool anyElemLock = false;
        for (std::size_t i = beginE; i < nE && !anyElemLock; ++i) {
            const bool lockH = (i < e.blockUntilH.size()) && (e.blockUntilH[i] > nowH);
            const bool lockRt = (i < e.blockUntilRtS.size()) && (e.blockUntilRtS[i] > nowRt);
            anyElemLock = lockH || lockRt;
        }

        bool anyReactCd = false;
        const std::size_t beginR = Gauges::firstIndex();
        if (!anyReactCd && !e.reactCdH.empty()) {
            const std::size_t nR = e.reactCdH.size();
            for (std::size_t ri = beginR; ri < nR; ++ri) {
                if (e.reactCdH[ri] > nowH) {
                    anyReactCd = true;
                    break;
                }
            }
        }
        if (!anyReactCd && !e.reactCdRtS.empty()) {
            const std::size_t nR = e.reactCdRtS.size();
            for (std::size_t ri = beginR; ri < nR; ++ri) {
                if (e.reactCdRtS[ri] > nowRt) {
                    anyReactCd = true;
                    break;
                }
            }
        }

        bool anyReactFlag = false;
        if (!e.inReaction.empty()) {
            const std::size_t nR = e.inReaction.size();
            for (std::size_t ri = beginR; ri < nR; ++ri) {
                if (e.inReaction[ri] != 0) {
                    anyReactFlag = true;
                    break;
                }
            }
        }

        if (const bool anyVal = (e.sumAll > 0); !anyVal && !anyElemLock && !anyReactCd && !anyReactFlag) {
            it = m.erase(it);
            continue;
        }

        const std::span<const std::uint8_t> spanVals{e.v.data() + beginE, countE};
        TotalsView view{spanVals};
        fn(it->first, view);

        ++it;
    }
}

std::optional<ElementalGauges::HudGaugeBundle> ElementalGauges::PickHudDecayed(RE::FormID id, double nowRt,
                                                                               float nowH) {
    auto& M = Gauges::state();
    auto it = M.find(id);
    if (it == M.end()) {
        return std::nullopt;
    }

    auto& e = it->second;

    const auto snap = Gauges::SnapshotDecay();
    Gauges::tickAll(e, nowH, snap);

    HudGaugeBundle bundle{};

    struct ElemInfo {
        ERF_ElementHandle handle{};
        std::uint8_t value{0};
        std::uint32_t rgb{0xFFFFFF};
        bool isolated{false};
    };

    const auto colors = GetColorLUT();
    const auto& ER = ElementRegistry::get();

    std::vector<ElemInfo> elems;
    elems.reserve(e.presentList.size());

    int newSum = 0;

    for (ERF_ElementHandle h : e.presentList) {
        const std::size_t idx = Gauges::idx(h);
        if (idx >= e.v.size()) {
            continue;
        }

        const std::uint8_t vv = e.v[idx];
        if (vv == 0) {
            continue;
        }

        ElemInfo info;
        info.handle = h;
        info.value = vv;
        info.rgb = (idx < colors.size()) ? colors[idx] : 0xFFFFFFu;

        if (const auto* d = ER.get(h)) {
            info.isolated = d->noMixInMixedMode;
        } else {
            info.isolated = false;
        }

        elems.push_back(info);
        newSum += vv;
    }

    if (newSum == 0) {
        const std::size_t beginE = Gauges::firstIndex();
        const std::size_t nE = e.v.size();

        bool anyElemLock = false;
        for (std::size_t i2 = beginE; i2 < nE && !anyElemLock; ++i2) {
            const bool lockH = (i2 < e.blockUntilH.size()) && (e.blockUntilH[i2] > nowH);
            const bool lockRt = (i2 < e.blockUntilRtS.size()) && (e.blockUntilRtS[i2] > nowRt);
            anyElemLock = lockH || lockRt;
        }

        bool anyReactCd = false;
        const std::size_t beginR = Gauges::firstIndex();

        if (!anyReactCd && !e.reactCdH.empty()) {
            const std::size_t nR = e.reactCdH.size();
            for (std::size_t ri = beginR; ri < nR; ++ri) {
                if (e.reactCdH[ri] > nowH) {
                    anyReactCd = true;
                    break;
                }
            }
        }

        if (!anyReactCd && !e.reactCdRtS.empty()) {
            const std::size_t nR = e.reactCdRtS.size();
            for (std::size_t ri = beginR; ri < nR; ++ri) {
                if (e.reactCdRtS[ri] > nowRt) {
                    anyReactCd = true;
                    break;
                }
            }
        }

        bool anyReactFlag = false;
        if (!e.inReaction.empty()) {
            const std::size_t nR = e.inReaction.size();
            for (std::size_t ri = beginR; ri < nR; ++ri) {
                if (e.inReaction[ri] != 0) {
                    anyReactFlag = true;
                    break;
                }
            }
        }

        if (!anyElemLock && !anyReactCd && !anyReactFlag) {
            M.erase(it);
        }

        bundle.icons = std::span<const char* const>();
        bundle.values = std::span<const std::uint32_t>();
        bundle.colors = std::span<const std::uint32_t>();

        return std::nullopt;
    }

    TL_vals32.clear();
    TL_cols32.clear();
    TL_accumIcons.clear();
    TL_elemsNZ.clear();

    TL_vals32.reserve(elems.size());
    TL_cols32.reserve(elems.size());
    TL_elemsNZ.reserve(elems.size());

    const auto& RR = ReactionRegistry::get();

    if (const bool singleMode = ERF::GetConfig().isSingle.load(std::memory_order_relaxed); singleMode) {
        for (const auto& info : elems) {
            TL_vals32.push_back(static_cast<std::uint32_t>(std::min<int>(info.value, 100)));
            TL_cols32.push_back(info.rgb);
            TL_elemsNZ.push_back(info.handle);
        }

        bundle.values = std::span<const std::uint32_t>(TL_vals32.data(), TL_vals32.size());
        bundle.colors = std::span<const std::uint32_t>(TL_cols32.data(), TL_cols32.size());

        TL_accumIcons.clear();

        thread_local std::vector<std::uint8_t> TL_totals;
        if (TL_totals.size() < e.v.size()) {
            TL_totals.resize(e.v.size());
        }

        for (std::size_t k = 0; k < TL_vals32.size(); ++k) {
            const auto elemH = TL_elemsNZ[k];
            const auto idx = Gauges::idx(elemH);
            if (idx >= TL_totals.size()) {
                TL_accumIcons.push_back(nullptr);
                continue;
            }

            std::ranges::fill(TL_totals, std::uint8_t{0});
            const auto vClamped = static_cast<std::uint8_t>(std::min<std::uint32_t>(TL_vals32[k], 100));
            TL_totals[idx] = vClamped;

            const int sum = vClamped;
            if (sum <= 0) {
                TL_accumIcons.push_back(nullptr);
                continue;
            }

            const float inv = 1.0f / static_cast<float>(sum);
            std::array<ERF_ElementHandle, 1> present{elemH};
            auto best = RR.pickBestFast(TL_totals, std::span<const ERF_ElementHandle>(present.data(), present.size()),
                                        sum, inv);

            const char* icon = (best && best->icon) ? best->icon : kFallbackReactionIcon;
            TL_accumIcons.push_back(icon);
        }

        bundle.icons = std::span<const char* const>(TL_accumIcons.data(), TL_accumIcons.size());
        return bundle;
    }

    int firstMixIndex = -1;
    for (std::size_t i3 = 0; i3 < elems.size(); ++i3) {
        if (!elems[i3].isolated) {
            firstMixIndex = static_cast<int>(i3);
            break;
        }
    }

    if (firstMixIndex < 0) {
        for (const auto& info : elems) {
            TL_vals32.push_back(static_cast<std::uint32_t>(std::min<int>(info.value, 100)));
            TL_cols32.push_back(info.rgb);
            TL_elemsNZ.push_back(info.handle);
        }

        bundle.values = std::span<const std::uint32_t>(TL_vals32.data(), TL_vals32.size());
        bundle.colors = std::span<const std::uint32_t>(TL_cols32.data(), TL_cols32.size());

        TL_accumIcons.clear();

        thread_local std::vector<std::uint8_t> TL_totalsIso;
        if (TL_totalsIso.size() < e.v.size()) {
            TL_totalsIso.resize(e.v.size());
        }

        for (std::size_t k2 = 0; k2 < TL_vals32.size(); ++k2) {
            const auto elemH = TL_elemsNZ[k2];
            const auto idx = Gauges::idx(elemH);
            if (idx >= TL_totalsIso.size()) {
                TL_accumIcons.push_back(nullptr);
                continue;
            }

            std::ranges::fill(TL_totalsIso, std::uint8_t{0});
            const auto vClamped = static_cast<std::uint8_t>(std::min<std::uint32_t>(TL_vals32[k2], 100));
            TL_totalsIso[idx] = vClamped;

            const int sum = vClamped;
            if (sum <= 0) {
                TL_accumIcons.push_back(nullptr);
                continue;
            }

            const float inv = 1.0f / static_cast<float>(sum);
            std::array<ERF_ElementHandle, 1> present{elemH};
            auto best = RR.pickBestFast(TL_totalsIso,
                                        std::span<const ERF_ElementHandle>(present.data(), present.size()), sum, inv);

            const char* icon = (best && best->icon) ? best->icon : kFallbackReactionIcon;

            TL_accumIcons.push_back(icon);
        }

        bundle.icons = std::span<const char* const>(TL_accumIcons.data(), TL_accumIcons.size());
        return bundle;
    }

    std::vector<const ElemInfo*> singlesBefore;
    std::vector<const ElemInfo*> mixList;
    std::vector<const ElemInfo*> singlesAfter;

    singlesBefore.reserve(elems.size());
    mixList.reserve(elems.size());
    singlesAfter.reserve(elems.size());

    for (std::size_t i4 = 0; i4 < elems.size(); ++i4) {
        const auto& info = elems[i4];
        if (!info.isolated) {
            mixList.push_back(&info);
        } else if (static_cast<int>(i4) < firstMixIndex) {
            singlesBefore.push_back(&info);
        } else {
            singlesAfter.push_back(&info);
        }
    }

    auto pushInfo = [&](const ElemInfo* p) {
        TL_vals32.push_back(static_cast<std::uint32_t>(std::min<int>(p->value, 100)));
        TL_cols32.push_back(p->rgb);
        TL_elemsNZ.push_back(p->handle);
    };

    for (auto* p : singlesBefore) {
        pushInfo(p);
    }
    for (auto* p : mixList) {
        pushInfo(p);
    }
    for (auto* p : singlesAfter) {
        pushInfo(p);
    }

    bundle.values = std::span<const std::uint32_t>(TL_vals32.data(), TL_vals32.size());
    bundle.colors = std::span<const std::uint32_t>(TL_cols32.data(), TL_cols32.size());

    TL_accumIcons.clear();

    const std::size_t nb = singlesBefore.size();
    const std::size_t nm = mixList.size();
    const std::size_t na = singlesAfter.size();

    const std::size_t gaugeCount = nb + (nm > 0 ? 1u : 0u) + na;
    TL_accumIcons.resize(gaugeCount, nullptr);

    bundle.singlesBefore = static_cast<std::uint32_t>(nb);
    bundle.singlesAfter = static_cast<std::uint32_t>(na);

    thread_local std::vector<std::uint8_t> TL_totalsSingle;
    if (TL_totalsSingle.size() < e.v.size()) {
        TL_totalsSingle.resize(e.v.size());
    }

    auto pickIconSingle = [&](const ElemInfo* info) -> const char* {
        const auto elemH = info->handle;
        const auto idx = Gauges::idx(elemH);
        if (idx >= TL_totalsSingle.size()) {
            return nullptr;
        }

        std::ranges::fill(TL_totalsSingle, std::uint8_t{0});
        const auto vClamped = static_cast<std::uint8_t>(std::min<int>(info->value, 100));
        TL_totalsSingle[idx] = vClamped;

        const int sum = vClamped;
        if (sum <= 0) {
            return nullptr;
        }

        const float inv = 1.0f / static_cast<float>(sum);
        std::array<ERF_ElementHandle, 1> present{elemH};
        auto best = RR.pickBestFast(TL_totalsSingle, std::span<const ERF_ElementHandle>(present.data(), present.size()),
                                    sum, inv);

        if (best && best->icon) {
            return best->icon;
        }
        return kFallbackReactionIcon;
    };

    for (std::size_t i5 = 0; i5 < nb; ++i5) {
        TL_accumIcons[i5] = pickIconSingle(singlesBefore[i5]);
    }

    if (nm > 0) {
        thread_local std::vector<std::uint8_t> TL_totalsMix;
        if (TL_totalsMix.size() < e.v.size()) {
            TL_totalsMix.resize(e.v.size());
        }
        std::ranges::fill(TL_totalsMix, std::uint8_t{0});

        int sumMixIcons = 0;
        std::vector<ERF_ElementHandle> presentMix;
        presentMix.reserve(nm);

        for (const auto* p : mixList) {
            const auto elemH = p->handle;
            const auto idx = Gauges::idx(elemH);
            if (idx >= TL_totalsMix.size()) {
                continue;
            }
            const auto vClamped = static_cast<std::uint8_t>(std::min<int>(p->value, 100));
            TL_totalsMix[idx] = vClamped;
            sumMixIcons += vClamped;
            presentMix.push_back(elemH);
        }

        if (sumMixIcons > 0 && !presentMix.empty()) {
            const float inv = 1.0f / static_cast<float>(sumMixIcons);
            auto best = RR.pickBestFast(std::span<const std::uint8_t>(TL_totalsMix.data(), TL_totalsMix.size()),
                                        std::span<const ERF_ElementHandle>(presentMix.data(), presentMix.size()),
                                        sumMixIcons, inv);
            TL_accumIcons[nb] = (best && best->icon) ? best->icon : kFallbackReactionIcon;
        }
    }

    const std::size_t offsetAfter = nb + (nm > 0 ? 1u : 0u);
    for (std::size_t i6 = 0; i6 < na; ++i6) {
        TL_accumIcons[offsetAfter + i6] = pickIconSingle(singlesAfter[i6]);
    }

    bundle.icons = std::span<const char* const>(TL_accumIcons.data(), TL_accumIcons.size());
    return bundle;
}

void ElementalGauges::RegisterStore() { Ser::Register({Gauges::kRecordID, Gauges::kVersion, &Save, &Load, &Revert}); }

void ElementalGauges::InvalidateStateMultipliers(RE::Actor* a) {
    if (!a) return;
    auto& m = Gauges::state();
    const auto it = m.find(a->GetFormID());
    if (it == m.end()) return;
    it->second.effDirty = true;
}

void ElementalGauges::BuildColorLUTOnce() {
    auto const& ER = ElementRegistry::get();
    const std::size_t n = ER.size() + 1;
    g_colorLUT.resize(n, 0xFFFFFFu);
    for (std::size_t i = 1; i < n; ++i) {
        if (const ERF_ElementDesc* d = ER.get(static_cast<ERF_ElementHandle>(i))) {
            g_colorLUT[i] = d->colorRGB;
        }
    }
}