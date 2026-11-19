cmake_minimum_required(VERSION 3.15)
project(RealWind C CXX)
set(CMAKE_CXX_STANDARD 11)

add_library(RealWind SHARED RealWindPlugin.cpp)

# Bundled XPLM headers — no SDK download needed
target_include_directories(RealWind PRIVATE "${CMAKE_SOURCE_DIR}/SDK")

# System curl — pre-installed on macOS and Linux, no brew required
find_package(CURL REQUIRED)
target_link_libraries(RealWind PRIVATE CURL::libcurl)

target_compile_definitions(RealWind PRIVATE XPLM200 XPLM210)

if(APPLE)
    target_compile_definitions(RealWind PRIVATE APL=1 IBM=0 LIN=0)
    # Resolve XPLM symbols at runtime — no framework stub needed
    target_link_options(RealWind PRIVATE -undefined dynamic_lookup)

elseif(WIN32)
    target_compile_definitions(RealWind PRIVATE IBM=1 APL=0 LIN=0)
    # Windows needs the XPLM stub — download SDK and pass -DXPLANE_SDK=...
    set(XPLANE_SDK "" CACHE PATH "X-Plane SDK root (Windows only)")
    if(XPLANE_SDK STREQUAL "")
        message(FATAL_ERROR "Set -DXPLANE_SDK=/path/to/SDK on Windows")
    endif()
    target_link_libraries(RealWind PRIVATE "${XPLANE_SDK}/Libraries/Win/XPLM_64.lib")

else()
    target_compile_definitions(RealWind PRIVATE LIN=1 IBM=0 APL=0)
    target_link_libraries(RealWind PRIVATE pthread dl m)
endif()

set_target_properties(RealWind PROPERTIES PREFIX "" SUFFIX ".xpl")
