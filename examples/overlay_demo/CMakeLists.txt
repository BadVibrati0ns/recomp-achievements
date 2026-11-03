cmake_minimum_required(VERSION 3.24)
project(recomp_achievements_overlay_demo CXX)

# Drives the software overlay (no ImGui, no GPU) with scripted input and
# writes raw RGB24 frames to stdout plus a WAV of the popup sounds.
set(CMAKE_CXX_STANDARD 20)
set(RECOMP_ACH_MEMORY OFF)
set(RECOMP_ACH_BUILD_TESTS OFF)
add_subdirectory(../.. recomp_achievements)

add_executable(overlay_demo overlay_demo.cpp)
target_link_libraries(overlay_demo PRIVATE recomp::achievements_ui_base)
