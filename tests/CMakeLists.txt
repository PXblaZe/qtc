find_package(Threads REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
find_package(Python3 COMPONENTS Interpreter)

set(QTC_TEST_ENV
  "QT_QPA_PLATFORM=offscreen"
  "LC_ALL=C.UTF-8"
  "QT_LOGGING_RULES=*.debug=false;qt.qpa.*=false;qt.qml.*=false"
  "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:detect_stack_use_after_return=1"
  "UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")

function(qtc_add_test name)
  add_test(NAME ${name} COMMAND ${ARGN} WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "${QTC_TEST_ENV}" TIMEOUT 1800)
endfunction()

if(TARGET qtc_test)
  add_executable(test_core test_core.c)
  target_link_libraries(test_core PRIVATE qtc qtc_test Threads::Threads m)
  qtc_add_test(core $<TARGET_FILE:test_core>)

  add_executable(test_concurrent test_concurrent.c)
  target_link_libraries(test_concurrent PRIVATE qtc Threads::Threads)
  qtc_add_test(concurrent $<TARGET_FILE:test_concurrent>)

  add_executable(test_override test_override.c)
  target_link_libraries(test_override PRIVATE qtc qtc_test Threads::Threads)
  set_target_properties(test_override PROPERTIES ENABLE_EXPORTS ON)   # qtc_set_override_handler_symbol
  qtc_add_test(override $<TARGET_FILE:test_override>)

  # graphics: the Null backend here; OpenGL and Vulkan too where a display (or Xvfb) and drivers are
  add_executable(test_rhi test_rhi.c)
  target_link_libraries(test_rhi PRIVATE qtc)
  qtc_add_test(rhi $<TARGET_FILE:test_rhi>)
  find_program(QTC_XVFB_RUN xvfb-run)
  if(QTC_XVFB_RUN)
    add_test(NAME rhi_xcb COMMAND ${QTC_XVFB_RUN} -a -s "-screen 0 1024x768x24" $<TARGET_FILE:test_rhi>
             WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set(_qtc_xcb_env ${QTC_TEST_ENV})
    list(REMOVE_ITEM _qtc_xcb_env "QT_QPA_PLATFORM=offscreen")
    list(APPEND _qtc_xcb_env "QT_QPA_PLATFORM=xcb")
    set_tests_properties(rhi_xcb PROPERTIES ENVIRONMENT "${_qtc_xcb_env}" TIMEOUT 1800)
  endif()

  # widgets on Wayland, against a headless weston (skipped where there is none)
  add_executable(test_wayland test_wayland.c)
  target_link_libraries(test_wayland PRIVATE qtc)
  find_program(QTC_WESTON weston)
  if(QTC_WESTON)
    add_test(NAME wayland COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_wayland.sh $<TARGET_FILE:test_wayland>
             WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set(_qtc_wl_env ${QTC_TEST_ENV})
    list(REMOVE_ITEM _qtc_wl_env "QT_QPA_PLATFORM=offscreen")
    list(APPEND _qtc_wl_env "WESTON=${QTC_WESTON}")
    set_tests_properties(wayland PROPERTIES ENVIRONMENT "${_qtc_wl_env}" TIMEOUT 600 SKIP_RETURN_CODE 77)
  endif()

  add_executable(fuzz_rhi fuzz_rhi.c)
  target_link_libraries(fuzz_rhi PRIVATE qtc)
  qtc_add_test(fuzz_rhi $<TARGET_FILE:fuzz_rhi> 1 20000)
  if(QTC_XVFB_RUN)
    add_test(NAME fuzz_rhi_xcb COMMAND ${QTC_XVFB_RUN} -a -s "-screen 0 1024x768x24" $<TARGET_FILE:fuzz_rhi> 2 20000
             WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(fuzz_rhi_xcb PROPERTIES ENVIRONMENT "${_qtc_xcb_env}" TIMEOUT 1800)
  endif()

  add_executable(test_class test_class.c)
  target_link_libraries(test_class PRIVATE qtc Threads::Threads)
  qtc_add_test(class $<TARGET_FILE:test_class>)

  add_executable(test_app test_app.c)
  target_link_libraries(test_app PRIVATE qtc qtc_test m)
  qtc_add_test(app $<TARGET_FILE:test_app>)
endif()

add_executable(test_sweep test_sweep.cpp)
target_link_libraries(test_sweep PRIVATE qtc Qt6::Core)
qtc_add_test(sweep $<TARGET_FILE:test_sweep>)

add_executable(test_stress test_stress.cpp)
target_link_libraries(test_stress PRIVATE qtc Qt6::Gui Threads::Threads)
qtc_add_test(stress $<TARGET_FILE:test_stress>)

add_executable(fuzz_methods fuzz_methods.cpp)
target_link_libraries(fuzz_methods PRIVATE qtc Qt6::Core)
if(Python3_FOUND)
  qtc_add_test(fuzz ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/fuzz_driver.py
    --binary $<TARGET_FILE:fuzz_methods> --work ${CMAKE_CURRENT_BINARY_DIR}/fuzz --report ${CMAKE_CURRENT_BINARY_DIR}/fuzz-report.json)
  set_tests_properties(fuzz PROPERTIES LABELS slow TIMEOUT 14400)
endif()

add_executable(bench bench.cpp)
target_link_libraries(bench PRIVATE qtc Qt6::Core Qt6::Widgets)
