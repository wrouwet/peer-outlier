# SPDX-License-Identifier: Apache-2.0
#
# Standalone build of pod for any C toolchain (host, bare-metal, RTOS).
# Zephyr users don't need this file: the repository is also a Zephyr module
# (see zephyr/).

cmake_minimum_required(VERSION 3.16)
project(pod VERSION 0.1.0 LANGUAGES C)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(POD_TOP_LEVEL ON)
else()
  set(POD_TOP_LEVEL OFF)
endif()

option(POD_BUILD_CMIS     "Build the CMIS optical-module adapter" ON)
option(POD_BUILD_TESTS    "Build the host tests"                  ${POD_TOP_LEVEL})
option(POD_BUILD_EXAMPLES "Build the host examples"               ${POD_TOP_LEVEL})

find_library(POD_LIBM m)

# Core library: C99 + <math.h>, no allocation, no I/O, no OS.
add_library(pod src/iforest.c src/robust.c src/pod.c)
target_include_directories(pod PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
set_target_properties(pod PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(POD_LIBM)
  target_link_libraries(pod PUBLIC ${POD_LIBM})
endif()

if(POD_BUILD_CMIS)
  add_library(pod_cmis adapters/cmis/src/pod_cmis.c)
  target_include_directories(pod_cmis PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/adapters/cmis/include>
    $<INSTALL_INTERFACE:include>)
  set_target_properties(pod_cmis PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  target_link_libraries(pod_cmis PUBLIC pod)
endif()

if(POD_BUILD_EXAMPLES)
  add_executable(pod_example_basic examples/basic/main.c)
  target_link_libraries(pod_example_basic PRIVATE pod)
endif()

if(POD_BUILD_EXAMPLES AND POD_BUILD_CMIS)
  add_executable(pod_example_cmis_drift examples/cmis_drift/main.c sim/fleet_sim.c)
  target_include_directories(pod_example_cmis_drift PRIVATE sim)
  target_link_libraries(pod_example_cmis_drift PRIVATE pod pod_cmis)
  set_target_properties(pod_example_cmis_drift PROPERTIES C_STANDARD 11)
endif()

if(POD_BUILD_TESTS AND POD_BUILD_CMIS)
  enable_testing()
  add_executable(test_pod
    tests/host/test_core.c tests/host/test_cmis.c sim/fleet_sim.c)
  target_include_directories(test_pod PRIVATE sim)
  target_link_libraries(test_pod PRIVATE pod pod_cmis)
  set_target_properties(test_pod PROPERTIES C_STANDARD 11)
  add_test(NAME pod COMMAND test_pod)
endif()

include(GNUInstallDirs)
install(TARGETS pod EXPORT podTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/pod DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(POD_BUILD_CMIS)
  install(TARGETS pod_cmis EXPORT podTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(DIRECTORY adapters/cmis/include/pod_cmis DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
install(EXPORT podTargets NAMESPACE pod:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pod)
