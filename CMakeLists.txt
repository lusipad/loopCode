cmake_minimum_required(VERSION 3.20)
project(loopguard LANGUAGES CXX)

add_executable(loopguard
    src/main.cpp
)

target_compile_features(loopguard PRIVATE cxx_std_17)
target_compile_definitions(loopguard PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(loopguard PRIVATE User32 Ole32 OleAut32 Uiautomationcore)

if(MSVC)
    target_compile_options(loopguard PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(loopguard PRIVATE -Wall -Wextra -Wpedantic)
endif()
