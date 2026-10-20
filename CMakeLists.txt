cmake_minimum_required(VERSION 3.24)
project(VolturaMarkdown VERSION 1.3.1 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# HTML tree construction uses the WHATWG parser; only its dependency modules are built.
set(HTML_PARSER_SOURCES)
foreach(module IN ITEMS core dom html ns tag)
    file(GLOB_RECURSE module_sources CONFIGURE_DEPENDS third_party/lexbor/source/lexbor/${module}/*.c)
    list(APPEND HTML_PARSER_SOURCES ${module_sources})
endforeach()
file(GLOB_RECURSE html_port_sources CONFIGURE_DEPENDS third_party/lexbor/source/lexbor/ports/windows_nt/*.c)
add_library(html_parser STATIC ${HTML_PARSER_SOURCES} ${html_port_sources})
target_include_directories(html_parser PUBLIC third_party/lexbor/source)
target_compile_definitions(html_parser PUBLIC LEXBOR_STATIC)
set(MARKDOWN_SOURCES src/document.cpp third_party/md4c/md4c.c third_party/md4c/entity.c)
add_library(markdown STATIC ${MARKDOWN_SOURCES})
add_library(markdown_test STATIC ${MARKDOWN_SOURCES})
target_compile_definitions(markdown_test PUBLIC VMD_TESTING)
foreach(parser IN ITEMS markdown markdown_test)
    target_link_libraries(${parser} PUBLIC html_parser)
    target_include_directories(${parser} PUBLIC src third_party/md4c)
    target_compile_definitions(${parser} PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(${parser} PRIVATE /utf-8)
endforeach()

set(VIEWER_SOURCES
    src/main.cpp
    src/edit.cpp
    src/editor.cpp
    src/editor-tools.cpp
    src/images.cpp
    src/shell.cpp
    src/updates.cpp
    src/restart.cpp
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
        uuid
        winhttp
        advapi32
        uiautomationcore
        uxtheme
        bcrypt
        crypt32
        wintrust
        version
        delayimp
        rstrtmgr
    )
    target_compile_options(${viewer} PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_link_options(${viewer} PRIVATE $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
    target_link_options(${viewer} PRIVATE /MANIFEST:NO)
    # Optional updater libraries must not load before the reader's first frame.
    target_link_options(${viewer} PRIVATE /DELAYLOAD:wintrust.dll /DELAYLOAD:crypt32.dll
        /DELAYLOAD:bcrypt.dll /DELAYLOAD:version.dll /DELAYLOAD:Rstrtmgr.dll)
endforeach()
add_executable(reader_tests tests/tests.cpp third_party/md4c/md4c-html.c)
target_link_libraries(reader_tests PRIVATE markdown_test dwrite d2d1 windowscodecs ole32 psapi)
target_compile_options(reader_tests PRIVATE /utf-8 /EHsc)
enable_testing()
add_executable(code_syntax_tests tests/code-syntax.cpp)
target_compile_options(code_syntax_tests PRIVATE /utf-8 /EHsc)
add_test(NAME code_syntax_tests COMMAND code_syntax_tests)
add_test(NAME reader_tests COMMAND reader_tests)
add_executable(edit_tests tests/edit.cpp src/edit.cpp src/editor.cpp src/editor-tools.cpp src/images.cpp)
target_link_libraries(edit_tests PRIVATE markdown_test dwrite shlwapi comctl32 comdlg32 dwmapi uxtheme user32 gdi32 ole32 windowscodecs shell32 winhttp uuid)
target_compile_options(edit_tests PRIVATE /W4 /utf-8 /EHsc)
add_test(NAME edit_tests COMMAND edit_tests)
set_tests_properties(edit_tests PROPERTIES TIMEOUT 30)
add_executable(integration_tests tests/integration.cpp src/images.cpp src/shell.cpp)
target_link_libraries(integration_tests PRIVATE
    markdown_test dwrite windowscodecs ole32 winhttp shlwapi shell32 advapi32 user32
)
target_compile_options(integration_tests PRIVATE /utf-8 /EHsc)
add_test(NAME integration_tests COMMAND integration_tests)
add_executable(svg_tests tests/svg.cpp)
target_compile_options(svg_tests PRIVATE /utf-8 /EHsc)
add_test(NAME svg_tests COMMAND svg_tests)
add_executable(update_tests tests/updates.cpp assets/app.rc)
target_compile_definitions(update_tests PRIVATE VMD_TESTING UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(update_tests PRIVATE /W4 /utf-8 /EHsc)
target_link_options(update_tests PRIVATE /MANIFEST:NO)
target_link_libraries(update_tests PRIVATE bcrypt crypt32 wintrust winhttp version shell32 ole32 advapi32 user32)
add_executable(update_fixture WIN32 tests/update-fixture.cpp tests/update-fixture.rc)
add_test(NAME update_tests COMMAND update_tests)
add_executable(restart_tests tests/restart.cpp)
target_include_directories(restart_tests PRIVATE src)
target_compile_definitions(restart_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(restart_tests PRIVATE /W4 /utf-8 /EHsc)
target_link_libraries(restart_tests PRIVATE bcrypt rstrtmgr shell32 ole32 user32)
add_test(NAME restart_tests COMMAND restart_tests)
set_tests_properties(restart_tests PROPERTIES TIMEOUT 30)
set_tests_properties(update_tests PROPERTIES TIMEOUT 30)
set_tests_properties(reader_tests integration_tests svg_tests PROPERTIES TIMEOUT 30)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME desktop_runner
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/test-desktop-runner.py)
    set_tests_properties(desktop_runner PROPERTIES TIMEOUT 45)
    add_test(NAME production_package
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/verify-package.py $<TARGET_FILE:VolturaMarkdown>)
endif()
