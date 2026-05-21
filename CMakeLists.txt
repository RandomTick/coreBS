cmake_minimum_required(VERSION 3.24)

project(coreBS VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(COREBS_BUILD_OBS_PLUGIN "Build the experimental OBS source plugin" OFF)


add_executable(coreBS
    src/main.cpp
    src/AudioCapture.cpp
    src/ProcessFinder.cpp
    src/Recorder.cpp
    src/SignalHandler.cpp
    src/Utils.cpp
    src/VideoCapture.cpp
    src/WindowCaptureSource.cpp
    src/WindowFinder.cpp
)

target_include_directories(coreBS PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(MSVC)
    if(NOT CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
        set(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION "10.0.22621.0")
    endif()

    target_include_directories(coreBS PRIVATE
        "C:/Program Files (x86)/Windows Kits/10/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/cppwinrt"
    )

    target_compile_definitions(coreBS PRIVATE
        NOMINMAX
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
    )

    target_compile_options(coreBS PRIVATE
        /W4
        /EHsc
        /permissive-
        /Zc:__cplusplus
    )
endif()

target_link_libraries(coreBS PRIVATE
    avrt
    d3d11
    dxgi
    mf
    mfplat
    mfreadwrite
    mfuuid
    mmdevapi
    ole32
    propsys
    runtimeobject
    shlwapi
    user32
    windowsapp
)

set_target_properties(coreBS PROPERTIES
    OUTPUT_NAME "coreBS"
)


if(COREBS_BUILD_OBS_PLUGIN)
    add_subdirectory(obs-plugin)
endif()
