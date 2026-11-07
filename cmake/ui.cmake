# Popup UI.
#   recomp::achievements_ui_base  styles, popup queues, sound synthesis, icon
#                                 cache, the software overlay (viewer and
#                                 popups) and the every-game library view;
#                                 needs neither ImGui nor SDL
#   recomp::achievements_ui       + SDL3 sound player and PNG/WAV loading (when
#                                 RECOMP_ACH_SDL3_TARGET is set) and the ImGui
#                                 renderer and list window (when
#                                 RECOMP_ACH_IMGUI_TARGET is set)
if(NOT RECOMP_ACH_UI)
  return()
endif()

set(_ach_ui_src ${CMAKE_CURRENT_LIST_DIR}/../src/ui)

add_library(recomp_achievements_ui_base STATIC
    ${_ach_ui_src}/style.cpp
    ${_ach_ui_src}/notification_center.cpp
    ${_ach_ui_src}/notification_attach.cpp
    ${_ach_ui_src}/sounds.cpp
    ${_ach_ui_src}/icon_cache.cpp
    ${_ach_ui_src}/builtin_decoders.cpp
    ${_ach_ui_src}/popup_painter.cpp
    ${_ach_ui_src}/canvas.cpp
    ${_ach_ui_src}/viewer_parts.cpp
    ${_ach_ui_src}/viewer_toggle.cpp
    ${_ach_ui_src}/overlay.cpp
    # Every game's library (the desktop app's grid, and the viewer's All games page).
    ${_ach_ui_src}/../app/app.cpp
    ${_ach_ui_src}/../app/library_view.cpp)
add_library(recomp::achievements_ui_base ALIAS recomp_achievements_ui_base)
target_link_libraries(recomp_achievements_ui_base PUBLIC recomp_achievements)

# The overlay's default fonts (DejaVu Sans, assets/fonts/LICENSE), compiled in
# as byte arrays so no font file has to ship with the game. Off: games pass
# their own TTF bytes in OverlayOptions.
option(RECOMP_ACH_EMBED_FONTS "Embed DejaVu Sans for the software overlay" ON)
if(RECOMP_ACH_EMBED_FONTS)
  set(_ach_fonts ${CMAKE_CURRENT_LIST_DIR}/../assets/fonts)
  set(_ach_gen ${CMAKE_CURRENT_BINARY_DIR}/recomp_achievements_fonts)
  foreach(_font IN ITEMS "DejaVuSans;kDejaVuSans" "DejaVuSans-Bold;kDejaVuSansBold")
    list(GET _font 0 _file)
    list(GET _font 1 _symbol)
    add_custom_command(
        OUTPUT ${_ach_gen}/${_file}.cpp
        COMMAND ${CMAKE_COMMAND} -DINPUT=${_ach_fonts}/${_file}.ttf -DOUTPUT=${_ach_gen}/${_file}.cpp
                -DSYMBOL=${_symbol} -P ${CMAKE_CURRENT_LIST_DIR}/embed_file.cmake
        DEPENDS ${_ach_fonts}/${_file}.ttf ${CMAKE_CURRENT_LIST_DIR}/embed_file.cmake
        COMMENT "Embedding ${_file}.ttf"
        VERBATIM)
    target_sources(recomp_achievements_ui_base PRIVATE ${_ach_gen}/${_file}.cpp)
  endforeach()
else()
  target_compile_definitions(recomp_achievements_ui_base PRIVATE RECOMP_ACH_NO_EMBEDDED_FONTS=1)
endif()

add_library(recomp_achievements_ui STATIC
    ${_ach_ui_src}/sound_player.cpp
    ${_ach_ui_src}/sdl_support.cpp)
add_library(recomp::achievements_ui ALIAS recomp_achievements_ui)
target_link_libraries(recomp_achievements_ui PUBLIC recomp_achievements_ui_base)

if(RECOMP_ACH_SDL3_TARGET)
  if(NOT TARGET ${RECOMP_ACH_SDL3_TARGET})
    message(FATAL_ERROR "RECOMP_ACH_SDL3_TARGET '${RECOMP_ACH_SDL3_TARGET}' is not a target")
  endif()
  target_link_libraries(recomp_achievements_ui PUBLIC ${RECOMP_ACH_SDL3_TARGET})
  target_compile_definitions(recomp_achievements_ui PUBLIC RECOMP_ACH_HAS_SDL3=1)
else()
  target_compile_definitions(recomp_achievements_ui PUBLIC RECOMP_ACH_HAS_SDL3=0)
  message(STATUS "recomp-achievements: no RECOMP_ACH_SDL3_TARGET, popups will be silent")
endif()

if(RECOMP_ACH_IMGUI_TARGET)
  if(NOT TARGET ${RECOMP_ACH_IMGUI_TARGET})
    message(FATAL_ERROR "RECOMP_ACH_IMGUI_TARGET '${RECOMP_ACH_IMGUI_TARGET}' is not a target")
  endif()
  target_sources(recomp_achievements_ui PRIVATE
      ${_ach_ui_src}/imgui_renderer.cpp
      ${_ach_ui_src}/achievement_list.cpp
      ${_ach_ui_src}/popup_ui.cpp)
  target_link_libraries(recomp_achievements_ui PUBLIC ${RECOMP_ACH_IMGUI_TARGET})
  target_compile_definitions(recomp_achievements_ui PUBLIC RECOMP_ACH_HAS_IMGUI=1)
else()
  target_compile_definitions(recomp_achievements_ui PUBLIC RECOMP_ACH_HAS_IMGUI=0)
  message(STATUS "recomp-achievements: no RECOMP_ACH_IMGUI_TARGET, building the UI without its ImGui renderer")
endif()

if(NOT MSVC)
  target_compile_options(recomp_achievements_ui_base PRIVATE -Wall -Wextra)
  target_compile_options(recomp_achievements_ui PRIVATE -Wall -Wextra)
endif()

if(RECOMP_ACH_BUILD_TESTS)
  enable_testing()
  find_package(Threads REQUIRED)
  set(_ach_ui_tests ${CMAKE_CURRENT_LIST_DIR}/../tests/ui)
  # Only the base library: these tests must run without ImGui or SDL.
  add_executable(recomp_achievements_ui_tests
      ${_ach_ui_tests}/test_main.cpp
      ${_ach_ui_tests}/test_notification_center.cpp
      ${_ach_ui_tests}/test_style.cpp
      ${_ach_ui_tests}/test_sounds.cpp
      ${_ach_ui_tests}/test_icon_cache.cpp
      ${_ach_ui_tests}/test_canvas.cpp
      ${_ach_ui_tests}/test_overlay.cpp
      ${_ach_ui_tests}/test_library_icons.cpp
      ${_ach_ui_tests}/test_library_page.cpp)
  target_link_libraries(recomp_achievements_ui_tests PRIVATE recomp_achievements_ui_base Threads::Threads)
  # The examples in docs/styling.md are checked for unknown keys.
  target_compile_definitions(recomp_achievements_ui_tests PRIVATE
      RECOMP_ACH_UI_STYLING_DOC="${CMAKE_CURRENT_LIST_DIR}/../docs/styling.md"
      RECOMP_ACH_UI_FONT_DIR="${CMAKE_CURRENT_LIST_DIR}/../assets/fonts")
  add_test(NAME recomp_achievements_ui_tests COMMAND recomp_achievements_ui_tests)

  if(RECOMP_ACH_IMGUI_TARGET)
    # Headless frames through the real renderer and list window.
    add_executable(recomp_achievements_ui_imgui_tests
        ${_ach_ui_tests}/test_main.cpp
        ${_ach_ui_tests}/test_imgui_renderer.cpp)
    target_link_libraries(recomp_achievements_ui_imgui_tests PRIVATE recomp_achievements_ui)
    add_test(NAME recomp_achievements_ui_imgui_tests COMMAND recomp_achievements_ui_imgui_tests)
  endif()
endif()
