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

  add_executable(test_app test_app.c)
  target_link_libraries(test_app PRIVATE qtc qtc_test m)
  qtc_add_test(app $<TARGET_FILE:test_app>)
endif()

add_executable(test_sweep test_sweep.cpp)
target_link_libraries(test_sweep PRIVATE qtc Qt6::Core)
qtc_add_test(sweep $<TARGET_FILE:test_sweep>)

add_executable(test_stress test_stress.cpp)
target_link_libraries(test_stress PRIVATE qtc Qt6::Core Threads::Threads)
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
