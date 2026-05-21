set(COREBS_OBS_PLUGIN_TARGET "corebs-obs-game-capture")

set(COREBS_OBS_SOURCE_ROOT "${CMAKE_SOURCE_DIR}/vendor/obs-studio" CACHE PATH
    "Path to the OBS Studio source tree used for plugin headers")
set(COREBS_OBS_SDK_ROOT "${CMAKE_SOURCE_DIR}/vendor/obs-sdk" CACHE PATH
    "Path to the local OBS SDK/import library staging folder")
set(COREBS_OBS_CONFIG_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated-obs-config")

set(OBS_INCLUDE_DIR "" CACHE PATH "Directory containing obs-module.h")
set(OBS_LIBRARY "" CACHE FILEPATH "Import library for OBS/libobs")

if(NOT OBS_INCLUDE_DIR AND EXISTS "${COREBS_OBS_SOURCE_ROOT}/libobs/obs-module.h")
    set(OBS_INCLUDE_DIR "${COREBS_OBS_SOURCE_ROOT}/libobs" CACHE PATH
        "Directory containing obs-module.h" FORCE)
endif()

if(NOT OBS_LIBRARY AND EXISTS "${COREBS_OBS_SDK_ROOT}/lib/obs.lib")
    set(OBS_LIBRARY "${COREBS_OBS_SDK_ROOT}/lib/obs.lib" CACHE FILEPATH
        "Import library for OBS/libobs" FORCE)
endif()

if(NOT OBS_INCLUDE_DIR)
    message(FATAL_ERROR "OBS_INCLUDE_DIR must point at the OBS SDK headers (directory containing obs-module.h).")
endif()

if(NOT EXISTS "${OBS_INCLUDE_DIR}/obs-module.h")
    message(FATAL_ERROR "OBS_INCLUDE_DIR does not contain obs-module.h: ${OBS_INCLUDE_DIR}")
endif()

if(NOT OBS_LIBRARY)
    message(FATAL_ERROR "OBS_LIBRARY must point at the OBS/libobs import library.")
endif()

if(EXISTS "${COREBS_OBS_SOURCE_ROOT}/libobs/obsconfig.h.in")
    set(OBS_DATA_PATH "../../data")
    set(OBS_PLUGIN_PATH "../../obs-plugins/64bit")
    set(OBS_PLUGIN_DESTINATION "obs-plugins/64bit")
    set(OBS_RELEASE_CANDIDATE 0)
    set(OBS_BETA 0)
    file(MAKE_DIRECTORY "${COREBS_OBS_CONFIG_DIR}")
    configure_file(
        "${COREBS_OBS_SOURCE_ROOT}/libobs/obsconfig.h.in"
        "${COREBS_OBS_CONFIG_DIR}/obsconfig.h"
        @ONLY)
endif()

add_library(${COREBS_OBS_PLUGIN_TARGET} MODULE
    src/plugin-main.cpp
    src/game-capture-source.cpp
    src/game-capture-source.h
    src/ProcessFinder.cpp
    src/ProcessFinder.h
    src/Utils.cpp
    src/Utils.h
    src/WindowCaptureSource.cpp
    src/WindowCaptureSource.h
    src/WindowFinder.cpp
    src/WindowFinder.h
)

target_include_directories(${COREBS_OBS_PLUGIN_TARGET} PRIVATE
    ${OBS_INCLUDE_DIR}
    ${COREBS_OBS_CONFIG_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(MSVC)
    target_include_directories(${COREBS_OBS_PLUGIN_TARGET} PRIVATE
        "C:/Program Files (x86)/Windows Kits/10/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/cppwinrt"
    )

    target_compile_definitions(${COREBS_OBS_PLUGIN_TARGET} PRIVATE
        NOMINMAX
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
    )

    target_compile_options(${COREBS_OBS_PLUGIN_TARGET} PRIVATE
        /W4
        /EHsc
        /permissive-
        /Zc:__cplusplus
    )
endif()

target_link_libraries(${COREBS_OBS_PLUGIN_TARGET} PRIVATE
    ${OBS_LIBRARY}
    avrt
    d3d11
    dxgi
    mmdevapi
    ole32
    propsys
    runtimeobject
    shlwapi
    user32
    windowsapp
)

set_target_properties(${COREBS_OBS_PLUGIN_TARGET} PROPERTIES
    OUTPUT_NAME "${COREBS_OBS_PLUGIN_TARGET}"
    PREFIX ""
)

install(TARGETS ${COREBS_OBS_PLUGIN_TARGET}
    RUNTIME DESTINATION "${COREBS_OBS_PLUGIN_TARGET}/bin/64bit"
    LIBRARY DESTINATION "${COREBS_OBS_PLUGIN_TARGET}/bin/64bit"
    ARCHIVE DESTINATION "${COREBS_OBS_PLUGIN_TARGET}/lib"
)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/"
    DESTINATION "${COREBS_OBS_PLUGIN_TARGET}/data"
)
