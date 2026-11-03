# Platform readers: Xbox 360 XDBF/SPA and PS3 TROPHY.TRP. No third-party
# dependencies; they link the core for the shared types and System.
set(_ra_src ${CMAKE_CURRENT_LIST_DIR}/../src/sources)

add_library(recomp_achievements_xbox STATIC ${_ra_src}/xdbf.cpp ${_ra_src}/xam.cpp)
add_library(recomp::achievements_xbox ALIAS recomp_achievements_xbox)
target_link_libraries(recomp_achievements_xbox PUBLIC recomp_achievements)

add_library(recomp_achievements_trophy STATIC
    ${_ra_src}/trophy.cpp
    ${_ra_src}/trophy_context.cpp
    ${_ra_src}/mini_xml.cpp)
add_library(recomp::achievements_trophy ALIAS recomp_achievements_trophy)
target_link_libraries(recomp_achievements_trophy PUBLIC recomp_achievements)

if(RECOMP_ACH_BUILD_TESTS)
  enable_testing()
  set(_ra_tests ${CMAKE_CURRENT_LIST_DIR}/../tests/sources)
  add_executable(recomp_achievements_sources_tests
      ${_ra_tests}/main.cpp
      ${_ra_tests}/xdbf_tests.cpp
      ${_ra_tests}/xam_tests.cpp
      ${_ra_tests}/trophy_tests.cpp)
  target_include_directories(recomp_achievements_sources_tests PRIVATE ${_ra_src} ${_ra_tests})
  target_link_libraries(recomp_achievements_sources_tests PRIVATE
      recomp_achievements_xbox recomp_achievements_trophy)
  add_test(NAME recomp_achievements_sources_tests COMMAND recomp_achievements_sources_tests)
  # Reads a real spa.bin (path from this variable or the RECOMP_ACH_TEST_SPA
  # environment variable); skipped (exit 77) when there is none.
  set(RECOMP_ACH_TEST_SPA "" CACHE FILEPATH "A real spa.bin for the optional XDBF test")
  add_test(NAME recomp_achievements_sources_real_spa
      COMMAND recomp_achievements_sources_tests --real-spa "${RECOMP_ACH_TEST_SPA}")
  set_tests_properties(recomp_achievements_sources_real_spa PROPERTIES SKIP_RETURN_CODE 77)
endif()
