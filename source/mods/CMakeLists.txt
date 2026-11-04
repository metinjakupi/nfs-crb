# The plugins this game ships: its bundled core mods (mods/core/<id>/).
add_custom_target(core_mods
  COMMAND ${Python3_EXECUTABLE} ${POP_ROOT}/tools/recomp/build_core.py
          --source ${CMAKE_CURRENT_SOURCE_DIR}/core
          --dest ${POP_OUT}/mods/core --cc ${CMAKE_C_COMPILER}
  WORKING_DIRECTORY ${POP_ROOT}
  COMMENT "Installing core mods into build/recomp/mods/core"
  VERBATIM)

add_custom_target(plugins DEPENDS core_mods)

# Compare the native audio fast path with the original translated routine.
if(POP_REAL_GEN AND RECOMP_NATIVE_HEADER AND NOT EMSCRIPTEN AND NOT IOS AND NOT ANDROID)
  add_executable(nfsc_audio_scale_tests
    ${RECOMP_GAME_DIR}/tests/audio_scale_tests.cpp
    ${POP_ROOT}/tools/recomp/tests/harness.c)
  target_include_directories(nfsc_audio_scale_tests PRIVATE ${POP_ROOT} ${POP_ROOT}/runtime ${POP_GEN_DIR})
  pop_link_gen(nfsc_audio_scale_tests)
  pop_optimize(nfsc_audio_scale_tests 2)
  pop_test_binary(nfsc_audio_scale_tests)
  add_test(NAME nfsc_audio_scale_tests COMMAND nfsc_audio_scale_tests WORKING_DIRECTORY ${POP_ROOT})
  set_tests_properties(nfsc_audio_scale_tests PROPERTIES LABELS game)

  add_executable(nfsc_geometry_tests
    ${RECOMP_GAME_DIR}/tests/geometry_tests.cpp
    ${POP_ROOT}/tools/recomp/tests/harness.c)
  target_include_directories(nfsc_geometry_tests PRIVATE ${POP_ROOT} ${POP_ROOT}/runtime ${POP_GEN_DIR})
  pop_link_gen(nfsc_geometry_tests)
  pop_optimize(nfsc_geometry_tests 2)
  pop_test_binary(nfsc_geometry_tests)
  add_test(NAME nfsc_geometry_tests COMMAND nfsc_geometry_tests WORKING_DIRECTORY ${POP_ROOT})
  set_tests_properties(nfsc_geometry_tests PROPERTIES LABELS game)
endif()
