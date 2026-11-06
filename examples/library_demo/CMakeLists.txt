cmake_minimum_required(VERSION 3.24)
project(recomp_achievements_library_demo CXX)

# Writes a library entry for a made-up game into the folder given on the command
# line, then reads it back as the all-games app will. No UI needed.
set(CMAKE_CXX_STANDARD 20)
set(RECOMP_ACH_UI OFF)
set(RECOMP_ACH_MEMORY OFF)
set(RECOMP_ACH_BUILD_TESTS OFF)
add_subdirectory(../.. recomp_achievements)

add_executable(library_demo library_demo.cpp)
target_link_libraries(library_demo PRIVATE recomp::achievements)
