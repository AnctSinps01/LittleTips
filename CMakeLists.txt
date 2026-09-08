cmake_minimum_required(VERSION 3.20)
project(LittleTips VERSION 1.0.0 LANGUAGES CXX)

add_executable(LittleTips WIN32
    src/main.cpp
    src/resources.rc
)

target_compile_features(LittleTips PRIVATE cxx_std_20)
target_compile_definitions(LittleTips PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(LittleTips PRIVATE comctl32 shell32)

if(MSVC)
    target_compile_options(LittleTips PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_link_options(LittleTips PRIVATE /MANIFEST:EMBED)
else()
    target_compile_options(LittleTips PRIVATE -Wall -Wextra -Wpedantic -Os)
    # Use the system runtime DLLs; this keeps the app's resident image small.
    target_link_options(LittleTips PRIVATE -municode -s -static-libgcc -static-libstdc++)
endif()

install(TARGETS LittleTips RUNTIME DESTINATION .)
