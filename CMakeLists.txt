cmake_minimum_required(VERSION 3.21)
project(qtc VERSION 1.1.0 LANGUAGES C CXX DESCRIPTION "Qt 6 for C, safe by construction")

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
# Licensing (docs/LICENSING.md). qtc is MIT. Most of Qt is available under the LGPL-3.0, which
# closed-source programs may use (dynamically linked, as libqtc links Qt). These Qt modules are
# available under the GPL-3.0 only (or a commercial Qt license): a libqtc linking one of them makes
# every program using it subject to the GPL when distributed. They are left out unless asked for.
set(QTC_GPL_ONLY_MODULES Charts NetworkAuth HttpServer Mqtt Coap)
option(QTC_GPL_MODULES "Also bind the GPL-3.0-only Qt modules: ${QTC_GPL_ONLY_MODULES}" OFF)

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
set(_qtc_gpl_left_out "")
foreach(m IN LISTS QTC_GEN_MODULES)
  if(NOT m IN_LIST _wanted)
    continue()
  endif()
  if(m IN_LIST QTC_GPL_ONLY_MODULES AND NOT QTC_GPL_MODULES)
    if(QTC_MODULES AND m IN_LIST QTC_MODULES)
      message(FATAL_ERROR "qtc: Qt ${m} is available under the GPL-3.0 only (or a commercial Qt license), and a "
        "libqtc binding it makes programs that use it GPL-3.0 when distributed. Pass -DQTC_GPL_MODULES=ON to bind "
        "it anyway (see docs/LICENSING.md).")
    endif()
    list(APPEND _qtc_gpl_left_out ${m})
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
set(QTC_ENABLED_GPL_MODULES "")
foreach(m IN LISTS QTC_ENABLED_MODULES)
  if(m IN_LIST QTC_GPL_ONLY_MODULES)
    list(APPEND QTC_ENABLED_GPL_MODULES ${m})
  endif()
endforeach()
if(QTC_ENABLED_GPL_MODULES)
  message(WARNING "qtc: this libqtc binds GPL-3.0-only Qt modules (${QTC_ENABLED_GPL_MODULES}). Programs that use "
    "it may be distributed only under GPL-3.0-compatible terms, unless you hold a commercial Qt license "
    "(see docs/LICENSING.md).")
elseif(_qtc_gpl_left_out)
  message(STATUS "qtc: left out the GPL-3.0-only modules ${_qtc_gpl_left_out} (-DQTC_GPL_MODULES=ON binds them)")
endif()

set(QTC_SOURCES
  src/qtc_core.cpp
  src/qtc_events.cpp
  src/qtc_roles.cpp
  src/qtc_concurrent.cpp
  src/qtc_override.cpp
  src/qtc_class.cpp
  src/qtc_rhi.cpp
  src/qtc_override.h
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

# runtime classes build their meta-objects with QMetaObjectBuilder, a private Qt Core API
find_package(Qt6 QUIET COMPONENTS CorePrivate)
if(TARGET Qt6::CorePrivate)
  list(APPEND QTC_LINK Qt6::CorePrivate)
else()
  list(APPEND QTC_DEFS QTC_NO_RUNTIME_CLASSES)
  message(STATUS "qtc: Qt's private Core headers are missing: runtime classes (qtc_class_*) are left out")
endif()

# the graphics layer: Qt's RHI (semi-public API of Qt GUI) and, to compile shaders at run time,
# Qt Shader Tools (LGPL-3.0 like Qt GUI; without it shaders come as .qsb packages)
if("Gui" IN_LIST QTC_ENABLED_MODULES)
  find_package(Qt6 QUIET COMPONENTS GuiPrivate)
  if(TARGET Qt6::GuiPrivate)
    list(APPEND QTC_LINK Qt6::GuiPrivate)
    option(QTC_SHADER_TOOLS "Compile shaders at run time with Qt Shader Tools (qtc_rhi_shader_bake)" ON)
    if(QTC_SHADER_TOOLS)
      find_package(Qt6 QUIET COMPONENTS ShaderTools ShaderToolsPrivate)
      if(TARGET Qt6::ShaderToolsPrivate)
        list(APPEND QTC_LINK Qt6::ShaderTools Qt6::ShaderToolsPrivate)
        list(APPEND QTC_DEFS QTC_HAS_SHADERTOOLS)
      else()
        message(STATUS "qtc: Qt Shader Tools is missing: qtc_rhi_shader_bake reports QTC_ERR_UNSUPPORTED (load .qsb packages)")
      endif()
    endif()
  else()
    message(STATUS "qtc: Qt's GUI private headers are missing: the RHI (qtc_rhi_*) reports QTC_ERR_UNSUPPORTED")
  endif()
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
# Installed libraries find the Qt they were linked with (when it is outside the system's library
# directories, as an installer's or a conda Qt is) and qtc_test finds qtc beside it, so programs
# and language bindings that load them at run time need no library path.
option(QTC_INSTALL_RPATH "Give the installed libraries the run path of the Qt they link" ON)
if(QTC_INSTALL_RPATH AND NOT WIN32)
  set_target_properties(qtc PROPERTIES INSTALL_RPATH_USE_LINK_PATH ON)
  if(TARGET qtc_test)
    if(APPLE)
      set_target_properties(qtc_test PROPERTIES INSTALL_RPATH_USE_LINK_PATH ON INSTALL_RPATH "@loader_path")
    else()
      set_target_properties(qtc_test PROPERTIES INSTALL_RPATH_USE_LINK_PATH ON INSTALL_RPATH "$ORIGIN")
    endif()
  endif()
endif()
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
