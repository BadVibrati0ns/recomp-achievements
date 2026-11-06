# recomp-achievements, the desktop app (docs/app.md). The core (grid, game
# page, library polling, scripts) needs no SDL, so its tests run headless;
# the executable adds the SDL3 window.
add_library(recomp_achievements_app_core STATIC app.cpp library_view.cpp script.cpp)
target_include_directories(recomp_achievements_app_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(recomp_achievements_app_core PUBLIC recomp_achievements_ui_base)

add_executable(recomp_achievements_app main.cpp)
set_target_properties(recomp_achievements_app PROPERTIES OUTPUT_NAME recomp-achievements)
target_link_libraries(recomp_achievements_app PRIVATE recomp_achievements_app_core recomp_achievements_ui)

if(NOT MSVC)
  target_compile_options(recomp_achievements_app_core PRIVATE -Wall -Wextra)
  target_compile_options(recomp_achievements_app PRIVATE -Wall -Wextra)
endif()

if(RECOMP_ACH_BUILD_TESTS)
  enable_testing()
  set(_app_tests ${CMAKE_CURRENT_SOURCE_DIR}/../../tests)
  add_executable(recomp_achievements_app_tests ${_app_tests}/ui/test_main.cpp ${_app_tests}/app/test_app.cpp)
  target_include_directories(recomp_achievements_app_tests PRIVATE ${_app_tests}/ui)
  target_link_libraries(recomp_achievements_app_tests PRIVATE recomp_achievements_app_core recomp_achievements_demo_data)
  target_compile_definitions(recomp_achievements_app_tests PRIVATE
      RECOMP_ACH_APP_TEST_TMP="${CMAKE_CURRENT_BINARY_DIR}/app_test_tmp")
  if(NOT MSVC)
    target_compile_options(recomp_achievements_app_tests PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME recomp_achievements_app_tests COMMAND recomp_achievements_app_tests)
endif()
