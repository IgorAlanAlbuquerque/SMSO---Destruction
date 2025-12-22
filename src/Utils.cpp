// THIS CODE IS INSPIRED BY THE CODE MADE BY ERSHin IN THE TRUEHUD REPOSITORY

#include "Utils.h"

#include <unordered_map>

namespace {
    static const RE::BSFixedString kHeadName{"NPC Head [Head]"};
    inline constexpr float kBoundFallbackRadius = 64.0f;

    struct HeadEntry {
        RE::NiAVObject* node{nullptr};
    };

    static std::unordered_map<RE::FormID, HeadEntry> g_headCache;
    static bool g_resvd = false;

    inline void EnsureReserveOnce() {
        if (!g_resvd) {
            g_headCache.reserve(128);
            g_resvd = true;
        }
    }

    inline RE::NiAVObject* FindHeadNode(RE::NiAVObject* root) noexcept {
        return root ? root->GetObjectByName(kHeadName) : nullptr;
    }

    inline RE::NiAVObject* GetHeadNodeFast_(RE::Actor* a) {
        if (!a) return nullptr;
        EnsureReserveOnce();

        const RE::FormID key = a->GetFormID();
        if (auto it = g_headCache.find(key); it != g_headCache.end()) {
            if (RE::NiAVObject* n = it->second.node) {
                if (n->parent) return n;
                if (RE::NiAVObject* root = a->Get3D2()) {
                    if (RE::NiAVObject* head = FindHeadNode(root)) {
                        it->second.node = head;
                        return head;
                    }
                }
                it->second.node = nullptr;
                return nullptr;
            } else {
                if (RE::NiAVObject* root = a->Get3D2()) {
                    if (RE::NiAVObject* head = FindHeadNode(root)) {
                        it->second.node = head;
                        return head;
                    }
                }
                return nullptr;
            }
        }

        if (RE::NiAVObject* root = a->Get3D2()) {
            if (RE::NiAVObject* head = FindHeadNode(root)) {
                g_headCache.emplace(key, HeadEntry{head});
                return head;
            }
        }
        g_headCache.emplace(key, HeadEntry{nullptr});
        return nullptr;
    }
}

bool Utils::GetHeadPosFast(RE::Actor* a, RE::NiPoint3& out) {
    if (!a) return false;
    if (RE::NiAVObject const* head = GetHeadNodeFast_(a)) {
        out = head->world.translate;
        return true;
    }
    if (RE::NiAVObject const* root = a->Get3D2()) {
        const auto& b = root->worldBound;
        if (b.radius > 1e-4f) {
            out = b.center;
            return true;
        }
    }
    return false;
}

bool Utils::GetNodePosition(RE::ActorPtr a_actor, const char* a_nodeName, RE::NiPoint3& point) {
    if (!a_actor) return false;
    if (!a_nodeName || a_nodeName[0] == '\0') return false;
    RE::NiAVObject* root = a_actor->Get3D2();
    if (!root) return false;
    static thread_local RE::BSFixedString s_name;
    s_name = a_nodeName;
    if (RE::NiAVObject const* node = root->GetObjectByName(s_name)) {
        point = node->world.translate;
        return true;
    }
    return false;
}

bool Utils::GetTorsoPos(RE::ActorPtr a_actor, RE::NiPoint3& point) {
    if (!a_actor) return false;
    RE::TESRace const* race = a_actor->GetRace();
    if (!race) return false;
    RE::BGSBodyPartData const* bodyPartData = race->bodyPartData;
    if (!bodyPartData) return false;
    RE::BGSBodyPart const* bodyPart = bodyPartData->parts[0];
    if (!bodyPart) return false;
    return GetNodePosition(a_actor, bodyPart->targetName.c_str(), point);
}

bool Utils::GetBoundTopPos(RE::Actor* a, RE::NiPoint3& out) {
    if (!a) return false;
    if (auto const* root = a->Get3D(false)) {
        const auto& b = root->worldBound;
        const float r = (b.radius > 1e-4f) ? b.radius : kBoundFallbackRadius;
        out = {b.center.x, b.center.y, b.center.z + r};
        return true;
    }
    return false;
}

bool Utils::GetTargetPos(RE::ObjectRefHandle a_target, RE::NiPoint3& pos, bool bPreferBody) {
    auto target = a_target.get();
    if (!target) return false;
    if (target->Get3D2() == nullptr) return false;

    if (target->formType == RE::FormType::ActorCharacter) {
        RE::Actor* actorRaw = target->As<RE::Actor>();
        RE::ActorPtr actorPtr(actorRaw);

        if (bPreferBody && GetTorsoPos(actorPtr, pos)) {
            return true;
        }
        if (Utils::GetHeadPosFast(actorRaw, pos)) return true;
        if (Utils::GetBoundTopPos(actorRaw, pos)) return true;

        pos = target->GetLookingAtLocation();
        return true;
    }

    pos = target->GetPosition();
    return true;
}

void Utils::HeadCacheReserve(std::size_t n) noexcept {
    EnsureReserveOnce();
    if (n > g_headCache.bucket_count()) g_headCache.reserve(n);
}

void Utils::HeadCacheClearAll() noexcept {
    g_headCache.clear();
    g_resvd = false;
}