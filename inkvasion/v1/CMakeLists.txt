cmake_minimum_required(VERSION 3.18.1)
project(inkvasion_auto)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)

if(NOT DEFINED REPO_ROOT)
    message(FATAL_ERROR "REPO_ROOT is required")
endif()

set(COMMON_CPP "${REPO_ROOT}/module/src/main/cpp")
set(XDL_DIR "${COMMON_CPP}/xdl")

include_directories(
    "${COMMON_CPP}"
    "${XDL_DIR}/include"
)

file(GLOB XDL_SRC "${XDL_DIR}/*.c")

add_library(inkvasion_auto SHARED
    main.cpp
    ${XDL_SRC}
)

target_compile_options(inkvasion_auto PRIVATE
    -O2
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -fdata-sections
    -ffunction-sections
    -ffixed-x18
)

target_link_options(inkvasion_auto PRIVATE
    -Wl,--gc-sections
    -Wl,--hash-style=both
)

target_link_libraries(inkvasion_auto log dl)
