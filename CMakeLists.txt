cmake_minimum_required(VERSION 3.25)
project(TacAudio VERSION 0.6.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static C/C++ runtime: the installed app runs without the Visual C++ Redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(TAC_BUILD_TESTS "Build TacAudio unit tests" ON)
option(TAC_WITH_SOFA "SOFA HRTF import via libmysofa (downloads zlib + libmysofa)" ON)
option(TAC_WITH_GUI "Windows GUI (downloads Dear ImGui)" ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /EHsc)
    add_compile_definitions(_USE_MATH_DEFINES NOMINMAX)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------- core library
add_library(tac_core STATIC
    src/core/fft.cpp
    src/core/convolver.cpp
    src/core/wav.cpp
    src/core/hrtf.cpp
    src/core/layout.cpp
    src/core/panner.cpp
    src/core/binaural_renderer.cpp
    src/core/scene.cpp
    src/core/limiter.cpp
    src/core/room.cpp
    src/core/dynamics.cpp
    src/engine/engine.cpp
)
target_include_directories(tac_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ---------------------------------------------------------------- SOFA import (optional)
if(TAC_WITH_SOFA)
    enable_language(C)
    include(FetchContent)

    set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(ZLIB_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(zlib
        GIT_REPOSITORY https://github.com/madler/zlib.git
        GIT_TAG v1.3.2
        GIT_SHALLOW TRUE)
    # libmysofa's own CMake wants NuGet zlib on MSVC, so only download it
    # (SOURCE_SUBDIR without a CMakeLists.txt) and compile its sources ourselves.
    FetchContent_Declare(libmysofa
        GIT_REPOSITORY https://github.com/hoene/libmysofa.git
        GIT_TAG v1.3.5
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR _tacaudio_no_cmake)
    FetchContent_MakeAvailable(zlib libmysofa)

    set(MYSOFA_SRC ${libmysofa_SOURCE_DIR}/src)
    set(MYSOFA_GEN ${CMAKE_CURRENT_BINARY_DIR}/mysofa_generated)
    set(CPACK_PACKAGE_VERSION_MAJOR 1)
    set(CPACK_PACKAGE_VERSION_MINOR 3)
    set(CPACK_PACKAGE_VERSION_PATCH 5)
    configure_file(${MYSOFA_SRC}/config.h.in ${MYSOFA_GEN}/config.h)
    file(WRITE ${MYSOFA_GEN}/mysofa_export.h "#pragma once\n#define MYSOFA_EXPORT\n")

    add_library(tac_mysofa STATIC
        ${MYSOFA_SRC}/hrtf/reader.c      ${MYSOFA_SRC}/hdf/superblock.c  ${MYSOFA_SRC}/hdf/dataobject.c
        ${MYSOFA_SRC}/hdf/btree.c        ${MYSOFA_SRC}/hdf/fractalhead.c ${MYSOFA_SRC}/hdf/gunzip.c
        ${MYSOFA_SRC}/hdf/gcol.c         ${MYSOFA_SRC}/hrtf/check.c      ${MYSOFA_SRC}/hrtf/spherical.c
        ${MYSOFA_SRC}/hrtf/lookup.c      ${MYSOFA_SRC}/hrtf/tools.c      ${MYSOFA_SRC}/hrtf/kdtree.c
        ${MYSOFA_SRC}/hrtf/neighbors.c   ${MYSOFA_SRC}/hrtf/interpolate.c ${MYSOFA_SRC}/hrtf/resample.c
        ${MYSOFA_SRC}/hrtf/loudness.c    ${MYSOFA_SRC}/hrtf/minphase.c   ${MYSOFA_SRC}/hrtf/easy.c
        ${MYSOFA_SRC}/hrtf/cache.c       ${MYSOFA_SRC}/resampler/speex_resampler.c)
    target_include_directories(tac_mysofa PUBLIC ${MYSOFA_SRC}/hrtf ${MYSOFA_GEN})
    target_link_libraries(tac_mysofa PRIVATE ZLIB::ZLIBSTATIC)
    target_compile_definitions(tac_mysofa PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(tac_mysofa PRIVATE /W0)
    endif()

    add_library(tac_sofa STATIC src/sofa/sofa_import.cpp)
    target_link_libraries(tac_sofa PUBLIC tac_core PRIVATE tac_mysofa)
    target_compile_definitions(tac_sofa PUBLIC TAC_HAS_SOFA=1)

    # Ship the MIT KEMAR set (free to use with attribution) next to the binaries.
    set(TAC_KEMAR_SOFA ${libmysofa_SOURCE_DIR}/share/MIT_KEMAR_normal_pinna.sofa)
endif()

# ---------------------------------------------------------------- shared CLI helpers
add_library(tac_cli STATIC apps/common/cli.cpp)
target_include_directories(tac_cli PUBLIC apps/common)
target_link_libraries(tac_cli PUBLIC tac_core)
if(TAC_WITH_SOFA)
    target_link_libraries(tac_cli PUBLIC tac_sofa)
endif()

# ---------------------------------------------------------------- offline renderer
add_executable(tacrender apps/tacrender/main.cpp)
target_link_libraries(tacrender PRIVATE tac_cli)
if(TAC_WITH_SOFA)
    add_custom_command(TARGET tacrender POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:tacrender>/hrtf
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TAC_KEMAR_SOFA} $<TARGET_FILE_DIR:tacrender>/hrtf/
        VERBATIM)
endif()

# ---------------------------------------------------------------- real-time engine (Windows / WASAPI)
if(WIN32)
    add_library(tac_platform STATIC src/platform/wasapi.cpp)
    target_include_directories(tac_platform PUBLIC src)
    target_link_libraries(tac_platform PUBLIC ole32 avrt)

    add_executable(tacaudio apps/tacaudio/main.cpp)
    target_link_libraries(tacaudio PRIVATE tac_session)
    set_target_properties(tacaudio PROPERTIES OUTPUT_NAME tacaudio-cli)  # TacAudio.exe is the app

    add_library(tac_session STATIC apps/common/session.cpp apps/common/cable_check.cpp apps/common/apo_control.cpp)
    target_link_libraries(tac_session PUBLIC tac_cli tac_platform shell32)

    # ------------------------------------------------------------ GUI (Dear ImGui, Win32 + D3D11)
    if(TAC_WITH_GUI)
        include(FetchContent)
        FetchContent_Declare(imgui
            GIT_REPOSITORY https://github.com/ocornut/imgui.git
            GIT_TAG v1.92.9
            GIT_SHALLOW TRUE
            SOURCE_SUBDIR _tacaudio_no_cmake)
        FetchContent_MakeAvailable(imgui)
        add_library(tac_imgui STATIC
            ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
            ${imgui_SOURCE_DIR}/imgui_widgets.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
        target_include_directories(tac_imgui PUBLIC ${imgui_SOURCE_DIR})
        target_compile_definitions(tac_imgui PUBLIC IMGUI_DEFINE_MATH_OPERATORS)  # ImVec2 + - * /
        target_link_libraries(tac_imgui PUBLIC d3d11 d3dcompiler dxgi dwmapi)
        if(MSVC)
            target_compile_options(tac_imgui PRIVATE /W0)
        endif()

        set(TAC_RES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/apps/tacaudio_gui/res)
        configure_file(${TAC_RES_DIR}/tacaudio.rc.in ${CMAKE_CURRENT_BINARY_DIR}/tacaudio.rc @ONLY)
        add_executable(tacaudio_gui WIN32
            apps/tacaudio_gui/main.cpp apps/tacaudio_gui/app.cpp apps/tacaudio_gui/ui_widgets.cpp
            ${CMAKE_CURRENT_BINARY_DIR}/tacaudio.rc)
        target_link_libraries(tacaudio_gui PRIVATE tac_session tac_imgui shell32)
        target_compile_definitions(tacaudio_gui PRIVATE TAC_VERSION="${PROJECT_VERSION}")
        set_target_properties(tacaudio_gui PROPERTIES OUTPUT_NAME TacAudio)
        if(TAC_WITH_SOFA)  # the GUI finds hrtf/ next to its exe, like tacrender
            add_custom_command(TARGET tacaudio_gui POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:tacaudio_gui>/hrtf
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TAC_KEMAR_SOFA} $<TARGET_FILE_DIR:tacaudio_gui>/hrtf/
                VERBATIM)
        endif()
    endif()

    # ------------------------------------------------------------ APO prototype (audio-engine add-on)
    add_library(tac_apo SHARED apps/apo/tac_apo.cpp apps/apo/apo_engine.cpp apps/apo/tac_apo.def)
    target_link_libraries(tac_apo PRIVATE tac_cli ole32 advapi32 shell32)
    set_target_properties(tac_apo PROPERTIES OUTPUT_NAME TacAudioApo)
    add_executable(tacapo_setup apps/apo/setup.cpp)
    target_link_libraries(tacapo_setup PRIVATE tac_platform advapi32 ole32)
    set_target_properties(tacapo_setup PROPERTIES OUTPUT_NAME tacapo-setup)
    add_dependencies(tacapo_setup tac_apo)
endif()

# ---------------------------------------------------------------- tests
if(TAC_BUILD_TESTS)
    enable_testing()
    add_executable(tac_tests tests/tests.cpp)
    target_link_libraries(tac_tests PRIVATE tac_core)
    if(TAC_WITH_SOFA)
        target_link_libraries(tac_tests PRIVATE tac_sofa)
        target_compile_definitions(tac_tests PRIVATE TAC_TEST_SOFA_FILE="${TAC_KEMAR_SOFA}")
    endif()
    add_test(NAME tac_tests COMMAND tac_tests)
endif()
