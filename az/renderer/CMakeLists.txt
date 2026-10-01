cmake_minimum_required(VERSION 3.22)
project(AZRenderer)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT DEFINED THIRDPARTY_ROOT)
  message(FATAL_ERROR "THIRDPARTY_ROOT is required")
endif()
set(TP "${THIRDPARTY_ROOT}/module/src/main/cpp")
add_library(az_renderer SHARED
  renderer.cpp
  "${TP}/ImGui/imgui.cpp"
  "${TP}/ImGui/imgui_draw.cpp"
  "${TP}/ImGui/imgui_widgets.cpp"
  "${TP}/ImGui/imgui_tables.cpp"
  "${TP}/ImGui/backends/imgui_impl_opengl3.cpp"
  "${TP}/ImGui/backends/imgui_impl_android.cpp"
)
target_include_directories(az_renderer PRIVATE
  "${TP}/ImGui"
  "${TP}/ImGui/backends"
  "${TP}/Includes/Dobby"
)
add_library(dobby STATIC IMPORTED)
set_target_properties(dobby PROPERTIES IMPORTED_LOCATION "${TP}/libs/${ANDROID_ABI}/libdobby.a")
find_library(log-lib log)
find_library(android-lib android)
find_library(egl-lib EGL)
find_library(gles3-lib GLESv3)
find_library(dl-lib dl)
target_compile_options(az_renderer PRIVATE -O2 -fvisibility=hidden -fPIC)
target_link_libraries(az_renderer dobby ${log-lib} ${android-lib} ${egl-lib} ${gles3-lib} ${dl-lib})
