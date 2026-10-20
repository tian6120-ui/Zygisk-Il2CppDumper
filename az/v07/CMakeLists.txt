cmake_minimum_required(VERSION 3.22)
project(AZHF7SingleCore)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

foreach(v IL2CPPTOOL_ROOT IMGUI_TEMPLATE_ROOT LUA_ROOT)
  if(NOT DEFINED ${v})
    message(FATAL_ERROR "${v} required")
  endif()
endforeach()

set(IT "${IL2CPPTOOL_ROOT}/app/src/main/jni")
set(TP "${IMGUI_TEMPLATE_ROOT}/module/src/main/cpp")

file(GLOB LUA_SRC "${LUA_ROOT}/*.c")
list(FILTER LUA_SRC EXCLUDE REGEX "/(lua|luac|onelua)\\.c$")

file(GLOB ASMJIT_CORE "${IT}/asmjit/include/core/*.cpp")
if(ANDROID_ABI STREQUAL "arm64-v8a")
  file(GLOB ASMJIT_ARCH "${IT}/asmjit/include/arm/*.cpp")
else()
  file(GLOB ASMJIT_ARCH "${IT}/asmjit/include/arm/*.cpp")
endif()

add_library(az_hf7_core SHARED
  core.cpp
  game_ui.cpp
  hf7_main.cpp
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

  ${IT}/KittyMemory/KittyMemory.cpp
  ${IT}/KittyMemory/KittyUtils.cpp
  ${IT}/KittyMemory/MemoryPatch.cpp
  ${IT}/KittyMemory/MemoryBackup.cpp

  ${IT}/Menu/ImGui.cpp

  ${IT}/Tool/Keyboard.cpp
  ${IT}/Tool/Tool.cpp
  ${IT}/Tool/Util.cpp
  ${IT}/Tool/Patcher.cpp
  ${IT}/Tool/PopUpSelector.cpp
  ${IT}/Tool/SelfCheck.cpp
  ${IT}/Tool/ClassesTab.cpp
  ${IT}/Tool/ChangeLog.cpp
  ${IT}/Tool/Unity.cpp
  ${IT}/Tool/ObjectDrawManager.cpp

  ${IT}/imgui/imgui.cpp
  ${IT}/imgui/imgui_draw.cpp
  ${IT}/imgui/imgui_widgets.cpp
  ${IT}/imgui/imgui_tables.cpp
  ${IT}/imgui/backends/imgui_impl_opengl3.cpp
  ${IT}/imgui/backends/imgui_impl_android.cpp

  ${ASMJIT_CORE}
  ${ASMJIT_ARCH}
  ${LUA_SRC}
)

target_include_directories(az_hf7_core PRIVATE
  ${IT}
  ${IT}/Il2cpp
  ${IT}/Il2cpp/xdl
  ${IT}/imgui
  ${IT}/imgui/backends
  ${IT}/asmjit/include
  ${LUA_ROOT}
  ${TP}/Includes
)

target_compile_definitions(az_hf7_core PRIVATE AZ_EMBEDDED=1 LIB_INPUT=1 ASMJIT_STATIC=1)
target_compile_options(az_hf7_core PRIVATE -O2 -fPIC -fvisibility=hidden -fexceptions -Wno-error -Wno-unused-command-line-argument)
target_link_options(az_hf7_core PRIVATE -Wl,--gc-sections -Wl,--strip-all)

add_library(dobby STATIC IMPORTED)
set_target_properties(dobby PROPERTIES IMPORTED_LOCATION "${TP}/libs/${ANDROID_ABI}/libdobby.a")

find_library(LOG_LIB log)
find_library(DL_LIB dl)
find_library(ANDROID_LIB android)
find_library(EGL_LIB EGL)
find_library(GLES3_LIB GLESv3)
find_library(Z_LIB z)

target_link_libraries(az_hf7_core
  dobby
  ${LOG_LIB} ${DL_LIB} ${ANDROID_LIB} ${EGL_LIB} ${GLES3_LIB} ${Z_LIB}
)

set_target_properties(az_hf7_core PROPERTIES OUTPUT_NAME "AZCoreHF7")
