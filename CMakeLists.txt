cmake_minimum_required(VERSION 3.22)
project(MyriadLife C)

if(ANDROID AND NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "MyriadLife supports Android arm64-v8a only")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(MyriadLife SHARED
    mod-api/tefkernel/tef_api_imp.c
    mod.c
)

target_include_directories(MyriadLife PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/mod-api
)

if(WIN32)
    set_target_properties(MyriadLife PROPERTIES PREFIX "" SUFFIX ".dll")
elseif(APPLE)
    set_target_properties(MyriadLife PROPERTIES PREFIX "lib" SUFFIX ".dylib")
else()
    set_target_properties(MyriadLife PROPERTIES PREFIX "lib" SUFFIX ".so")
endif()

if(ANDROID)
    target_compile_definitions(MyriadLife PRIVATE __ANDROID__)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release" AND CMAKE_STRIP)
    add_custom_command(TARGET MyriadLife POST_BUILD
        COMMAND ${CMAKE_STRIP} --strip-unneeded $<TARGET_FILE:MyriadLife>)
endif()
