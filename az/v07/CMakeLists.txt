cmake_minimum_required(VERSION 3.22)
project(AZScriptCore)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED IL2CPPTOOL_ROOT)
  message(FATAL_ERROR "IL2CPPTOOL_ROOT required")
endif()
if(NOT DEFINED LUA_ROOT)
  message(FATAL_ERROR "LUA_ROOT required")
endif()
if(NOT DEFINED IMGUI_TEMPLATE_ROOT)
  message(FATAL_ERROR "IMGUI_TEMPLATE_ROOT required")
endif()

set(IT "${IL2CPPTOOL_ROOT}/app/src/main/jni")
set(TP "${IMGUI_TEMPLATE_ROOT}/module/src/main/cpp")

file(GLOB LUA_SRC "${LUA_ROOT}/*.c")
list(FILTER LUA_SRC EXCLUDE REGEX "/(lua|luac|onelua)\\.c$")

add_library(az_scriptcore SHARED
  core.cpp
  game_ui.cpp
  ../lua_compat.cpp
  ${IT}/Il2cpp/Il2cpp.cpp
  ${IT}/Il2cpp/il2cpp-class.cpp
  ${IT}/Il2cpp/xdl/xdl.c
  ${IT}/Il2cpp/xdl/xdl_iterate.c
  ${IT}/Il2cpp/xdl/xdl_linker.c
  ${IT}/Il2cpp/xdl/xdl_lzma.c
  ${IT}/Il2cpp/xdl/xdl_util.c
  ${IT}/Includes/Utils.cpp
  ${IT}/Includes/Logger.cpp
  ${IT}/Tool/Util.cpp
  ${IT}/KittyMemory/KittyMemory.cpp
  ${IT}/KittyMemory/KittyUtils.cpp
  ${IT}/KittyMemory/MemoryPatch.cpp
  ${IT}/KittyMemory/MemoryBackup.cpp
  ${IT}/imgui/imgui.cpp
  ${IT}/imgui/imgui_draw.cpp
  ${IT}/imgui/imgui_widgets.cpp
  ${IT}/imgui/imgui_tables.cpp
  ${TP}/ImGui/backends/imgui_impl_opengl3.cpp
  ${TP}/ImGui/backends/imgui_impl_android.cpp
  ${LUA_SRC}
)
target_include_directories(az_scriptcore PRIVATE
  ${IT}
  ${IT}/Il2cpp
  ${IT}/Il2cpp/xdl
  ${IT}/imgui
  ${TP}/ImGui
  ${TP}/ImGui/backends
  ${LUA_ROOT}
  ${TP}/Includes
)
add_library(dobby STATIC IMPORTED)
set_target_properties(dobby PROPERTIES IMPORTED_LOCATION "${TP}/libs/${ANDROID_ABI}/libdobby.a")
find_library(log-lib log)
find_library(dl-lib dl)
find_library(android-lib android)
find_library(egl-lib EGL)
find_library(gles3-lib GLESv3)
target_compile_options(az_scriptcore PRIVATE -O2 -fPIC -fvisibility=hidden -fexceptions)
target_link_options(az_scriptcore PRIVATE -Wl,--gc-sections -Wl,--strip-all)
target_link_libraries(az_scriptcore dobby ${log-lib} ${dl-lib} ${android-lib} ${egl-lib} ${gles3-lib})
set_target_properties(az_scriptcore PROPERTIES OUTPUT_NAME "AZCore07")


add_library(az_hostui SHARED
  ui_host.cpp
  game_ui.cpp
  ${TP}/ImGui/imgui.cpp
  ${TP}/ImGui/imgui_draw.cpp
  ${TP}/ImGui/imgui_widgets.cpp
  ${TP}/ImGui/imgui_tables.cpp
  ${TP}/ImGui/backends/imgui_impl_opengl3.cpp
  ${TP}/ImGui/backends/imgui_impl_android.cpp
)
target_include_directories(az_hostui PRIVATE
  ${TP}/ImGui
  ${TP}/ImGui/backends
  ${TP}/Includes
)
target_compile_options(az_hostui PRIVATE -O2 -fPIC -fvisibility=hidden -fexceptions)
target_link_options(az_hostui PRIVATE -Wl,--gc-sections -Wl,--strip-all)
target_link_libraries(az_hostui dobby ${log-lib} ${dl-lib} ${android-lib} ${egl-lib} ${gles3-lib})
set_target_properties(az_hostui PROPERTIES OUTPUT_NAME "AZHostUI07")
