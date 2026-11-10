cmake_minimum_required(VERSION 3.21)
project(qtc VERSION 1.0.0 LANGUAGES C CXX DESCRIPTION "Qt 6 for C, safe by construction")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(QTC_BUILD_TESTS "Build the test suite" ON)
option(QTC_BUILD_TESTLIB "Build libqtc_test (input simulation through QtTest)" ON)
option(QTC_BUILD_EXAMPLES "Build the C examples" ON)
option(QTC_ALLOW_OLDER_QT "Also accept Qt 6.4 to 6.11 (untested: Qt 6.12 is the supported release)" OFF)
set(QTC_SANITIZE "" CACHE STRING "Sanitizers for the library and tests, e.g. address,undefined")
set(QTC_MODULES "" CACHE STRING "Qt modules to bind (default: every module that is installed)")
set(QTC_GEN_OPT "" CACHE STRING "Optimization flag for the generated sources (default: the build type's)")

include(${CMAKE_CURRENT_SOURCE_DIR}/src/gen/modules.cmake)
if(QTC_MODULES)
  set(_wanted ${QTC_MODULES})
else()
  set(_wanted ${QTC_GEN_MODULES})
endif()

# Qt 6.12 LTS is the release qtc is tested and supported on. The generated code still compiles
# each method only where it exists, so older Qt 6 releases can be allowed explicitly; they are
# not tested.
if(QTC_ALLOW_OLDER_QT)
  set(_qtc_qt_min 6.4)
else()
  set(_qtc_qt_min 6.12)
endif()
find_package(Qt6 ${_qtc_qt_min} QUIET COMPONENTS Core)
if(NOT Qt6_FOUND)
  set(_seen "")
  if(Qt6_CONSIDERED_VERSIONS)
    set(_old ${Qt6_CONSIDERED_VERSIONS})
    list(REMOVE_DUPLICATES _old)
    list(JOIN _old ", " _old)
    set(_seen " Found only Qt ${_old}.")
  endif()
  message(FATAL_ERROR "qtc needs Qt ${_qtc_qt_min} or newer; Qt 6.12 LTS is the supported release. "
    "Point CMAKE_PREFIX_PATH at its install prefix.${_seen} "
    "To build against Qt 6.4 to 6.11 anyway (untested), pass -DQTC_ALLOW_OLDER_QT=ON.")
endif()
if(Qt6_VERSION VERSION_LESS 6.12)
  message(WARNING "qtc: building against Qt ${Qt6_VERSION}. Only Qt 6.12 is tested: methods newer "
    "than this release are left out, and crashes particular to it may not be guarded.")
endif()
set(QTC_ENABLED_MODULES "")
foreach(m IN LISTS QTC_GEN_MODULES)
  if(NOT m IN_LIST _wanted)
    continue()
  endif()
  set(_ok TRUE)
  foreach(d IN LISTS QTC_GEN_DEPS_${m})
    if(d AND NOT d IN_LIST QTC_ENABLED_MODULES)
      set(_ok FALSE)
    endif()
  endforeach()
  if(NOT _ok)
    continue()
  endif()
  find_package(Qt6 QUIET COMPONENTS ${m})
  if(TARGET Qt6::${m})
    list(APPEND QTC_ENABLED_MODULES ${m})
  endif()
endforeach()
message(STATUS "qtc: Qt ${Qt6_VERSION}, binding modules: ${QTC_ENABLED_MODULES}")

set(QTC_SOURCES
  src/qtc_core.cpp
  src/qtc_events.cpp
  src/qtc_roles.cpp
  src/qtc_builtin.cpp
  src/qtc_builtin.h
  ${QTC_GEN_COMMON})
set(QTC_LINK Qt6::Core)
set(QTC_DEFS QTC_BUILDING QT_NO_KEYWORDS)
foreach(m IN LISTS QTC_ENABLED_MODULES)
  list(APPEND QTC_SOURCES ${QTC_GEN_${m}})
  list(APPEND QTC_LINK Qt6::${m})
  list(APPEND QTC_DEFS QTC_HAS_MODULE_${m})
endforeach()
if("Gui" IN_LIST QTC_ENABLED_MODULES)
  list(APPEND QTC_DEFS QTC_HAS_GUI)
endif()
if("Widgets" IN_LIST QTC_ENABLED_MODULES)
  list(APPEND QTC_DEFS QTC_HAS_WIDGETS)
endif()

add_library(qtc SHARED ${QTC_SOURCES})
set_target_properties(qtc PROPERTIES
  AUTOMOC ON
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(qtc PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_definitions(qtc PRIVATE ${QTC_DEFS})
target_link_libraries(qtc PRIVATE ${QTC_LINK})
if(MSVC)
  target_compile_options(qtc PRIVATE /bigobj /W3 /Zc:__cplusplus /permissive-)
else()
  target_compile_options(qtc PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing)
endif()
if(QTC_GEN_OPT)
  set_source_files_properties(${QTC_GEN_COMMON} PROPERTIES COMPILE_OPTIONS "${QTC_GEN_OPT}")
  foreach(m IN LISTS QTC_ENABLED_MODULES)
    set_source_files_properties(${QTC_GEN_${m}} PROPERTIES COMPILE_OPTIONS "${QTC_GEN_OPT}")
  endforeach()
endif()
if(QTC_SANITIZE)
  target_compile_options(qtc PUBLIC -fsanitize=${QTC_SANITIZE} -fno-omit-frame-pointer)
  target_link_options(qtc PUBLIC -fsanitize=${QTC_SANITIZE})
endif()
add_library(qtc::qtc ALIAS qtc)

if(QTC_BUILD_TESTLIB)
  find_package(Qt6 QUIET COMPONENTS Test)
  if(TARGET Qt6::Test AND "Widgets" IN_LIST QTC_ENABLED_MODULES)
    add_library(qtc_test SHARED src/qtc_testlib.cpp include/qtc_test.h)
    set_target_properties(qtc_test PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}
      CXX_VISIBILITY_PRESET hidden)
    target_include_directories(qtc_test PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
    target_compile_definitions(qtc_test PRIVATE QTC_TEST_BUILDING)
    target_link_libraries(qtc_test PUBLIC qtc PRIVATE Qt6::Test Qt6::Widgets)
    add_library(qtc::qtc_test ALIAS qtc_test)
  endif()
endif()

include(GNUInstallDirs)
install(TARGETS qtc EXPORT qtcTargets LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(TARGET qtc_test)
  install(TARGETS qtc_test EXPORT qtcTargets LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(FILES include/qtc_test.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
install(FILES include/qtc.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES manifest/qtc_api.json DESTINATION ${CMAKE_INSTALL_DATADIR}/qtc)
install(EXPORT qtcTargets NAMESPACE qtc:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qtc)
include(CMakePackageConfigHelpers)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/qtcConfig.cmake "include(\${CMAKE_CURRENT_LIST_DIR}/qtcTargets.cmake)\n")
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/qtcConfigVersion.cmake COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/qtcConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/qtcConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qtc)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/qtc.pc.in ${CMAKE_CURRENT_BINARY_DIR}/qtc.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/qtc.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

if(QTC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(QTC_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
