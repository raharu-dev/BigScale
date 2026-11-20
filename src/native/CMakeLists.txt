cmake_minimum_required(VERSION 3.20)
project(BigScaleNative LANGUAGES C CXX)

if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "BigScale.Native requires Windows x64 and MSVC.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Configure an x64 build (use -A x64).")
endif()

# ---------------------------------------------------------------------------------------------
# Pinned NVIDIA DLSS SDK (310.9.1). Headers only at compile time; the static NGX loader is linked
# in, and the release runtime DLL is staged next to the bridge for NGX to load at runtime.
# ---------------------------------------------------------------------------------------------
set(NGX_SDK_DIR "" CACHE PATH "Root of the pinned NVIDIA DLSS SDK checkout")
if(NGX_SDK_DIR STREQUAL "")
  get_filename_component(NGX_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/dlss-sdk" ABSOLUTE)
endif()
find_path(NGX_INCLUDE_DIR nvsdk_ngx.h PATHS "${NGX_SDK_DIR}/include" NO_DEFAULT_PATH)
if(NOT NGX_INCLUDE_DIR)
  message(FATAL_ERROR "nvsdk_ngx.h not found under NGX_SDK_DIR='${NGX_SDK_DIR}'. Run scripts/fetch-deps.ps1.")
endif()
find_library(NGX_STATIC_LIBRARY nvsdk_ngx_s PATHS "${NGX_SDK_DIR}/lib/Windows_x86_64/x64" NO_DEFAULT_PATH)
if(NOT NGX_STATIC_LIBRARY)
  message(FATAL_ERROR "nvsdk_ngx_s.lib not found under NGX_SDK_DIR='${NGX_SDK_DIR}'.")
endif()
set(NGX_RUNTIME_DLL "${NGX_SDK_DIR}/lib/Windows_x86_64/rel/nvngx_dlss.dll")
if(NOT EXISTS "${NGX_RUNTIME_DLL}")
  message(FATAL_ERROR "Release runtime nvngx_dlss.dll not found under NGX_SDK_DIR='${NGX_SDK_DIR}'.")
endif()

# ---------------------------------------------------------------------------------------------
# AMD FSR 2 / FSR 3 DX11. Both are statically linked; only the DX11 backend and the upscaler
# component are built, with the bridge's static MSVC runtime so no CRT crosses the DLL boundary.
# ---------------------------------------------------------------------------------------------
set(FSR2_DX11_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/fsr2-dx11" CACHE PATH "FSR 2 DX11 port root")
option(BIGSCALE_BUILD_FSR "Build the AMD FSR 2 DX11 backend" ON)

set(BIGSCALE_UPSCALER_SOURCES "")
set(BIGSCALE_FSR_LIBRARIES "")
if(BIGSCALE_BUILD_FSR)
  if(NOT EXISTS "${FSR2_DX11_DIR}/src/ffx-fsr2-api/CMakeLists.txt")
    message(FATAL_ERROR "FSR 2 sources missing under FSR2_DX11_DIR='${FSR2_DX11_DIR}'. Run scripts/fetch-deps.ps1.")
  endif()

  set(FFX_FSR2_API_DX11 ON CACHE BOOL "" FORCE)
  set(FFX_FSR2_API_DX12 OFF CACHE BOOL "" FORCE)
  set(FFX_FSR2_API_VK OFF CACHE BOOL "" FORCE)
  set(FSR2_BUILD_AS_DLL OFF CACHE BOOL "" FORCE)
  set(FSR2_AUTO_COMPILE_SHADERS ON CACHE BOOL "" FORCE)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" CACHE STRING "" FORCE)
  add_subdirectory("${FSR2_DX11_DIR}/src/ffx-fsr2-api" "${CMAKE_CURRENT_BINARY_DIR}/fsr2" EXCLUDE_FROM_ALL)

  list(APPEND BIGSCALE_UPSCALER_SOURCES providers/Fsr2Backend.cpp providers/Fsr2Backend.h)
  list(APPEND BIGSCALE_FSR_LIBRARIES ffx_fsr2_api_x64 ffx_fsr2_api_dx11_x64)
endif()

# ---------------------------------------------------------------------------------------------
# Background-DX12 backends: official AMD FSR 4 and Intel XeSS. Both run on the shared D3D12
# interop device; their runtimes are loaded from the plugin directory at execution time.
# ---------------------------------------------------------------------------------------------
set(XESS_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/xess-sdk" CACHE PATH "XeSS SDK root")
set(FSR_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/fsr-sdk" CACHE PATH "AMD FidelityFX SDK root")
set(STREAMLINE_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/streamline" CACHE PATH "NVIDIA Streamline SDK root")
set(MFG_UNLOCK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/mfg-ada-unlock" CACHE PATH "MFGAdaUnlock profile source root")
set(MFG_COMPAT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/mfg-compat" CACHE PATH "Include compatibility root for the profile headers")
set(ANR_FG_DIR "" CACHE PATH "Local ANR frame-generation runtime root (never redistributed)")
set(BIGSCALE_DX12_BACKEND_SOURCES "")
set(BIGSCALE_DX12_INCLUDES "")

# The D3D12 bridge and the isolated D3D11 context state are always compiled: frame generation needs
# them even when no background-DX12 super-resolution provider is built.
list(APPEND BIGSCALE_DX12_BACKEND_SOURCES
  Dx12Interop.cpp Dx12Interop.h
  ContextState.cpp ContextState.h
  NgxSynchronization.cpp NgxSynchronization.h
  FrameGeneration.cpp FrameGeneration.h FrameGenerationAbi.h FrameGenerationApi.cpp
  VrFrameGenerationAbi.h
  vr/VrFrameGeneration.cpp vr/VrFrameGeneration.h
  vr/VrFrameGenerationApi.cpp
  vr/VrFsrInterpolator.cpp vr/VrFsrInterpolator.h)

if(EXISTS "${XESS_SDK_DIR}/inc/xess/xess.h")
  list(APPEND BIGSCALE_DX12_BACKEND_SOURCES providers/XessBackend.cpp providers/XessBackend.h)
  list(APPEND BIGSCALE_DX12_INCLUDES "${XESS_SDK_DIR}/inc")
else()
  message(WARNING "XeSS SDK not found under '${XESS_SDK_DIR}'; XeSS super resolution stays unavailable.")
endif()

if(EXISTS "${FSR_SDK_DIR}/Kits/FidelityFX/api/include/ffx_api.h")
  list(APPEND BIGSCALE_DX12_BACKEND_SOURCES providers/FsrDx12Backend.cpp providers/FsrDx12Backend.h)
  list(APPEND BIGSCALE_DX12_INCLUDES
    "${FSR_SDK_DIR}/Kits/FidelityFX/api/include"
    "${FSR_SDK_DIR}/Kits/FidelityFX/api/include/dx12"
    "${FSR_SDK_DIR}/Kits/FidelityFX/upscalers/include")
else()
  message(WARNING "AMD FidelityFX SDK not found under '${FSR_SDK_DIR}'; FSR super resolution stays unavailable.")
endif()

# ---------------------------------------------------------------------------------------------
# Frame generation is a required part of this package: every provider adapter must exist, so
# missing SDK headers are a build failure rather than a silently disabled feature.
# ---------------------------------------------------------------------------------------------
if(NOT EXISTS "${FSR_SDK_DIR}/Kits/FidelityFX/framegeneration/include/ffx_framegeneration.h")
  message(FATAL_ERROR "FidelityFX frame-generation headers are missing under FSR_SDK_DIR='${FSR_SDK_DIR}'. Run scripts/fetch-deps.ps1.")
endif()
if(NOT EXISTS "${XESS_SDK_DIR}/inc/xess_fg/xefg_swapchain.h")
  message(FATAL_ERROR "XeSS frame-generation headers are missing under XESS_SDK_DIR='${XESS_SDK_DIR}'. Run scripts/fetch-deps.ps1.")
endif()
if(NOT EXISTS "${STREAMLINE_SDK_DIR}/include/sl.h")
  message(FATAL_ERROR "Streamline headers are missing under STREAMLINE_SDK_DIR='${STREAMLINE_SDK_DIR}'. Run scripts/fetch-deps.ps1.")
endif()
if(NOT EXISTS "${MFG_UNLOCK_DIR}/src/addons/mfgunlock/binary_patch.hpp" OR
   NOT EXISTS "${MFG_COMPAT_DIR}/include/reshade.hpp")
  message(FATAL_ERROR "MFGAdaUnlock profile headers are missing. Run scripts/fetch-deps.ps1.")
endif()
list(APPEND BIGSCALE_DX12_BACKEND_SOURCES
  fg/FrameGenerationProvider.h
  fg/FrameGenerationResources.cpp fg/FrameGenerationResources.h
  fg/FgCadence.cpp fg/FgCadence.h
  fg/FgCapture.cpp fg/FgCapture.h
  fg/FgReadback.cpp fg/FgReadback.h
  fg/FgTestObserver.cpp fg/FgTestObserver.h
  fg/FgMath.h
  fg/NgxParameterMap.cpp fg/NgxParameterMap.h
  fg/NgxFgProvider.cpp fg/NgxFgProvider.h
  fg/StreamlineFgProvider.cpp fg/StreamlineFgProvider.h
  fg/StreamlinePatch.cpp fg/StreamlinePatch.h
  fg/FsrFgRuntime.cpp fg/FsrFgRuntime.h
  fg/FsrFgProvider.cpp fg/FsrFgProvider.h
  fg/XessFgProvider.cpp fg/XessFgProvider.h
  fg/XellLatency.cpp fg/XellLatency.h)
list(APPEND BIGSCALE_DX12_INCLUDES
  "${FSR_SDK_DIR}/Kits/FidelityFX/framegeneration/include"
  "${FSR_SDK_DIR}/Kits/FidelityFX/framegeneration/include/dx12"
  "${XESS_SDK_DIR}/inc"
  "${STREAMLINE_SDK_DIR}/include"
  "${MFG_UNLOCK_DIR}/src/addons/mfgunlock"
  "${MFG_COMPAT_DIR}")

# ---------------------------------------------------------------------------------------------
# Valve OpenVR SDK headers (BSD-3-Clause). Only the interface declarations are used: the compositor
# and system objects are obtained from VR_GetGenericInterface at execution time, and nothing links
# openvr_api.lib. The comments' interface versions are pinned by scripts/fetch-deps.ps1.
# ---------------------------------------------------------------------------------------------
set(OPENVR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/openvr" CACHE PATH "Valve OpenVR SDK root")
if(NOT EXISTS "${OPENVR_DIR}/headers/openvr.h")
  message(FATAL_ERROR "OpenVR headers are missing under OPENVR_DIR='${OPENVR_DIR}'. Run scripts/fetch-deps.ps1.")
endif()
list(APPEND BIGSCALE_DX12_INCLUDES "${OPENVR_DIR}/headers")

# The profile headers use defaulted comparison operators (C++20) but expose only POD types through
# StreamlinePatch.h, so the language level is raised for this one translation unit.
set_source_files_properties(fg/StreamlinePatch.cpp PROPERTIES COMPILE_OPTIONS "/std:c++20")

# ---------------------------------------------------------------------------------------------
# MinHook (BSD-2-Clause) provides the transient DXGI detours used by the desktop presenter.
# ---------------------------------------------------------------------------------------------
set(MINHOOK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/minhook" CACHE PATH "MinHook root")
if(EXISTS "${MINHOOK_DIR}/src/hook.c")
  add_library(bigscale_minhook STATIC
    "${MINHOOK_DIR}/src/buffer.c"
    "${MINHOOK_DIR}/src/hook.c"
    "${MINHOOK_DIR}/src/trampoline.c"
    "${MINHOOK_DIR}/src/hde/hde64.c")
  target_include_directories(bigscale_minhook PUBLIC "${MINHOOK_DIR}/include")
  set_property(TARGET bigscale_minhook PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
  target_compile_options(bigscale_minhook PRIVATE /W3)
  set(BIGSCALE_MINHOOK_LIB bigscale_minhook)
else()
  message(WARNING "MinHook not found under '${MINHOOK_DIR}'; desktop frame generation stays unavailable.")
  set(BIGSCALE_MINHOOK_LIB "")
endif()

# ---------------------------------------------------------------------------------------------
# Build-time SM5 compilation. fxc ships with the Windows SDK that Visual Studio installs; there is
# no runtime d3dcompiler dependency and no Unity Editor dependency.
# ---------------------------------------------------------------------------------------------
set(FXC_EXECUTABLE "" CACHE FILEPATH "DirectX Shader Compiler (fxc.exe)")
if(FXC_EXECUTABLE STREQUAL "")
  # A shader compiler already on PATH wins; otherwise fall back to the newest Windows SDK copy.
  find_program(FXC_EXECUTABLE NAMES fxc)
endif()
if(FXC_EXECUTABLE STREQUAL "")
  file(GLOB FXC_CANDIDATES "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/fxc.exe")
  if(NOT FXC_CANDIDATES)
    message(FATAL_ERROR "fxc.exe was not found on PATH or under 'C:/Program Files (x86)/Windows Kits/10/bin/*/x64'. Install the Windows SDK or pass -DFXC_EXECUTABLE=<path>.")
  endif()
  list(SORT FXC_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
  list(GET FXC_CANDIDATES 0 FXC_EXECUTABLE)
  set(FXC_EXECUTABLE "${FXC_EXECUTABLE}" CACHE FILEPATH "DirectX Shader Compiler (fxc.exe)" FORCE)
endif()
message(STATUS "Using fxc: ${FXC_EXECUTABLE}")

# symbol name = source name. The generated arrays are k<Symbol>Cso / k<Symbol>CsoSize.
set(SHADER_SYMBOLS ConvertColor ConvertDepth ConvertMotion Upsample ResampleMotion
    GraphToScreenDepth GraphToScreenMotion GraphToScreenMotionHalf GraphToScreenColor)
set(SHADER_FILES convert_color convert_depth convert_motion upsample resample_motion
    graph_to_screen_depth graph_to_screen_motion graph_to_screen_motion_half graph_to_screen_color)
set(SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUTPUT_DIR}")

set(SHADER_ENTRIES "")
set(SHADER_OUTPUTS "")
list(LENGTH SHADER_FILES SHADER_COUNT)
math(EXPR SHADER_LAST "${SHADER_COUNT} - 1")
foreach(index RANGE ${SHADER_LAST})
  list(GET SHADER_SYMBOLS ${index} symbol)
  list(GET SHADER_FILES ${index} name)
  set(hlsl "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${name}.hlsl")
  set(cso "${SHADER_OUTPUT_DIR}/${name}.cso")
  add_custom_command(
    OUTPUT "${cso}"
    COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_0 /E main /O3 /Fo "${cso}" "${hlsl}"
    DEPENDS "${hlsl}"
    COMMENT "Compiling shaders/${name}.hlsl (SM5 compute)"
    VERBATIM)
  list(APPEND SHADER_OUTPUTS "${cso}")
  list(APPEND SHADER_ENTRIES "${symbol}=${cso}")
endforeach()
string(REPLACE ";" "|" SHADER_ENTRIES "${SHADER_ENTRIES}")

set(GENERATED_HEADER "${CMAKE_CURRENT_BINARY_DIR}/shaders_generated.h")
add_custom_command(
  OUTPUT "${GENERATED_HEADER}"
  COMMAND "${CMAKE_COMMAND}" "-DPAIRS=${SHADER_ENTRIES}" "-DOUTPUT=${GENERATED_HEADER}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/gen_shaders.cmake"
  DEPENDS ${SHADER_OUTPUTS} "${CMAKE_CURRENT_SOURCE_DIR}/gen_shaders.cmake"
  COMMENT "Embedding compiled shader bytecode into shaders_generated.h"
  VERBATIM)

# ---------------------------------------------------------------------------------------------
# The bridge DLL.
# ---------------------------------------------------------------------------------------------
add_library(BigScale.Native SHARED
  Bridge.cpp Bridge.h
  Upscaler.h
  NgxBackend.cpp NgxBackend.h
  EyeResources.cpp EyeResources.h
  StageProbe.cpp StageProbe.h
  vr/XrPresentationProof.cpp vr/XrPresentationProof.h
  TexelReader.cpp TexelReader.h
  Log.cpp Log.h
  Shaders.h
  ${BIGSCALE_UPSCALER_SOURCES}
  ${BIGSCALE_DX12_BACKEND_SOURCES})
add_custom_target(BigScale.NativeShaders DEPENDS "${GENERATED_HEADER}")
add_dependencies(BigScale.Native BigScale.NativeShaders)

target_compile_features(BigScale.Native PRIVATE cxx_std_17)
target_compile_definitions(BigScale.Native PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(BIGSCALE_BUILD_FSR)
  target_compile_definitions(BigScale.Native PRIVATE BIGSCALE_WITH_FSR=1)
endif()
target_compile_definitions(BigScale.Native PRIVATE _WINDOWS=1)
target_include_directories(BigScale.Native PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${NGX_INCLUDE_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}" ${BIGSCALE_DX12_INCLUDES})
if(BIGSCALE_BUILD_FSR)
  target_include_directories(BigScale.Native PRIVATE "${FSR2_DX11_DIR}/src/ffx-fsr2-api")
endif()
target_link_libraries(BigScale.Native PRIVATE d3d11 d3d12 dxgi dxguid gdi32 version "${NGX_STATIC_LIBRARY}" ${BIGSCALE_FSR_LIBRARIES} ${BIGSCALE_MINHOOK_LIB})
set_property(TARGET BigScale.Native PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
target_compile_options(BigScale.Native PRIVATE /W4 /EHsc /utf-8 /Zi)
# Emit a program database next to the bridge so the diagnostics archive can carry stack symbols.
target_link_options(BigScale.Native PRIVATE /DEBUG /OPT:REF)
set_target_properties(BigScale.Native PROPERTIES LINK_FLAGS "/DEBUG /OPT:REF")
set_property(TARGET BigScale.Native PROPERTY MSVC_DEBUG_INFORMATION_FORMAT ProgramDatabase)
set_target_properties(BigScale.Native PROPERTIES
  PREFIX ""
  OUTPUT_NAME "BigScale.Native"
  FOLDER "BigScale")

# ---------------------------------------------------------------------------------------------
# Offline D3D11 checks. They compile the same production sources and never load the game or Unity.
# ---------------------------------------------------------------------------------------------
option(BIGSCALE_BUILD_CHECKS "Build the offline D3D11 check executable" OFF)
if(BIGSCALE_BUILD_CHECKS)
  add_executable(BigScaleChecks
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/NativeChecks/main.cpp"
    Bridge.cpp Bridge.h
    Upscaler.h
    NgxBackend.cpp NgxBackend.h
    EyeResources.cpp EyeResources.h
    StageProbe.cpp StageProbe.h
    vr/XrPresentationProof.cpp vr/XrPresentationProof.h
    TexelReader.cpp TexelReader.h
    Log.cpp Log.h
    Shaders.h
    ${BIGSCALE_UPSCALER_SOURCES}
    ${BIGSCALE_DX12_BACKEND_SOURCES})
  add_dependencies(BigScaleChecks BigScale.NativeShaders)
  target_compile_features(BigScaleChecks PRIVATE cxx_std_17)
  target_compile_definitions(BigScaleChecks PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  if(BIGSCALE_BUILD_FSR)
    target_compile_definitions(BigScaleChecks PRIVATE BIGSCALE_WITH_FSR=1)
  endif()
  target_compile_definitions(BigScaleChecks PRIVATE _WINDOWS=1 BIGSCALE_TESTING=1)
  target_include_directories(BigScaleChecks PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${NGX_INCLUDE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}" ${BIGSCALE_DX12_INCLUDES})
  if(BIGSCALE_BUILD_FSR)
    target_include_directories(BigScaleChecks PRIVATE "${FSR2_DX11_DIR}/src/ffx-fsr2-api")
  endif()
  target_link_libraries(BigScaleChecks PRIVATE d3d11 d3d12 dxgi dxguid gdi32 version "${NGX_STATIC_LIBRARY}" ${BIGSCALE_FSR_LIBRARIES} ${BIGSCALE_MINHOOK_LIB})
  target_compile_options(BigScaleChecks PRIVATE /W4 /EHsc /utf-8 /Zi)
  # Symbols let the harness's crash handler name the faulting frame instead of an address.
  target_link_options(BigScaleChecks PRIVATE /DEBUG /OPT:REF)
  set_property(TARGET BigScaleChecks PROPERTY MSVC_DEBUG_INFORMATION_FORMAT ProgramDatabase)
  set_property(TARGET BigScaleChecks PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
  set_target_properties(BigScaleChecks PROPERTIES
    OUTPUT_NAME "BigScaleChecks"
    FOLDER "BigScale")
  add_custom_command(TARGET BigScaleChecks POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${NGX_RUNTIME_DLL}"
            "$<TARGET_FILE_DIR:BigScaleChecks>/nvngx_dlss.dll"
    COMMENT "Staging nvngx_dlss.dll next to the offline checks"
    VERBATIM)
  foreach(runtime
      "${XESS_SDK_DIR}/bin/libxess.dll"
      "${XESS_SDK_DIR}/bin/libxess_fg.dll"
      "${XESS_SDK_DIR}/bin/libxell.dll"
      "${FSR_SDK_DIR}/Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll"
      "${FSR_SDK_DIR}/Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_dx12.dll"
      "${STREAMLINE_SDK_DIR}/bin/x64/sl.interposer.dll"
      "${STREAMLINE_SDK_DIR}/bin/x64/sl.common.dll"
      "${STREAMLINE_SDK_DIR}/bin/x64/sl.dlss_g.dll"
      "${STREAMLINE_SDK_DIR}/bin/x64/sl.reflex.dll"
      "${STREAMLINE_SDK_DIR}/bin/x64/sl.pcl.dll"
      "${NGX_SDK_DIR}/lib/Windows_x86_64/rel/nvngx_dlssg.dll")
    if(EXISTS "${runtime}")
      add_custom_command(TARGET BigScaleChecks POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${runtime}"
                "$<TARGET_FILE_DIR:BigScaleChecks>"
        COMMENT "Staging ${runtime} next to the offline checks"
        VERBATIM)
    endif()
  endforeach()
  if(ANR_FG_DIR AND EXISTS "${ANR_FG_DIR}/nvngx_dlssg.dll")
    add_custom_command(TARGET BigScaleChecks POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_directory "${ANR_FG_DIR}"
              "$<TARGET_FILE_DIR:BigScaleChecks>/providers/anr"
      COMMENT "Staging the local ANR runtime next to the offline checks"
      VERBATIM)
  endif()
endif()

# Stage the verified NVIDIA release runtime beside the bridge so NGX can find it through
# FeatureCommonInfo.PathListInfo without touching process-wide DLL search paths.
add_custom_command(TARGET BigScale.Native POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${NGX_RUNTIME_DLL}" "$<TARGET_FILE_DIR:BigScale.Native>/nvngx_dlss.dll"
  COMMENT "Staging nvngx_dlss.dll next to BigScale.Native.dll"
  VERBATIM)
# Streamline is loaded from the plugin directory; the provider never touches process DLL search.
add_custom_command(TARGET BigScale.Native POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${STREAMLINE_SDK_DIR}/bin/x64/sl.interposer.dll"
          "${STREAMLINE_SDK_DIR}/bin/x64/sl.common.dll"
          "${STREAMLINE_SDK_DIR}/bin/x64/sl.dlss_g.dll"
          "${STREAMLINE_SDK_DIR}/bin/x64/sl.reflex.dll"
          "${STREAMLINE_SDK_DIR}/bin/x64/sl.pcl.dll"
          "$<TARGET_FILE_DIR:BigScale.Native>"
  COMMENT "Staging the Streamline runtime next to BigScale.Native.dll"
  VERBATIM)
