cmake_minimum_required(VERSION 3.24)
project(recomp_achievements_popup_demo CXX)

# Renders the built-in popup themes to video without a GPU. Point IMGUI_DIR at
# a Dear ImGui checkout (1.92+, for ImGuiBackendFlags_RendererHasTextures).
set(CMAKE_CXX_STANDARD 20)
set(IMGUI_DIR "" CACHE PATH "Dear ImGui source folder")
if(NOT IMGUI_DIR)
  message(FATAL_ERROR "Set -DIMGUI_DIR=<path to imgui>")
endif()

add_library(imgui STATIC ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR})

set(RECOMP_ACH_IMGUI_TARGET imgui)
set(RECOMP_ACH_MEMORY OFF)
set(RECOMP_ACH_BUILD_TESTS OFF)
add_subdirectory(../.. recomp_achievements)

add_executable(popup_demo popup_demo.cpp)
target_link_libraries(popup_demo PRIVATE recomp::achievements_ui)
