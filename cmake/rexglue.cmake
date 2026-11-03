# Binding for rexglue (Xbox 360) recomps. Built only when the consumer names
# its rexglue runtime target. With RECOMP_ACH_REXGLUE_PATCHED (rexglue built
# with patches/rexglue-achievements.patch) it hooks the kernel callbacks;
# without it, the recomp's own XAM shims call RexglueBinding::xam().
if(RECOMP_ACH_REXGLUE_TARGET STREQUAL "")
  return()
endif()
if(NOT TARGET ${RECOMP_ACH_REXGLUE_TARGET})
  message(FATAL_ERROR "RECOMP_ACH_REXGLUE_TARGET '${RECOMP_ACH_REXGLUE_TARGET}' is not a target")
endif()

add_library(recomp_achievements_rexglue STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../src/rexglue/rexglue_binding.cpp)
add_library(recomp::achievements_rexglue ALIAS recomp_achievements_rexglue)
target_link_libraries(recomp_achievements_rexglue
    PUBLIC recomp_achievements recomp_achievements_xbox
    PRIVATE ${RECOMP_ACH_REXGLUE_TARGET})
target_compile_definitions(recomp_achievements_rexglue PRIVATE
    RECOMP_ACH_REXGLUE_KERNEL_HOOKS=$<BOOL:${RECOMP_ACH_REXGLUE_PATCHED}>)
# rexglue's headers use C++23 (std::byteswap).
target_compile_features(recomp_achievements_rexglue PRIVATE cxx_std_23)
