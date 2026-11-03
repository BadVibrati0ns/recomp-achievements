# Memory triggers: RetroAchievements-syntax conditions evaluated with rcheevos.
# Only rcheevos' condition parser/evaluator is built; rc_client, rapi, rhash and
# the URL/network code are left out so a recomp can never talk to RA.
if(NOT RECOMP_ACH_MEMORY)
  return()
endif()

include(FetchContent)
FetchContent_Declare(recomp_ach_rcheevos
    GIT_REPOSITORY https://github.com/RetroAchievements/rcheevos.git
    GIT_TAG v11.6.0
    GIT_SHALLOW TRUE)
# rcheevos has no CMakeLists.txt, so this only downloads it.
FetchContent_MakeAvailable(recomp_ach_rcheevos)
set(RECOMP_ACH_RCHEEVOS_DIR ${recomp_ach_rcheevos_SOURCE_DIR})

add_library(recomp_achievements_rcheevos STATIC
    ${RECOMP_ACH_RCHEEVOS_DIR}/src/rcheevos/alloc.c
    ${RECOMP_ACH_RCHEEVOS_DIR}/src/rcheevos/condition.c
    ${RECOMP_ACH_RCHEEVOS_DIR}/src/rcheevos/condset.c
    ${RECOMP_ACH_RCHEEVOS_DIR}/src/rcheevos/memref.c
    ${RECOMP_ACH_RCHEEVOS_DIR}/src/rcheevos/operand.c
    ${RECOMP_ACH_RCHEEVOS_DIR}/src/rcheevos/trigger.c
    ${RECOMP_ACH_RCHEEVOS_DIR}/src/rcheevos/value.c
    ${RECOMP_ACH_RCHEEVOS_DIR}/src/rc_compat.c
    ${RECOMP_ACH_RCHEEVOS_DIR}/src/rc_util.c)
target_include_directories(recomp_achievements_rcheevos
    PUBLIC ${RECOMP_ACH_RCHEEVOS_DIR}/include
    PRIVATE ${RECOMP_ACH_RCHEEVOS_DIR}/src)
target_compile_definitions(recomp_achievements_rcheevos PUBLIC RC_DISABLE_LUA)
set_target_properties(recomp_achievements_rcheevos PROPERTIES
    POSITION_INDEPENDENT_CODE ON C_STANDARD 99)
if(NOT MSVC)
  target_compile_options(recomp_achievements_rcheevos PRIVATE -w)
endif()

# The System-free half (GuestMemory, ConditionEvaluator), usable on its own.
add_library(recomp_achievements_memory_eval STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../src/memory/guest_memory.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/memory/condition_evaluator.cpp)
target_include_directories(recomp_achievements_memory_eval PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/../include)
target_link_libraries(recomp_achievements_memory_eval PRIVATE recomp_achievements_rcheevos)

add_library(recomp_achievements_memory STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../src/memory/memory_triggers.cpp)
add_library(recomp::achievements_memory ALIAS recomp_achievements_memory)
target_link_libraries(recomp_achievements_memory PUBLIC
    recomp_achievements_memory_eval recomp_achievements)

if(RECOMP_ACH_BUILD_TESTS)
  enable_testing()
  add_executable(recomp_achievements_memory_tests
      ${CMAKE_CURRENT_LIST_DIR}/../tests/memory/test_main.cpp
      ${CMAKE_CURRENT_LIST_DIR}/../tests/memory/evaluator_tests.cpp
      ${CMAKE_CURRENT_LIST_DIR}/../tests/memory/system_tests.cpp)
  target_link_libraries(recomp_achievements_memory_tests PRIVATE recomp_achievements_memory)
  target_compile_definitions(recomp_achievements_memory_tests PRIVATE
      RECOMP_ACH_MEMORY_TEST_DATA="${CMAKE_CURRENT_LIST_DIR}/../tests/memory/data")
  # One CTest entry per case, so a failure is reported by name.
  set(RECOMP_ACH_MEMORY_TEST_CASES
      guest_memory_regions guest_memory_n64_swizzle guest_memory_ps2 guest_memory_x360
      guest_memory_peek_callback parse_error simple_compare waiting_state hit_counts
      reset_if pause_if measured measured_percent delta_compare n64_condition x360_condition
      reset_rearm primed
      system_unlock system_parse_error system_progress system_resync system_imported_set)
  foreach(case IN LISTS RECOMP_ACH_MEMORY_TEST_CASES)
    add_test(NAME memory.${case} COMMAND recomp_achievements_memory_tests ${case})
  endforeach()
endif()
