#pragma once

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <string>
#include <string_view>

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

using namespace std::literals;

#define DEBUG

#ifdef DEBUG
    #define DEBUG_LOG(...) spdlog::info(__VA_ARGS__)
#else
    #define DEBUG_LOG(...) ((void)0)
#endif