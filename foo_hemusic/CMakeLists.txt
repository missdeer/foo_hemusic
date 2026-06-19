cmake_minimum_required(VERSION 3.21)

# Force the dynamic CRT (/MD) on every config so all targets + foobar2000's
# shared.dll agree on the runtime. Must be set before project().
if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" CACHE STRING "" FORCE)

project(foo_hemusic VERSION 0.0.1 LANGUAGES CXX)

if(NOT MSVC)
  message(FATAL_ERROR "foo_hemusic targets foobar2000 on Windows / MSVC only.")
endif()

# --- foobar2000 SDK location + auto-fetch ------------------------------------
get_filename_component(_repo_root "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(FB2K_SDK_ROOT "${_repo_root}/foobar2000-SDK-2025-03-07"
    CACHE PATH "foobar2000 SDK root (source tree)")
set(FB2K_SDK_URL "https://www.foobar2000.org/downloads/SDK-2025-03-07.7z"
    CACHE STRING "Official foobar2000 SDK archive (used only when SDK is absent)")

# Use the existing SDK if present; otherwise download + extract it once.
if(NOT EXISTS "${FB2K_SDK_ROOT}/foobar2000/SDK/foobar2000.h")
  set(_sdk_archive "${CMAKE_CURRENT_BINARY_DIR}/foobar2000-sdk.7z")
  message(STATUS "foobar2000 SDK not found at ${FB2K_SDK_ROOT}")
  message(STATUS "Downloading SDK: ${FB2K_SDK_URL}")
  file(DOWNLOAD "${FB2K_SDK_URL}" "${_sdk_archive}"
       STATUS _dl_status TLS_VERIFY ON SHOW_PROGRESS)
  list(GET _dl_status 0 _dl_code)
  if(NOT _dl_code EQUAL 0)
    list(GET _dl_status 1 _dl_msg)
    file(REMOVE "${_sdk_archive}")
    message(FATAL_ERROR
      "SDK download failed (${_dl_code}: ${_dl_msg}).\n"
      "Download ${FB2K_SDK_URL} manually and extract it so that\n"
      "  ${FB2K_SDK_ROOT}/foobar2000/SDK/foobar2000.h\n"
      "exists, or pass -DFB2K_SDK_ROOT=<path-to-existing-SDK>.")
  endif()
  file(MAKE_DIRECTORY "${FB2K_SDK_ROOT}")
  # CMake's bundled libarchive handles .7z.
  file(ARCHIVE_EXTRACT INPUT "${_sdk_archive}" DESTINATION "${FB2K_SDK_ROOT}")
  if(NOT EXISTS "${FB2K_SDK_ROOT}/foobar2000/SDK/foobar2000.h")
    message(FATAL_ERROR
      "SDK extracted to ${FB2K_SDK_ROOT} but the expected layout "
      "(foobar2000/SDK/foobar2000.h) was not found.")
  endif()
  message(STATUS "foobar2000 SDK ready at ${FB2K_SDK_ROOT}")
endif()

# --- Platform bits -----------------------------------------------------------
# shared-*.lib is the import lib for foobar2000's shared.dll; it ships with the
# SDK and provides pfc::crashHook / winFormatSystemErrorMessageHook at runtime
# (which is why pfc-fb2k-hooks.cpp is excluded from the pfc sources below).
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(_fb2k_shared "${FB2K_SDK_ROOT}/foobar2000/shared/shared-x64.lib")
  set(_export_sym  "/INCLUDE:foobar2000_get_interface")
else()
  set(_fb2k_shared "${FB2K_SDK_ROOT}/foobar2000/shared/shared-Win32.lib")
  set(_export_sym  "/INCLUDE:_foobar2000_get_interface")
endif()
if(NOT EXISTS "${_fb2k_shared}")
  message(FATAL_ERROR "Missing foobar2000 shared import lib: ${_fb2k_shared}")
endif()

# --- foobar2000 SDK compiled from source -------------------------------------
# The downloadable SDK ships source only (no prebuilt libs), so we build the
# pieces a minimal component needs: pfc + the SDK core + the DLL entry stub.
# helpers / libPPUI (and WTL) are added later when the UI phase needs them.
file(GLOB _pfc_src CONFIGURE_DEPENDS "${FB2K_SDK_ROOT}/pfc/*.cpp")
list(FILTER _pfc_src EXCLUDE REGEX "pfc-fb2k-hooks\\.cpp$")
file(GLOB _sdk_src CONFIGURE_DEPENDS "${FB2K_SDK_ROOT}/foobar2000/SDK/*.cpp")
set(_client_src
    "${FB2K_SDK_ROOT}/foobar2000/foobar2000_component_client/component_client.cpp")

add_library(fb2k_sdk STATIC ${_pfc_src} ${_sdk_src} ${_client_src})
target_include_directories(fb2k_sdk PUBLIC
  "${FB2K_SDK_ROOT}/foobar2000"   # <SDK/...>, <helpers/...>, <shared/...>
  "${FB2K_SDK_ROOT}")             # <pfc/...>, <libPPUI/...>
target_compile_features(fb2k_sdk PUBLIC cxx_std_17)   # SDK's own standard
target_compile_definitions(fb2k_sdk PUBLIC UNICODE _UNICODE)

# --- The component DLL -------------------------------------------------------
add_library(foo_hemusic SHARED src/component.cpp)
target_compile_features(foo_hemusic PRIVATE cxx_std_20)

target_link_libraries(foo_hemusic PRIVATE
  fb2k_sdk
  "${_fb2k_shared}"
  shlwapi comctl32 uxtheme gdi32 user32 ole32 oleaut32 shell32 advapi32 comdlg32)

# foobar2000_get_interface lives in component_client (inside fb2k_sdk) and is
# referenced by nothing in our code, so force-keep its export.
target_link_options(foo_hemusic PRIVATE "${_export_sym}")

# foobar2000 loads DLLs named foo_*.dll (no "lib" prefix).
set_target_properties(foo_hemusic PROPERTIES
  PREFIX ""
  OUTPUT_NAME "foo_hemusic"
  SUFFIX ".dll")
