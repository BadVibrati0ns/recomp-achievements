# The demo library generator: made-up games written through the real export
# path (System + Library), for the app's demo and tests. Needs only the core.
add_library(recomp_achievements_demo_data STATIC demo_library.cpp)
target_include_directories(recomp_achievements_demo_data PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(recomp_achievements_demo_data PUBLIC recomp_achievements)

add_executable(recomp_achievements_demo main.cpp)
set_target_properties(recomp_achievements_demo PROPERTIES OUTPUT_NAME recomp-achievements-demo)
target_link_libraries(recomp_achievements_demo PRIVATE recomp_achievements_demo_data)

if(NOT MSVC)
  target_compile_options(recomp_achievements_demo_data PRIVATE -Wall -Wextra)
  target_compile_options(recomp_achievements_demo PRIVATE -Wall -Wextra)
endif()
