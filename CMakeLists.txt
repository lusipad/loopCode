cmake_minimum_required(VERSION 3.20)
project(loopcode LANGUAGES CXX)

add_executable(loopcode
    src/main.cpp
)

target_compile_features(loopcode PRIVATE cxx_std_17)
target_link_libraries(loopcode PRIVATE User32 Ole32 OleAut32 Uiautomationcore)

if(MSVC)
    target_compile_options(loopcode PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(loopcode PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_custom_command(TARGET loopcode POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:loopcode>
        $<TARGET_FILE_DIR:loopcode>/loopguard.exe
)
