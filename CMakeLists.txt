cmake_minimum_required(VERSION 3.15)
project(RealWind C CXX)

set(CMAKE_CXX_STANDARD 11)

# Point to your X-Plane SDK: cmake -DXPLANE_SDK=/path/to/SDK ..
set(XPLANE_SDK "${CMAKE_SOURCE_DIR}/SDK" CACHE PATH "X-Plane SDK root")

add_library(RealWind SHARED RealWindPlugin.cpp)

target_include_directories(RealWind PRIVATE
    "${XPLANE_SDK}/CHeaders/XPLM"
)

find_package(CURL REQUIRED)
target_link_libraries(RealWind PRIVATE CURL::libcurl)

target_compile_definitions(RealWind PRIVATE XPLM200 XPLM210)

if(APPLE)
    target_compile_definitions(RealWind PRIVATE APL=1 IBM=0 LIN=0)
    find_library(XPLM_LIB XPLM PATHS "${XPLANE_SDK}/Libraries/Mac" REQUIRED)
    target_link_libraries(RealWind PRIVATE ${XPLM_LIB} "-framework CoreFoundation")
elseif(WIN32)
    target_compile_definitions(RealWind PRIVATE IBM=1 APL=0 LIN=0)
    target_link_libraries(RealWind PRIVATE "${XPLANE_SDK}/Libraries/Win/XPLM_64.lib")
else()
    target_compile_definitions(RealWind PRIVATE LIN=1 IBM=0 APL=0)
    target_link_libraries(RealWind PRIVATE pthread dl m)
    # X-Plane resolves XPLM symbols at runtime on Linux — no stub needed
endif()

set_target_properties(RealWind PROPERTIES
    PREFIX ""
    SUFFIX ".xpl"
)
