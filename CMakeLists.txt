cmake_minimum_required(VERSION 3.20)
project(WinTopPreview LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 只构建 Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(WinTopPreview WIN32
    src/Main.cpp
    src/StickyNote.cpp
    src/WindowEnumerator.cpp
    src/PickerDialog.cpp
    src/ControlPanel.cpp
    src/Dock.cpp
    app.rc
)

target_include_directories(WinTopPreview PRIVATE src)

target_compile_definitions(WinTopPreview PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    STRICT
)

# MSVC 特性
if(MSVC)
    target_compile_options(WinTopPreview PRIVATE
        /W4 /utf-8 /EHsc
    )
    target_link_options(WinTopPreview PRIVATE
        /SUBSYSTEM:WINDOWS
    )
endif()

# 系统库
target_link_libraries(WinTopPreview PRIVATE
    dwmapi
    shell32
    gdi32
    user32
)

# 单文件输出
set_target_properties(WinTopPreview PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
