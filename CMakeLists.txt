cmake_minimum_required(VERSION 3.21)
cmake_policy(SET CMP0002 NEW)

project(ElementalReactionsFramework VERSION 0.0.1 LANGUAGES CXX)

if(DEFINED ENV{SKYRIM_MODS_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_MODS_FOLDER}")
    set(OUTPUT_FOLDER "$ENV{SKYRIM_MODS_FOLDER}/${PROJECT_NAME}")
endif()

set(BUILD_TESTS OFF CACHE BOOL "Build unit tests for CommonLibVR." FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "Disable Skyrim VR in CommonLibVR-ng" FORCE)

add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/CommonLibVR-ng
    ${CMAKE_BINARY_DIR}/_deps/CommonLibVR-ng-build
)

get_target_property(COMMONLIB_SRC_DIR CommonLibSSE SOURCE_DIR)

include(${COMMONLIB_SRC_DIR}/cmake/CommonLibSSE.cmake)

add_commonlibsse_plugin(ElementalReactionsFramework
  SOURCES
    src/PCH.cpp
    src/plugin.cpp
    src/Utils.cpp
    src/elemental_reactions/ElementalStates.cpp
    src/common/PluginSerialization.cpp
    src/elemental_reactions/ElementalGauges.cpp
    src/elemental_reactions/ElementalGaugesHook.cpp
    src/hud/HUDTick.cpp
    src/hud/InjectHUD.cpp
    src/hud/TrueHUDMenuWatcher.cpp
    src/elemental_reactions/erf_element.cpp
    src/elemental_reactions/erf_reaction.cpp
    src/elemental_reactions/erf_state.cpp
    src/elemental_reactions/erf_preeffect.cpp
    src/ModAPI.cpp
    src/ui/ERF_UI.cpp
    src/Config.cpp
    src/overrides/Overrides.cpp
)

target_include_directories(ElementalReactionsFramework
  PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src )

target_compile_features(ElementalReactionsFramework PRIVATE cxx_std_23)

target_precompile_headers(ElementalReactionsFramework PRIVATE 
  src/PCH.h
  src/Utils.h
  src/common/PluginSerialization.h
  src/common/Helpers.h
  src/elemental_reactions/ElementalStates.h
  src/elemental_reactions/ElementalGauges.h
  src/elemental_reactions/ElementalGaugesHook.h
  src/hud/HUDTick.h
  src/hud/InjectHUD.h
  src/hud/TrueHUDMenuWatcher.h
  src/elemental_reactions/erf_element.h
  src/elemental_reactions/erf_reaction.h
  src/elemental_reactions/erf_state.h
  src/elemental_reactions/erf_preeffect.h
  src/ModAPI.h
  src/ui/ERF_UI.h
  src/Offsets.h
  src/Config.h
  src/overrides/Overrides.h
)

if(DEFINED OUTPUT_FOLDER)
    set(DLL_FOLDER "${OUTPUT_FOLDER}/SKSE/Plugins")

    message(STATUS "SKSE plugin output folder: ${DLL_FOLDER}")

    add_custom_command(
        TARGET "${PROJECT_NAME}"
        POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DLL_FOLDER}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
        VERBATIM
    )
endif()
