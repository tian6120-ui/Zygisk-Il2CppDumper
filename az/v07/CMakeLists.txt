cmake_minimum_required(VERSION 3.22)
project(AZHF7SingleCore)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED IL2CPPTOOL_ROOT)
  message(FATAL_ERROR "IL2CPPTOOL_ROOT required")
endif()
if(NOT DEFINED IMGUI_TEMPLATE_ROOT)
  message(FATAL_ERROR "IMGUI_TEMPLATE_ROOT required")
endif()

set(IT "${IL2CPPTOOL_ROOT}/app/src/main/jni")
set(TP "${IMGUI_TEMPLATE_ROOT}/module/src/main/cpp")
set(LUA_ROOT "${CMAKE_CURRENT_LIST_DIR}/lua")

file(GLOB LUA_SRC "${LUA_ROOT}/*.c")
list(FILTER LUA_SRC EXCLUDE REGEX "/(lua|luac|onelua|ltests)\\.c$")

set(AZ_SOURCES
  core.cpp
  game_ui.cpp
  tool_runtime.cpp
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

  ${IT}/Tool/Keyboard.cpp
  ${IT}/Tool/Tool.cpp
  ${IT}/Tool/Util.cpp
  ${IT}/Tool/Patcher.cpp
  ${IT}/Tool/PopUpSelector.cpp
  ${IT}/Tool/ClassesTab.cpp
  ${IT}/Tool/ChangeLog.cpp
  ${IT}/Tool/ObjectDrawManager.cpp

  ${IT}/imgui/imgui.cpp
  ${IT}/imgui/imgui_draw.cpp
  ${IT}/imgui/imgui_widgets.cpp
  ${IT}/imgui/imgui_tables.cpp
  ${IT}/imgui/backends/imgui_impl_opengl3.cpp

  ${LUA_SRC}
)

add_library(az_hf7_core SHARED ${AZ_SOURCES})

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

target_compile_definitions(az_hf7_core PRIVATE
  AZ_EMBEDDED=1
  LIB_INPUT=1
  ASMJIT_STATIC=1
)

target_compile_options(az_hf7_core PRIVATE
  -O2 -fPIC -fvisibility=hidden -fexceptions
  -ffunction-sections -fdata-sections
  -Wno-error -Wno-unused-command-line-argument
)
target_link_options(az_hf7_core PRIVATE
  -Wl,--gc-sections
  -Wl,--strip-all
)

add_library(dobby STATIC IMPORTED)
set_target_properties(dobby PROPERTIES
  IMPORTED_LOCATION "${TP}/libs/${ANDROID_ABI}/libdobby.a"
)

if(ANDROID_ABI STREQUAL "arm64-v8a")
  add_library(asmjit STATIC IMPORTED)
  set_target_properties(asmjit PROPERTIES
    IMPORTED_LOCATION "${IT}/asmjit/lib/libasmjit.a"
  )
elseif(ANDROID_ABI STREQUAL "x86_64")
  file(GLOB ASMJIT_CORE "${IT}/asmjit/include/core/*.cpp")
  file(GLOB ASMJIT_X86 "${IT}/asmjit/include/x86/*.cpp")
  add_library(asmjit STATIC ${ASMJIT_CORE} ${ASMJIT_X86})
  target_include_directories(asmjit PUBLIC "${IT}/asmjit/include")
  target_compile_definitions(asmjit PUBLIC ASMJIT_STATIC=1)
  target_compile_options(asmjit PRIVATE -O2 -fPIC)
else()
  message(FATAL_ERROR "S5 currently targets arm64-v8a and x86_64 only")
endif()

find_library(LOG_LIB log)
find_library(DL_LIB dl)
find_library(ANDROID_LIB android)
find_library(EGL_LIB EGL)
find_library(GLES3_LIB GLESv3)
find_library(Z_LIB z)

target_link_libraries(az_hf7_core
  dobby asmjit
  ${LOG_LIB} ${DL_LIB} ${ANDROID_LIB}
  ${EGL_LIB} ${GLES3_LIB} ${Z_LIB}
)

set_target_properties(az_hf7_core PROPERTIES OUTPUT_NAME "AZCoreHF7")
