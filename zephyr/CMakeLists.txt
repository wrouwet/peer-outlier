# SPDX-License-Identifier: Apache-2.0
if(CONFIG_POD)
  set(POD_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
  zephyr_include_directories(${POD_ROOT}/include)
  zephyr_library()
  zephyr_library_sources(
    ${POD_ROOT}/src/iforest.c
    ${POD_ROOT}/src/robust.c
    ${POD_ROOT}/src/pod.c
  )
  if(CONFIG_POD_CMIS)
    zephyr_include_directories(${POD_ROOT}/adapters/cmis/include)
    zephyr_library_sources(${POD_ROOT}/adapters/cmis/src/pod_cmis.c)
  endif()
  if(CONFIG_POD_CMIS_FLEET_SIM)
    zephyr_include_directories(${POD_ROOT}/sim)
    zephyr_library_sources(${POD_ROOT}/sim/fleet_sim.c)
  endif()
endif()
