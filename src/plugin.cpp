#include <atomic>
#include <chrono>
#include <thread>
#include <utility>

#include "Config.h"
#include "ElementalReactionsAPI.h"
#include "ModAPI.h"
#include "PCH.h"
#include "TrueHUDAPI.h"
#include "common/Helpers.h"
#include "common/PluginSerialization.h"
#include "elemental_reactions/ElementalGauges.h"
#include "elemental_reactions/ElementalGaugesHook.h"
#include "elemental_reactions/ElementalStates.h"
#include "hud/InjectHUD.h"
#include "hud/TrueHUDMenuWatcher.h"
#include "ui/ERF_UI.h"

#ifndef DLLEXPORT
    #include "REL/Relocation.h"
#endif
#ifndef DLLEXPORT
    #define DLLEXPORT __declspec(dllexport)
#endif

using namespace SKSE;
using namespace RE;

const std::filesystem::path& ERF_GetThisDllDir();

namespace {
    void InitializeLogger() {
        if (auto path = log::log_directory()) {
            *path /= "ElementalReactionsFramework.log";
            auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
            auto logger = std::make_shared<spdlog::logger>("global", sink);
            spdlog::set_default_logger(logger);
            spdlog::set_level(spdlog::level::info);
            spdlog::flush_on(spdlog::level::info);
            spdlog::info("Logger iniciado.");
        }
    }

    void GlobalMessageHandler(SKSE::MessagingInterface::Message* msg) {
        if (!msg) return;

        switch (msg->type) {
            case SKSE::MessagingInterface::kDataLoaded: {
                ERF::API::OpenRegistrationWindowAndScheduleFreeze();
                ElementalGaugesHook::InitCarrierRefs();
                ElementalGaugesHook::Install();
                ElementalGaugesHook::RegisterAEEventSink();

                auto& st = InjectHUD::Globals();
                st.trueHUD = static_cast<TRUEHUD_API::IVTrueHUD4*>(
                    TRUEHUD_API::RequestPluginAPI(TRUEHUD_API::InterfaceVersion::V4));
                if (!st.trueHUD) {
                    spdlog::warn("[ERF] TrueHUD não detectado. Widget não será carregado.");
                    return;
                }

                st.pluginHandle = SKSE::GetPluginHandle();

                if (auto* ui = RE::UI::GetSingleton()) {
                    ui->GetEventSource<RE::MenuOpenCloseEvent>()->AddEventSink(
                        TrueHUDWatcher::TrueHUDMenuWatcher::GetSingleton());
                }

                st.trueHUD->LoadCustomWidgets(
                    st.pluginHandle, InjectHUD::ERF_SWF_PATH, [](TRUEHUD_API::APIResult result) {
                        auto& st = InjectHUD::Globals();

                        if (result == TRUEHUD_API::APIResult::OK) {
                            st.trueHUD->RegisterNewWidgetType(st.pluginHandle, InjectHUD::ERF_WIDGET_TYPE);

                            ElementalGaugesHook::AllowHudTickFlag().store(true, std::memory_order_release);
                        } else {
                            spdlog::error("[ERF] Falha ao carregar o SWF dos widgets!");
                        }
                    });

                ERF::GetConfig().Load();
                ERF_UI::Register();
                ERF::Overrides::SetGaugeEffect(ElementalGaugesHook::GaugeAccEffect());
                ERF::Overrides::ApplyOverridesFromJSON();
                break;
            }

            case SKSE::MessagingInterface::kNewGame:
                [[fallthrough]];
            case SKSE::MessagingInterface::kPostLoadGame: {
                InjectHUD::RemoveAllWidgets();
                HUD::ResetTracking();
                break;
            }

            default:
                break;
        }
    }
}

extern "C" DLLEXPORT void* SKSEAPI RequestPluginAPI(std::uint32_t requestedVersion) {
    if (requestedVersion == ERF_API_VERSION) {
        return static_cast<void*>(ERF::API::Get());
    }
    return nullptr;
}

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* skse) {
    SKSE::Init(skse);
    InitializeLogger();

    Ser::Install(FOURCC('E', 'L', 'R', 'E'));

    ElementalStates::RegisterStore();
    ElementalGauges::RegisterStore();

    if (const auto mi = SKSE::GetMessagingInterface()) {
        mi->RegisterListener(GlobalMessageHandler);
    }

    return true;
}

const std::filesystem::path& ERF_GetThisDllDir() {
    static std::filesystem::path cached;

    if (static bool init = false; !init) {
        init = true;

        HMODULE self = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&SKSEPlugin_Load), &self)) {  // NOSONAR
            wchar_t buf[MAX_PATH]{};                                                   // NOSONAR
            const DWORD n = GetModuleFileNameW(self, buf, static_cast<DWORD>(std::size(buf)));
            if (n > 0) {
                cached = std::filesystem::path(buf).parent_path();
            }
        }
    }
    return cached;
}