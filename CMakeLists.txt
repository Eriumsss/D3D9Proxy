cmake_minimum_required(VERSION 3.20)
project(D3D9Proxy LANGUAGES CXX)

# Retail Conquest is a 32-bit executable: compiling an x64 proxy is invalid.
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "D3D9Proxy requires a Windows x86 (Win32) build")
endif()

set(PROXY_SOURCES
  src/d3d9device.cpp
  src/d3d9proxy.cpp
  src/overlay.cpp
  src/memorypatch.cpp
  src/filehook.cpp
  src/vespucci_bridge.cpp
  src/stronghold_log.cpp
  src/crowd_event_handler.cpp
  src/retail_fn_trace.cpp
  src/retail_heap_extender.cpp
  src/static_pool_census.cpp
  src/model_registry_2048.cpp
  src/texture_registry_4096.cpp
  src/effect_registry_1024.cpp
  src/gfx_registry_512.cpp
  src/auxiliary_registry_8192.cpp
  src/device_reset_pool_4096.cpp
  src/dynamic_buffer_reset_pools_4096.cpp
  src/geometry_budget_telemetry.cpp
  src/retail_texture_vault.cpp
)
add_library(d3d9 SHARED ${PROXY_SOURCES} src/d3d9proxy.def)
target_compile_features(d3d9 PRIVATE cxx_std_17)
target_include_directories(d3d9 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(d3d9 PRIVATE WIN32 WIN32_LEAN_AND_MEAN NOMINMAX _WINDLL)
if(MSVC)
  target_compile_options(d3d9 PRIVATE /EHsc /W0)
  set_property(TARGET d3d9 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()
target_link_libraries(d3d9 PRIVATE d3d9.lib dxguid.lib uuid.lib user32.lib kernel32.lib)
set_target_properties(d3d9 PROPERTIES OUTPUT_NAME "d3d9" PREFIX "")
