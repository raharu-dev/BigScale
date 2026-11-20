# Test-only Harness. Never staged into dist/.
cmake_minimum_required(VERSION 3.20)
project(StreamlineMfgSpike CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(STREAMLINE_DIR "${REPO_ROOT}/third_party/streamline" CACHE PATH "Streamline SDK root")
set(MFG_ROOT "${REPO_ROOT}/third_party/mfg-ada-unlock" CACHE PATH "MFGAdaUnlock source root")
set(MINHOOK_DIR "${REPO_ROOT}/third_party/minhook" CACHE PATH "MinHook root")
set(DLSSG_DLL "${REPO_ROOT}/third_party/dlss-sdk/lib/Windows_x86_64/rel/nvngx_dlssg.dll" CACHE FILEPATH "Pinned DLSS-G runtime")

# The SDK archive layout is normalize-flattened by scripts/fetch-deps.ps1, but the library and the
# runtime still live in architecture subdirectories whose exact names are not relied on here.
file(GLOB_RECURSE SL_INTERPOSER_LIB "${STREAMLINE_DIR}/*/sl.interposer.lib")
if(NOT SL_INTERPOSER_LIB)
  message(FATAL_ERROR "sl.interposer.lib not found under '${STREAMLINE_DIR}'. Run scripts/fetch-deps.ps1 first.")
endif()
if(NOT EXISTS "${STREAMLINE_DIR}/include/sl.h")
  message(FATAL_ERROR "sl.h not found under '${STREAMLINE_DIR}/include'.")
endif()
if(NOT EXISTS "${MFG_ROOT}/src/addons/mfgunlock/binary_patch.hpp")
  message(FATAL_ERROR "mfgunlock headers not found under '${MFG_ROOT}'. Run scripts/fetch-deps.ps1 first.")
endif()

add_executable(StreamlineMfgSpike
  main.cpp
  "${MINHOOK_DIR}/src/buffer.c"
  "${MINHOOK_DIR}/src/hook.c"
  "${MINHOOK_DIR}/src/trampoline.c"
  "${MINHOOK_DIR}/src/hde/hde64.c")

target_include_directories(StreamlineMfgSpike PRIVATE
  "${STREAMLINE_DIR}/include"
  "${MFG_ROOT}/src/addons/mfgunlock"
  "${MINHOOK_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/shim")

# sl.interposer.dll exports CreateDXGIFactory2 and D3D12CreateDevice, so the interposer owns device
# creation and Streamline hooks the swapchain automatically. d3d12.lib/dxgi.lib are deliberately not
# linked, and D3D11CreateDevice is resolved from d3d11.dll at run time so the capture device is never
# routed through the interposer.
target_link_libraries(StreamlineMfgSpike PRIVATE
  "${SL_INTERPOSER_LIB}"
  user32
  kernel32)

set_property(TARGET StreamlineMfgSpike PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
target_compile_options(StreamlineMfgSpike PRIVATE /W3 /permissive-)
target_compile_definitions(StreamlineMfgSpike PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

# Every Streamline runtime next to the executable, then the pinned DLSS-G provider over any copy
# the SDK ships so the unlock profiles bind to an exact known image.
add_custom_command(TARGET StreamlineMfgSpike POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${STREAMLINE_DIR}/bin/x64" "$<TARGET_FILE_DIR:StreamlineMfgSpike>"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${DLSSG_DLL}" "$<TARGET_FILE_DIR:StreamlineMfgSpike>/nvngx_dlssg.dll"
  VERBATIM)
