cmake_minimum_required(VERSION 3.24)
project(VolturaMarkdown VERSION 1.0.0 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_library(markdown STATIC src/document.cpp third_party/md4c/md4c.c third_party/md4c/entity.c)
target_include_directories(markdown PUBLIC src third_party/md4c)
target_compile_definitions(markdown PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
add_executable(VolturaMarkdown WIN32
    src/main.cpp
    src/images.cpp
    src/shell.cpp
    src/empty-state.cpp
    assets/app.rc
)
set_target_properties(VolturaMarkdown PROPERTIES OUTPUT_NAME vmd)
target_link_libraries(VolturaMarkdown PRIVATE
    markdown
    d2d1
    dwrite
    windowscodecs
    dwmapi
    comctl32
    comdlg32
    shell32
    shlwapi
    ole32
    winhttp
    advapi32
    uiautomationcore
    uxtheme
)
target_compile_options(VolturaMarkdown PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_compile_options(markdown PRIVATE /utf-8)
target_link_options(VolturaMarkdown PRIVATE $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
target_link_options(VolturaMarkdown PRIVATE /MANIFEST:NO)
add_executable(reader_tests tests/tests.cpp third_party/md4c/md4c-html.c)
target_link_libraries(reader_tests PRIVATE markdown dwrite d2d1 windowscodecs ole32 psapi)
target_compile_options(reader_tests PRIVATE /utf-8 /EHsc)
enable_testing()
add_test(NAME reader_tests COMMAND reader_tests)
add_executable(integration_tests tests/integration.cpp src/images.cpp src/shell.cpp)
target_link_libraries(integration_tests PRIVATE
    markdown windowscodecs ole32 winhttp shlwapi shell32 advapi32 user32
)
target_compile_options(integration_tests PRIVATE /utf-8 /EHsc)
add_test(NAME integration_tests COMMAND integration_tests)
