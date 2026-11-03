cmake_minimum_required(VERSION 3.24)
project(recomp_achievements LANGUAGES C CXX)

# Achievements, trophies and popups for static recompilations. The core has no
# dependencies beyond the bundled nlohmann/json; every other part is optional.
#
#   recomp::achievements           registry, JSON sets, triggers, saves
#   recomp::achievements_ui_base   styles, popup queues, the software overlay
#   recomp::achievements_ui        popups (Dear ImGui), styles, sounds (SDL3)
#   recomp::achievements_xbox      XDBF/SPA reader
#   recomp::achievements_trophy    PS3 TROPHY.TRP / TROPCONF.SFM reader
#   recomp::achievements_memory    RetroAchievements-syntax memory triggers (rcheevos)
#   recomp::achievements_rexglue   binding for rexglue (Xbox 360) recomps

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(RECOMP_ACH_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(RECOMP_ACH_TOP_LEVEL ON)
endif()

option(RECOMP_ACH_BUILD_TESTS "Build the SDK's unit tests" ${RECOMP_ACH_TOP_LEVEL})
option(RECOMP_ACH_UI "Build the popup UI (needs an imgui target)" ON)
option(RECOMP_ACH_MEMORY "Build memory triggers (fetches rcheevos)" ON)
# The consumer's Dear ImGui and SDL3 targets. Without an imgui target the UI
# is skipped; without SDL3 popups are silent.
set(RECOMP_ACH_IMGUI_TARGET "" CACHE STRING "Existing Dear ImGui target to link the UI against")
set(RECOMP_ACH_SDL3_TARGET "" CACHE STRING "Existing SDL3 target used to play sounds")
# rexglue's runtime target (e.g. rex::runtime). Empty: no rexglue binding.
set(RECOMP_ACH_REXGLUE_TARGET "" CACHE STRING "rexglue runtime target for the Xbox 360 binding")
# Whether that rexglue has patches/rexglue-achievements.patch. Off: stock
# rexglue, and the recomp shims XAM itself through RexglueBinding::xam().
option(RECOMP_ACH_REXGLUE_PATCHED "rexglue includes patches/rexglue-achievements.patch" ON)

include(cmake/core.cmake)
include(cmake/ui.cmake)
include(cmake/sources.cmake)
include(cmake/memory.cmake)
include(cmake/rexglue.cmake)
