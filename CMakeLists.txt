cmake_minimum_required(VERSION 3.15)
project(EZAuto VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(EZAuto
    src/main.cpp
    src/FocusMonitor.cpp
    src/ImeSwitcher.cpp
    src/ConfigManager.cpp
)

target_include_directories(EZAuto PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(EZAuto PRIVATE
    ole32 oleaut32 uuid oleacc imm32 psapi version
)

target_compile_definitions(EZAuto PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
)

# Copy config.json to output directory after build
add_custom_command(TARGET EZAuto POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/config.json"
        "$<TARGET_FILE_DIR:EZAuto>/config.json"
    COMMENT "Copying config.json to output directory"
)
