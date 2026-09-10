cmake_minimum_required(VERSION 3.20)

project(YourFriend VERSION 1.1.0 LANGUAGES CXX RC)

add_executable(DesktopFly WIN32
    src/main.cpp
    src/fly.rc
    src/app.manifest
    src/resource.h
)

target_compile_features(DesktopFly PRIVATE cxx_std_17)
target_compile_definitions(DesktopFly PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_link_libraries(DesktopFly PRIVATE gdiplus ole32 uuid gdi32 user32)

set_target_properties(DesktopFly PROPERTIES
    OUTPUT_NAME "DesktopFly_5min"
)

if(MINGW)
    target_link_options(DesktopFly PRIVATE -static -static-libgcc -static-libstdc++)
endif()
