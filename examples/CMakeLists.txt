# The C examples. Each runs interactively, or with --selftest drives itself, checks
# the result and exits (that is how ctest runs them, on Qt's offscreen platform).
set(QTC_EXAMPLES hello form canvas qml threads)
find_package(Threads REQUIRED)
foreach(ex IN LISTS QTC_EXAMPLES)
  add_executable(qtc_example_${ex} ${ex}.c)
  set_target_properties(qtc_example_${ex} PROPERTIES OUTPUT_NAME ${ex} C_STANDARD 11 C_STANDARD_REQUIRED ON)
  target_link_libraries(qtc_example_${ex} PRIVATE qtc)
  if(QTC_BUILD_TESTS)
    add_test(NAME example_${ex} COMMAND qtc_example_${ex} --selftest WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(example_${ex} PROPERTIES TIMEOUT 60
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_LOGGING_RULES=*=false;LC_ALL=C.UTF-8;ASAN_OPTIONS=detect_leaks=0:abort_on_error=1;UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")
  endif()
endforeach()
target_link_libraries(qtc_example_threads PRIVATE Threads::Threads)
