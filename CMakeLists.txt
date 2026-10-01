# Power Observatory - electrical observability runtime for data center control planes.
# Copyright 2026 Summon Software Labs. Apache License 2.0.
cmake_minimum_required(VERSION 3.21)

project(power-observatory
  VERSION 1.0.0
  DESCRIPTION "Electrical observability runtime for data center control planes"
  HOMEPAGE_URL "https://github.com/summonlabs/Power-Observatory"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(PO_BUILD_CLI "Build the power-observatory command line tool" ON)
option(PO_BUILD_TESTS "Build the Power Observatory test suites" ${PROJECT_IS_TOP_LEVEL})
option(PO_BUILD_BENCHMARKS "Build the Power Observatory benchmark harness" ${PROJECT_IS_TOP_LEVEL})
option(PO_WARNINGS_AS_ERRORS "Treat first-party compiler diagnostics as errors" ON)
option(PO_ENABLE_ASAN "Build and link with AddressSanitizer where the toolchain supports it" OFF)
option(PO_ENABLE_UBSAN "Build and link with UndefinedBehaviorSanitizer where the toolchain supports it" OFF)
option(PO_ENABLE_STATIC_ANALYSIS "Run the platform static analyser as part of the build" OFF)

if(PO_ENABLE_ASAN AND PO_ENABLE_STATIC_ANALYSIS)
  message(FATAL_ERROR "PO_ENABLE_ASAN and PO_ENABLE_STATIC_ANALYSIS cannot both be enabled: /analyze and /fsanitize=address are incompatible")
endif()

if(PO_ENABLE_UBSAN AND MSVC)
  message(STATUS "PO_ENABLE_UBSAN has no MSVC equivalent and is ignored on this toolchain")
  set(PO_ENABLE_UBSAN OFF)
endif()

# ---------------------------------------------------------------------------
# Language and binary defaults
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  # /RTC1 is incompatible with /fsanitize=address.
  if(PO_ENABLE_ASAN)
    foreach(po_flag_var CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELWITHDEBINFO)
      string(REPLACE "/RTC1" "" ${po_flag_var} "${${po_flag_var}}")
    endforeach()
  endif()
endif()

# ---------------------------------------------------------------------------
# Warning policy. First-party code must compile clean at this level.
# ---------------------------------------------------------------------------
function(po_apply_warning_policy target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4
      /permissive-
      /utf-8
      /EHsc
      /Zc:__cplusplus
      /Zc:preprocessor
      /Zc:inline
      /diagnostics:caret)
    if(PO_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    if(PO_ENABLE_STATIC_ANALYSIS)
      target_compile_options(${target} PRIVATE /analyze /analyze:external-)
    endif()
    if(PO_ENABLE_ASAN)
      target_compile_options(${target} PRIVATE /fsanitize=address)
      target_link_options(${target} PRIVATE /fsanitize=address)
      target_compile_definitions(${target} PRIVATE PO_ASAN_ENABLED=1)
    endif()
    set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  else()
    target_compile_options(${target} PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wshadow
      -Wconversion
      -Wsign-conversion
      -Wold-style-cast
      -Wnon-virtual-dtor
      -Woverloaded-virtual
      -Wnull-dereference
      -Wdouble-promotion)
    if(PO_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
    # flock(2) and posix_spawn(3) are declared only while _DEFAULT_SOURCE (or
    # _GNU_SOURCE) is in effect. CMAKE_CXX_EXTENSIONS is OFF, so the compiler is
    # invoked with -std=c++20 and __STRICT_ANSI__ is defined, which would
    # otherwise hide them. The macro is set explicitly instead of relying on
    # libstdc++ happening to define _GNU_SOURCE before the first system header.
    target_compile_definitions(${target} PRIVATE _DEFAULT_SOURCE)
    set(po_sanitizers "")
    if(PO_ENABLE_ASAN)
      list(APPEND po_sanitizers address)
    endif()
    if(PO_ENABLE_UBSAN)
      list(APPEND po_sanitizers undefined)
      # Undefined behaviour is a defect, not a recoverable event: stop at the
      # first finding rather than logging it and carrying on.
      target_compile_options(${target} PRIVATE -fno-sanitize-recover=all)
    endif()
    if(po_sanitizers)
      string(JOIN "," po_sanitizer_list ${po_sanitizers})
      target_compile_options(${target} PRIVATE -fsanitize=${po_sanitizer_list} -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=${po_sanitizer_list})
    endif()
    # The build description reports which sanitizers are active. GCC defines
    # __SANITIZE_ADDRESS__ but has no macro at all for -fsanitize=undefined, so
    # the compiler builtins alone would describe a combined build as address
    # only. The build system knows what it actually passed, so it says so.
    if(PO_ENABLE_ASAN)
      target_compile_definitions(${target} PRIVATE PO_SANITIZER_ASAN=1)
    endif()
    if(PO_ENABLE_UBSAN)
      target_compile_definitions(${target} PRIVATE PO_SANITIZER_UBSAN=1)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
# The runtime owns a worker thread and synchronises with mutexes and condition
# variables. On POSIX those symbols live in the system thread library, and a
# downstream consumer linking the installed archive needs exactly the same
# dependency, so it is declared PUBLIC and is re-exported through the package.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

set(PO_CORE_SOURCES
  src/version.cpp
  src/reason.cpp
  src/strong.cpp
  src/quantity.cpp
  src/time.cpp
  src/evidence.cpp
  src/topology.cpp
  src/policy.cpp
  src/explanation.cpp
  src/freshness.cpp
  src/context.cpp
  src/flow.cpp
  src/reserve.cpp
  src/attribution.cpp
  src/quality.cpp
  src/failover.cpp
  src/file_io.cpp
  src/file_lock.cpp
  src/persistence.cpp
  src/snapshot.cpp
  src/divergence.cpp
  src/runtime.cpp
  src/json.cpp
  src/json_reports.cpp
  src/scenario.cpp
  src/cli.cpp
)

add_library(power_observatory STATIC ${PO_CORE_SOURCES})
add_library(PowerObservatory::power_observatory ALIAS power_observatory)

target_link_libraries(power_observatory PUBLIC Threads::Threads)

target_include_directories(power_observatory
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

po_apply_warning_policy(power_observatory)

# ---------------------------------------------------------------------------
# Command line tool
# ---------------------------------------------------------------------------
if(PO_BUILD_CLI)
  add_executable(power-observatory apps/main.cpp)
  target_link_libraries(power-observatory PRIVATE power_observatory)
  po_apply_warning_policy(power-observatory)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(PO_BUILD_TESTS)
  enable_testing()

  add_library(po_test_support STATIC tests/support/test_harness.cpp)
  target_include_directories(po_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/tests/support")
  target_link_libraries(po_test_support PUBLIC power_observatory)
  po_apply_warning_policy(po_test_support)

  # Test sources are discovered rather than listed so that adding a suite is a
  # single-file change. CONFIGURE_DEPENDS makes the build regenerate when a file
  # appears or disappears.
  file(GLOB po_test_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")

  foreach(po_test_source IN LISTS po_test_sources)
    get_filename_component(po_test_name "${po_test_source}" NAME_WE)
    add_executable(${po_test_name} "${po_test_source}")
    target_link_libraries(${po_test_name} PRIVATE power_observatory po_test_support)
    target_compile_definitions(${po_test_name} PRIVATE
      PO_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
      PO_CLI_PATH="$<TARGET_FILE:power-observatory>")
    po_apply_warning_policy(${po_test_name})
    add_test(NAME ${po_test_name} COMMAND ${po_test_name})
    # Each suite runs in a scratch directory of its own so that suites which
    # create files cannot collide when CTest runs them in parallel.
    add_custom_command(TARGET ${po_test_name} POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E make_directory
              "${CMAKE_CURRENT_BINARY_DIR}/scratch/${po_test_name}")
    set_tests_properties(${po_test_name} PROPERTIES
      LABELS "${po_test_name}"
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/scratch/${po_test_name}")
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------
if(PO_BUILD_BENCHMARKS)
  add_executable(bench_answer bench/bench_answer.cpp)
  target_link_libraries(bench_answer PRIVATE power_observatory)
  po_apply_warning_policy(bench_answer)
  add_custom_target(bench COMMAND bench_answer DEPENDS bench_answer
    COMMENT "Running the Power Observatory benchmark harness")
endif()

# ---------------------------------------------------------------------------
# Install and export
# ---------------------------------------------------------------------------
install(TARGETS power_observatory
  EXPORT PowerObservatoryTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(PO_BUILD_CLI)
  install(TARGETS power-observatory
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

# The package directory must be named after the package. CMake searches
# <prefix>/lib/cmake/<name>*/ for <name>Config.cmake, and a directory named
# "power-observatory" does not match the package name "PowerObservatory".
set(PO_CMAKE_PACKAGE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/PowerObservatory")

install(EXPORT PowerObservatoryTargets
  FILE PowerObservatoryTargets.cmake
  NAMESPACE PowerObservatory::
  DESTINATION "${PO_CMAKE_PACKAGE_DIR}")

configure_package_config_file(
  cmake/PowerObservatoryConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/PowerObservatoryConfig.cmake"
  INSTALL_DESTINATION "${PO_CMAKE_PACKAGE_DIR}")

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/PowerObservatoryConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/PowerObservatoryConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/PowerObservatoryConfigVersion.cmake"
  DESTINATION "${PO_CMAKE_PACKAGE_DIR}")
