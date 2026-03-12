cmake_minimum_required(VERSION 3.28)

project(igr_ui VERSION 0.1.0 LANGUAGES CXX)

include(FetchContent)
set(IGR_FETCHCONTENT_BASE_DIR "${PROJECT_SOURCE_DIR}/.cache/fetchcontent" CACHE PATH "Base directory for fetched third-party dependencies")
set(FETCHCONTENT_BASE_DIR "${IGR_FETCHCONTENT_BASE_DIR}")

option(IGR_BUILD_TESTS "Build native tests" ON)
option(IGR_BUILD_EXAMPLES "Build native examples" ON)
option(IGR_ENABLE_GLSL_SHADERS "Enable GLSL fragment shader translation support" ON)
option(IGR_ENABLE_HERMES "Enable Hermes transport runtime integration hooks" ON)
set(IGR_HERMES_VERSION "0.1.27" CACHE STRING "Microsoft.JavaScript.Hermes package version")
set(IGR_HERMES_PACKAGE_DIR "" CACHE PATH "Override path to an unpacked Microsoft.JavaScript.Hermes package")

function(igr_configure_target target_name)
  target_compile_features(${target_name} PUBLIC cxx_std_20)
  target_include_directories(${target_name} PUBLIC "${PROJECT_SOURCE_DIR}/include")
  target_compile_definitions(${target_name} PRIVATE IGR_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

  if(MSVC)
    target_compile_options(${target_name} PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus /utf-8 /FS)
    target_compile_definitions(${target_name} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  else()
    target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

function(igr_copy_runtime_dlls target_name)
  if(WIN32)
    add_custom_command(TARGET ${target_name} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:${target_name}>
        $<TARGET_FILE_DIR:${target_name}>
      COMMAND_EXPAND_LISTS)
  endif()
endfunction()

if(IGR_ENABLE_GLSL_SHADERS)
  set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
  set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
  set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
  set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
  set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_STATIC ON CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_ENABLE_MSL OFF CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
  FetchContent_Declare(glslang
    URL https://github.com/KhronosGroup/glslang/archive/f0bd0257c308b9a26562c1a30c4748a0219cc951.zip
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
  FetchContent_Declare(spirv_cross
    URL https://github.com/KhronosGroup/SPIRV-Cross/archive/a0fba56c34a6700f1724bf9b751da5b488a3775c.zip
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
  FetchContent_MakeAvailable(glslang spirv_cross)
endif()

if(IGR_ENABLE_HERMES)
  if(NOT WIN32)
    message(FATAL_ERROR "IGR_ENABLE_HERMES is only supported on Windows.")
  endif()

  set(_igr_hermes_root "${IGR_HERMES_PACKAGE_DIR}")
  if(_igr_hermes_root STREQUAL "")
    set(_igr_hermes_cache_root "${PROJECT_SOURCE_DIR}/.cache/hermes")
    set(_igr_hermes_root "${_igr_hermes_cache_root}/${IGR_HERMES_VERSION}")
    set(_igr_hermes_archive "${_igr_hermes_cache_root}/microsoft.javascript.hermes.${IGR_HERMES_VERSION}.nupkg")
    set(_igr_hermes_header "${_igr_hermes_root}/build/native/include/hermes/hermes_api.h")

    if(NOT EXISTS "${_igr_hermes_header}")
      file(MAKE_DIRECTORY "${_igr_hermes_cache_root}")

      if(NOT EXISTS "${_igr_hermes_archive}")
        set(_igr_hermes_url "https://api.nuget.org/v3-flatcontainer/microsoft.javascript.hermes/${IGR_HERMES_VERSION}/microsoft.javascript.hermes.${IGR_HERMES_VERSION}.nupkg")
        file(DOWNLOAD "${_igr_hermes_url}" "${_igr_hermes_archive}" STATUS _igr_hermes_download_status SHOW_PROGRESS)
        list(GET _igr_hermes_download_status 0 _igr_hermes_download_code)
        list(GET _igr_hermes_download_status 1 _igr_hermes_download_message)
        if(NOT _igr_hermes_download_code EQUAL 0)
          message(FATAL_ERROR "Failed to download ${_igr_hermes_url}: ${_igr_hermes_download_message}")
        endif()
      endif()

      file(ARCHIVE_EXTRACT INPUT "${_igr_hermes_archive}" DESTINATION "${_igr_hermes_root}")
    endif()
  endif()

  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^[Aa][Rr][Mm]64")
      set(_igr_hermes_arch "arm64")
    else()
      set(_igr_hermes_arch "x64")
    endif()
  else()
    set(_igr_hermes_arch "x86")
  endif()

  set(_igr_hermes_include_root "${_igr_hermes_root}/build/native/include")
  set(_igr_hermes_import_lib "${_igr_hermes_root}/build/native/win32/${_igr_hermes_arch}/hermes.lib")
  set(_igr_hermes_dll "${_igr_hermes_root}/build/native/win32/${_igr_hermes_arch}/hermes.dll")

  if(NOT EXISTS "${_igr_hermes_include_root}/hermes/hermes_api.h")
    message(FATAL_ERROR "Hermes headers were not found at ${_igr_hermes_include_root}.")
  endif()
  if(NOT EXISTS "${_igr_hermes_include_root}/node-api/js_runtime_api.h")
    message(FATAL_ERROR "Hermes Node-API headers were not found at ${_igr_hermes_include_root}/node-api.")
  endif()
  if(NOT EXISTS "${_igr_hermes_import_lib}" OR NOT EXISTS "${_igr_hermes_dll}")
    message(FATAL_ERROR "Hermes libraries were not found for architecture ${_igr_hermes_arch} under ${_igr_hermes_root}.")
  endif()

  add_library(igr_hermes_runtime SHARED IMPORTED GLOBAL)
  set_target_properties(igr_hermes_runtime PROPERTIES
    IMPORTED_IMPLIB "${_igr_hermes_import_lib}"
    IMPORTED_LOCATION "${_igr_hermes_dll}"
    INTERFACE_INCLUDE_DIRECTORIES "${_igr_hermes_include_root};${_igr_hermes_include_root}/node-api")
endif()

add_library(igr_core STATIC
  src/context.cpp
  src/diagnostics.cpp
  src/frame.cpp
  src/interaction.cpp
  src/resources.cpp
  src/result.cpp
)
add_library(igr::core ALIAS igr_core)
igr_configure_target(igr_core)
target_link_libraries(igr_core PUBLIC Psapi)

add_library(igr_shader_compiler STATIC
  src/shader_compiler.cpp
)
add_library(igr::shader_compiler ALIAS igr_shader_compiler)
igr_configure_target(igr_shader_compiler)
target_link_libraries(igr_shader_compiler PUBLIC igr_core d3dcompiler)
if(IGR_ENABLE_GLSL_SHADERS)
  target_compile_definitions(igr_shader_compiler PUBLIC IGR_ENABLE_GLSL_SHADERS=1)
  target_link_libraries(igr_shader_compiler PRIVATE glslang SPIRV glslang-default-resource-limits spirv-cross-core spirv-cross-hlsl)
else()
  target_compile_definitions(igr_shader_compiler PUBLIC IGR_ENABLE_GLSL_SHADERS=0)
endif()

add_library(igr_react_bridge STATIC
  src/react/document.cpp
  src/react/hermes_runtime.cpp
  src/react/runtime_bridge.cpp
  src/react/transport.cpp
)
add_library(igr::react_bridge ALIAS igr_react_bridge)
igr_configure_target(igr_react_bridge)
target_link_libraries(igr_react_bridge PUBLIC igr_core)
if(IGR_ENABLE_HERMES)
  target_compile_definitions(igr_react_bridge PUBLIC IGR_ENABLE_HERMES=1)
  target_link_libraries(igr_react_bridge PRIVATE igr_hermes_runtime)
else()
  target_compile_definitions(igr_react_bridge PUBLIC IGR_ENABLE_HERMES=0)
endif()

add_library(igr_backend_dx11 STATIC
  backends/dx11/dx11_backend.cpp
)
add_library(igr::backend_dx11 ALIAS igr_backend_dx11)
igr_configure_target(igr_backend_dx11)
target_link_libraries(igr_backend_dx11 PUBLIC igr_core d3d11 dxgi)
target_link_libraries(igr_backend_dx11 PUBLIC d3dcompiler d2d1 dwrite)
target_link_libraries(igr_backend_dx11 PRIVATE igr_shader_compiler)

add_library(igr_backend_dx12 STATIC
  backends/dx12/dx12_backend.cpp
)
add_library(igr::backend_dx12 ALIAS igr_backend_dx12)
igr_configure_target(igr_backend_dx12)
target_link_libraries(igr_backend_dx12 PUBLIC igr_core d3d12 dxgi)
target_link_libraries(igr_backend_dx12 PUBLIC d3dcompiler d3d11 d2d1 dwrite)
target_link_libraries(igr_backend_dx12 PRIVATE igr_shader_compiler)

if(IGR_BUILD_EXAMPLES)
  add_executable(igr_foundation_sample
    examples/foundation-sample/main.cpp
  )
  igr_configure_target(igr_foundation_sample)
  target_link_libraries(igr_foundation_sample PRIVATE igr_core igr_react_bridge)
  if(IGR_ENABLE_HERMES)
    igr_copy_runtime_dlls(igr_foundation_sample)
  endif()

  add_executable(igr_win32_dx11_sample WIN32
    examples/win32-dx11-sample/main.cpp
  )
  igr_configure_target(igr_win32_dx11_sample)
  target_link_libraries(igr_win32_dx11_sample PRIVATE igr_core igr_react_bridge igr_backend_dx11)
  if(IGR_ENABLE_HERMES)
    igr_copy_runtime_dlls(igr_win32_dx11_sample)
  endif()

  add_executable(igr_win32_dx12_sample WIN32
    examples/win32-dx12-sample/main.cpp
  )
  igr_configure_target(igr_win32_dx12_sample)
  target_link_libraries(igr_win32_dx12_sample PRIVATE igr_core igr_react_bridge igr_backend_dx12)
  if(IGR_ENABLE_HERMES)
    igr_copy_runtime_dlls(igr_win32_dx12_sample)
  endif()
endif()

if(IGR_BUILD_TESTS)
  enable_testing()

  add_executable(igr_core_tests
    tests/core_frame_tests.cpp
  )
  igr_configure_target(igr_core_tests)
  target_link_libraries(igr_core_tests PRIVATE igr_core)
  add_test(NAME igr_core_tests COMMAND igr_core_tests)

  add_executable(igr_react_bridge_tests
    tests/react_bridge_tests.cpp
  )
  igr_configure_target(igr_react_bridge_tests)
  target_link_libraries(igr_react_bridge_tests PRIVATE igr_core igr_react_bridge)
  if(IGR_ENABLE_HERMES)
    igr_copy_runtime_dlls(igr_react_bridge_tests)
  endif()
  add_test(NAME igr_react_bridge_tests COMMAND igr_react_bridge_tests)

  add_executable(igr_react_transport_tests
    tests/react_transport_tests.cpp
  )
  igr_configure_target(igr_react_transport_tests)
  target_link_libraries(igr_react_transport_tests PRIVATE igr_core igr_react_bridge)
  if(IGR_ENABLE_HERMES)
    igr_copy_runtime_dlls(igr_react_transport_tests)
  endif()
  add_test(NAME igr_react_transport_tests COMMAND igr_react_transport_tests)

  add_executable(igr_react_runtime_bridge_tests
    tests/react_runtime_bridge_tests.cpp
  )
  igr_configure_target(igr_react_runtime_bridge_tests)
  target_link_libraries(igr_react_runtime_bridge_tests PRIVATE igr_core igr_react_bridge)
  if(IGR_ENABLE_HERMES)
    igr_copy_runtime_dlls(igr_react_runtime_bridge_tests)
  endif()
  add_test(NAME igr_react_runtime_bridge_tests COMMAND igr_react_runtime_bridge_tests)

  add_executable(igr_interaction_tests
    tests/interaction_tests.cpp
  )
  igr_configure_target(igr_interaction_tests)
  target_link_libraries(igr_interaction_tests PRIVATE igr_core)
  add_test(NAME igr_interaction_tests COMMAND igr_interaction_tests)

  add_executable(igr_shader_compiler_tests
    tests/shader_compiler_tests.cpp
  )
  igr_configure_target(igr_shader_compiler_tests)
  target_link_libraries(igr_shader_compiler_tests PRIVATE igr_shader_compiler)
  add_test(NAME igr_shader_compiler_tests COMMAND igr_shader_compiler_tests)

  add_executable(igr_dx11_backend_tests
    tests/dx11_backend_tests.cpp
  )
  igr_configure_target(igr_dx11_backend_tests)
  target_link_libraries(igr_dx11_backend_tests PRIVATE igr_core igr_backend_dx11)
  add_test(NAME igr_dx11_backend_tests COMMAND igr_dx11_backend_tests)

  add_executable(igr_dx11_external_host_tests
    tests/dx11_external_host_tests.cpp
  )
  igr_configure_target(igr_dx11_external_host_tests)
  target_link_libraries(igr_dx11_external_host_tests PRIVATE igr_core igr_react_bridge igr_backend_dx11)
  if(IGR_ENABLE_HERMES)
    igr_copy_runtime_dlls(igr_dx11_external_host_tests)
  endif()
  add_test(NAME igr_dx11_external_host_tests COMMAND igr_dx11_external_host_tests)

  add_executable(igr_dx12_contract_tests
    tests/dx12_contract_tests.cpp
  )
  igr_configure_target(igr_dx12_contract_tests)
  target_link_libraries(igr_dx12_contract_tests PRIVATE igr_core igr_react_bridge igr_backend_dx12)
  if(IGR_ENABLE_HERMES)
    igr_copy_runtime_dlls(igr_dx12_contract_tests)
  endif()
  add_test(NAME igr_dx12_contract_tests COMMAND igr_dx12_contract_tests)

  add_executable(igr_dx12_backend_tests
    tests/dx12_backend_tests.cpp
  )
  igr_configure_target(igr_dx12_backend_tests)
  target_link_libraries(igr_dx12_backend_tests PRIVATE igr_core igr_backend_dx12)
  add_test(NAME igr_dx12_backend_tests COMMAND igr_dx12_backend_tests)
endif()
