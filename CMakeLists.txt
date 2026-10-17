cmake_minimum_required(VERSION 3.24)
project(VolturaMarkdown VERSION 1.1.6 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(MARKDOWN_SOURCES src/document.cpp third_party/md4c/md4c.c third_party/md4c/entity.c)
add_library(markdown STATIC ${MARKDOWN_SOURCES})
add_library(markdown_test STATIC ${MARKDOWN_SOURCES})
target_compile_definitions(markdown_test PUBLIC VMD_TESTING)
foreach(parser IN ITEMS markdown markdown_test)
    target_include_directories(${parser} PUBLIC src third_party/md4c)
    target_compile_definitions(${parser} PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(${parser} PRIVATE /utf-8)
endforeach()

set(VIEWER_SOURCES
    src/main.cpp
    src/images.cpp
    src/shell.cpp
    src/empty-state.cpp
    assets/app.rc
)
add_executable(VolturaMarkdown WIN32 ${VIEWER_SOURCES})
set_target_properties(VolturaMarkdown PROPERTIES OUTPUT_NAME vmd)
add_executable(VolturaMarkdownTest WIN32 ${VIEWER_SOURCES})
set_target_properties(VolturaMarkdownTest PROPERTIES OUTPUT_NAME vmd-test)
target_link_libraries(VolturaMarkdown PRIVATE markdown)
target_link_libraries(VolturaMarkdownTest PRIVATE markdown_test)
foreach(viewer IN ITEMS VolturaMarkdown VolturaMarkdownTest)
    target_link_libraries(${viewer} PRIVATE
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
    target_compile_options(${viewer} PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_link_options(${viewer} PRIVATE $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
    target_link_options(${viewer} PRIVATE /MANIFEST:NO)
endforeach()
add_executable(reader_tests tests/tests.cpp third_party/md4c/md4c-html.c)
target_link_libraries(reader_tests PRIVATE markdown_test dwrite d2d1 windowscodecs ole32 psapi)
target_compile_options(reader_tests PRIVATE /utf-8 /EHsc)
enable_testing()
add_test(NAME reader_tests COMMAND reader_tests)
add_executable(integration_tests tests/integration.cpp src/images.cpp src/shell.cpp)
target_link_libraries(integration_tests PRIVATE
    markdown_test windowscodecs ole32 winhttp shlwapi shell32 advapi32 user32
)
target_compile_options(integration_tests PRIVATE /utf-8 /EHsc)
add_test(NAME integration_tests COMMAND integration_tests)
add_executable(svg_tests tests/svg.cpp)
target_compile_options(svg_tests PRIVATE /utf-8 /EHsc)
add_test(NAME svg_tests COMMAND svg_tests)
set_tests_properties(reader_tests integration_tests svg_tests PROPERTIES TIMEOUT 30)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME production_package
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/verify-package.py $<TARGET_FILE:VolturaMarkdown>)
endif()
