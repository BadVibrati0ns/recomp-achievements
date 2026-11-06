add_library(recomp_achievements STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../src/system.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/json_format.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/config_dir.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/library.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/library_reader.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/library_util.cpp)
add_library(recomp::achievements ALIAS recomp_achievements)
target_include_directories(recomp_achievements PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/../include
    ${CMAKE_CURRENT_LIST_DIR}/../third_party)
# The library writer runs a worker thread.
find_package(Threads REQUIRED)
target_link_libraries(recomp_achievements PUBLIC Threads::Threads)

if(RECOMP_ACH_BUILD_TESTS)
  enable_testing()
  add_executable(recomp_achievements_core_tests
      ${CMAKE_CURRENT_LIST_DIR}/../tests/core/test_main.cpp
      ${CMAKE_CURRENT_LIST_DIR}/../tests/core/test_json_format.cpp
      ${CMAKE_CURRENT_LIST_DIR}/../tests/core/test_compat.cpp
      ${CMAKE_CURRENT_LIST_DIR}/../tests/core/test_system.cpp
      ${CMAKE_CURRENT_LIST_DIR}/../tests/core/test_settings.cpp)
  # The format tests reach the private parser directly.
  target_include_directories(recomp_achievements_core_tests PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../src)
  target_link_libraries(recomp_achievements_core_tests PRIVATE recomp::achievements)
  target_compile_definitions(recomp_achievements_core_tests PRIVATE
      RECOMP_ACH_TEST_DATA="${CMAKE_CURRENT_LIST_DIR}/../tests/core/data"
      RECOMP_ACH_TEST_TMP="${CMAKE_CURRENT_BINARY_DIR}/core_test_tmp"
      RECOMP_ACH_SCHEMA="${CMAKE_CURRENT_LIST_DIR}/../schema/achievement-set.schema.json")
  if(NOT MSVC)
    target_compile_options(recomp_achievements_core_tests PRIVATE -Wall -Wextra)
  endif()
  find_package(Threads REQUIRED)
  target_link_libraries(recomp_achievements_core_tests PRIVATE Threads::Threads)
  add_test(NAME recomp_achievements_core_tests COMMAND recomp_achievements_core_tests)

  # The shared achievement library: writer, reader and path helpers.
  add_executable(recomp_achievements_library_tests
      ${CMAKE_CURRENT_LIST_DIR}/../tests/core/test_main.cpp
      ${CMAKE_CURRENT_LIST_DIR}/../tests/library/test_library.cpp
      ${CMAKE_CURRENT_LIST_DIR}/../tests/library/test_library_reader.cpp
      ${CMAKE_CURRENT_LIST_DIR}/../tests/library/test_paths.cpp)
  target_include_directories(recomp_achievements_library_tests PRIVATE
      ${CMAKE_CURRENT_LIST_DIR}/../src ${CMAKE_CURRENT_LIST_DIR}/../tests/core)
  target_link_libraries(recomp_achievements_library_tests PRIVATE recomp::achievements)
  target_compile_definitions(recomp_achievements_library_tests PRIVATE
      RECOMP_ACH_TEST_DATA="${CMAKE_CURRENT_LIST_DIR}/../tests/core/data"
      RECOMP_ACH_TEST_TMP="${CMAKE_CURRENT_BINARY_DIR}/library_test_tmp")
  if(NOT MSVC)
    target_compile_options(recomp_achievements_library_tests PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME recomp_achievements_library_tests COMMAND recomp_achievements_library_tests)
endif()
