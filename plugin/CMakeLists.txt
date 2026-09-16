# ---------------------------------------------------------------------------
# GameBaiters TTS - TeamSpeak 3 plugin.
#
# Type in a box, press Enter, a local neural voice speaks in the channel.
# The plugin is a thin, crash-safe Qt client: audio injection, effects, UI and
# talk-state handling live here; the neural model runs in a separate Python
# process (../backend) that streams PCM over loopback TCP.
#
# DSP chain + effects editor are the GameBaiters Soundboard ones, compiled IN
# PLACE from SOUNDBOARD_4.0 (same trick as Soundboard LIVE): one implementation,
# a DSP fix lands in both products.
#
#   .\build_local.ps1 -Local        (from the repo root)
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)
project(gb_tts LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(GBTTS_VERSION "1.5.0")   # release: also backend/gbtts/__init__.py + release-notes.txt (tools/release_check.ps1)
set(SOUNDBOARD_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../SOUNDBOARD_4.0" CACHE PATH
    "GameBaiters Soundboard workspace (the folder holding upstream-clone/)")
get_filename_component(SOUNDBOARD_ROOT "${SOUNDBOARD_ROOT}" ABSOLUTE)
set(SB_SRC "${SOUNDBOARD_ROOT}/upstream-clone/src")
set(SB_SDK "${SOUNDBOARD_ROOT}/upstream-clone/pluginsdk/include")

if(NOT EXISTS "${SB_SRC}/dsp/SlotDsp.cpp")
    message(FATAL_ERROR "Soundboard sources not found in ${SB_SRC}. GameBaiters TTS compiles "
                        "the Soundboard DSP + effects editor in place: keep SOUNDBOARD_4.0 next to "
                        "this repo or pass -DSOUNDBOARD_ROOT=<path>.")
endif()

find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets Network LinguistTools)

# ---------------------------------------------------------------------------
# Leia HRTF deps (libmysofa + zlib), static. Reuse the Soundboard's populated
# sources so configuring needs no network.
# ---------------------------------------------------------------------------
include(FetchContent)
if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()
foreach(dep zlib mysofa)
    if(EXISTS "${SOUNDBOARD_ROOT}/build_local/_deps/${dep}-src/CMakeLists.txt")
        string(TOUPPER ${dep} DEP_UP)
        set(FETCHCONTENT_SOURCE_DIR_${DEP_UP} "${SOUNDBOARD_ROOT}/build_local/_deps/${dep}-src" CACHE PATH "" FORCE)
    endif()
endforeach()

set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(zlib GIT_REPOSITORY https://github.com/madler/zlib.git GIT_TAG v1.3.1 GIT_SHALLOW TRUE)
FetchContent_GetProperties(zlib)
if(NOT zlib_POPULATED)
    FetchContent_Populate(zlib)
    add_subdirectory(${zlib_SOURCE_DIR} ${zlib_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()
set(ZLIB_FOUND TRUE CACHE BOOL "" FORCE)
set(ZLIB_INCLUDE_DIRS "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}" CACHE STRING "" FORCE)
set(ZLIB_LIBRARIES zlibstatic CACHE STRING "" FORCE)
set(ZLIB_INCLUDE_DIR "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}" CACHE PATH "" FORCE)
set(ZLIB_LIBRARY zlibstatic CACHE STRING "" FORCE)

set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MYSOFA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(mysofa GIT_REPOSITORY https://github.com/hoene/libmysofa.git GIT_TAG v1.3.2 GIT_SHALLOW TRUE)
FetchContent_GetProperties(mysofa)
if(NOT mysofa_POPULATED)
    FetchContent_Populate(mysofa)
    macro(install)
    endmacro()
    macro(export)
    endmacro()
    add_subdirectory(${mysofa_SOURCE_DIR} ${mysofa_BINARY_DIR} EXCLUDE_FROM_ALL)
    macro(install)
        _install(${ARGV})
    endmacro()
    macro(export)
        _export(${ARGV})
    endmacro()
endif()
foreach(leiaDep mysofa-static zlibstatic)
    if(TARGET ${leiaDep})
        set_target_properties(${leiaDep} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Shared Soundboard sources (compiled, never copied)
# ---------------------------------------------------------------------------
set(SHARED_DSP
    ${SB_SRC}/dsp/BiquadPeaking.cpp     ${SB_SRC}/dsp/Chorus.cpp
    ${SB_SRC}/dsp/Compressor.cpp        ${SB_SRC}/dsp/ConvolutionReverb.cpp
    ${SB_SRC}/dsp/DeEsser.cpp           ${SB_SRC}/dsp/Delay.cpp
    ${SB_SRC}/dsp/DynEq.cpp             ${SB_SRC}/dsp/EqRack.cpp
    ${SB_SRC}/dsp/Flanger.cpp           ${SB_SRC}/dsp/Flangus.cpp
    ${SB_SRC}/dsp/Limiter.cpp           ${SB_SRC}/dsp/NoiseGate.cpp
    ${SB_SRC}/dsp/Paulstretch.cpp       ${SB_SRC}/dsp/Phaser.cpp
    ${SB_SRC}/dsp/Positional.cpp        ${SB_SRC}/dsp/Reverb.cpp
    ${SB_SRC}/dsp/SandboxState.cpp      ${SB_SRC}/dsp/Saturator.cpp
    ${SB_SRC}/dsp/SlotDsp.cpp           ${SB_SRC}/dsp/SpatialLeia.cpp
    ${SB_SRC}/dsp/TransientShaper.cpp   ${SB_SRC}/dsp/VoiceFx.cpp
    ${SB_SRC}/dsp/leia/HrtfProcessor.cpp
    ${SB_SRC}/dsp/leia/LeiaEngine.cpp
    ${SB_SRC}/dsp/leia/ShoeboxRoom.cpp
)

set(SHARED_UI
    ${SB_SRC}/style_helper.cpp
    ${SB_SRC}/ExpandableSection.cpp
    # ExpandableSection.ui is NOT listed: no source includes ui_ExpandableSection.h, and with
    # the Ninja generator (CI) the explicit .ui produced "multiple rules generate ui_*.h".
    ${SB_SRC}/SpeechBubble.cpp
    ${SB_SRC}/modules/theme.cpp
    ${SB_SRC}/modules/icon_factory.cpp
    ${SB_SRC}/modules/help_bubble.cpp
    ${SB_SRC}/modules/positional_pad.cpp
    ${SB_SRC}/modules/preset_manager.cpp
    ${SB_SRC}/modules/pipeline_widget.cpp
    ${SB_SRC}/modules/eq_band_widget.cpp
    ${SB_SRC}/modules/section_box.cpp
    ${SB_SRC}/modules/fine_slider.cpp
    ${SB_SRC}/modules/channel_meter.cpp
    ${SB_SRC}/modules/channel_sandbox_dialog.cpp
)

# ---------------------------------------------------------------------------
# Own sources
# ---------------------------------------------------------------------------
set(GBTTS_SOURCES
    src/plugin_exports.cpp
    src/compat.cpp
    src/gbtts.h                       src/gbtts.cpp

    src/audio/SpscRing.h
    src/audio/TtsAudio.h              src/audio/TtsAudio.cpp
    src/audio/LocalPlayer.h           src/audio/LocalPlayer.cpp

    src/net/BackendLink.h             src/net/BackendLink.cpp
    src/net/BackendProcess.h          src/net/BackendProcess.cpp
    src/net/EngineInstaller.h         src/net/EngineInstaller.cpp

    src/core/Settings.h               src/core/Settings.cpp
    src/core/Controller.h             src/core/Controller.cpp
    src/core/UpdateFeed.h             src/core/UpdateFeed.cpp
    src/core/Updater.h                src/core/Updater.cpp
    src/ts3/TalkState.h               src/ts3/TalkState.cpp

    src/ui/TtsIcons.h                 src/ui/TtsIcons.cpp
    src/ui/VoiceGroups.h              src/ui/VoiceGroups.cpp
    src/ui/TtsWindow.h                src/ui/TtsWindow.cpp
    src/ui/InputBox.h                 src/ui/InputBox.cpp
    src/ui/VoiceDialog.h              src/ui/VoiceDialog.cpp
    src/ui/SettingsDialog.h           src/ui/SettingsDialog.cpp
    src/ui/ToolbarButton.h            src/ui/ToolbarButton.cpp
    src/ui/UpdaterWindow.h            src/ui/UpdaterWindow.cpp
    src/ui/ProgressBar.h              src/ui/ProgressBar.cpp
    src/ui/EngineSetupDialog.h        src/ui/EngineSetupDialog.cpp
)

# Translations: English sources, Italian catalogue compiled at build time.
set(GBTTS_TS ${CMAKE_CURRENT_SOURCE_DIR}/i18n/gbtts_it.ts)
set_source_files_properties(${GBTTS_TS} PROPERTIES OUTPUT_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/i18n")
qt5_add_translation(GBTTS_QM ${GBTTS_TS})

# Resources. Paths the reused Soundboard code hard-codes (:/style, :/leia) are
# only ever COPIED out by their readers, so registering them from two DLLs is
# harmless. The translation catalogues are memory-mapped by QTranslator, so
# they get plugin-unique paths (:/gbtts/...) - a shared path could leave the
# Soundboard's translator pointing into this DLL after it unloads.
set(GBTTS_QRC "${CMAKE_CURRENT_BINARY_DIR}/gbtts_resources.qrc")
file(WRITE "${GBTTS_QRC}"
"<RCC>
  <qresource prefix=\"style\"><file alias=\"dark_style.qss\">${SB_SRC}/dark_style.qss</file></qresource>
  <qresource prefix=\"leia\"><file alias=\"default.sofa\">${SB_SRC}/leia/default.sofa</file></qresource>
  <qresource prefix=\"gbtts\">
    <file alias=\"i18n/soundboard_it.qm\">${SB_SRC}/i18n/soundboard_it.qm</file>
    <file alias=\"i18n/gbtts_it.qm\">${CMAKE_CURRENT_BINARY_DIR}/i18n/gbtts_it.qm</file>
  </qresource>
</RCC>
")
# Explicit rcc step (not AUTORCC): its dependency scan sees the generated .qm,
# so lrelease is guaranteed to run before the resource is compiled.
set_source_files_properties("${GBTTS_QRC}" PROPERTIES SKIP_AUTORCC ON)
qt5_add_resources(GBTTS_RCC "${GBTTS_QRC}")

add_library(gb_tts SHARED ${GBTTS_SOURCES} ${SHARED_DSP} ${SHARED_UI} ${GBTTS_RCC} ${GBTTS_QM})

target_include_directories(gb_tts PRIVATE
    src
    ${SB_SRC}
    ${SB_SDK}
    ${mysofa_SOURCE_DIR}/src/hrtf
)
target_compile_definitions(gb_tts PRIVATE
    _USE_MATH_DEFINES NOMINMAX WIN32_LEAN_AND_MEAN
    GBTTS_VERSION="${GBTTS_VERSION}")
target_link_libraries(gb_tts PRIVATE
    Qt5::Core Qt5::Gui Qt5::Widgets Qt5::Network
    mysofa-static zlibstatic)

if(WIN32)
    target_link_libraries(gb_tts PRIVATE winmm user32 advapi32 dxgi)
    set_target_properties(gb_tts PROPERTIES PREFIX "" SUFFIX "_win64.dll"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/release"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/debug")
endif()

if(MSVC)
    target_compile_definitions(gb_tts PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
    target_compile_options(gb_tts PRIVATE /MP /utf-8 /W3 /wd4267 /wd4005 /wd4244 /wd4100)
endif()

# ---------------------------------------------------------------------------
# Offline audio-core self-test (no TeamSpeak needed): -DGBTTS_BUILD_TESTS=ON
# ---------------------------------------------------------------------------
option(GBTTS_BUILD_TESTS "Build gbtts_audio_selftest" OFF)
if(GBTTS_BUILD_TESTS)
    add_executable(gbtts_audio_selftest
        tests/audio_selftest.cpp
        src/audio/TtsAudio.cpp src/audio/LocalPlayer.cpp src/compat.cpp
        ${SHARED_DSP})
    target_include_directories(gbtts_audio_selftest PRIVATE src ${SB_SRC} ${SB_SDK} ${mysofa_SOURCE_DIR}/src/hrtf)
    target_compile_definitions(gbtts_audio_selftest PRIVATE _USE_MATH_DEFINES NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(gbtts_audio_selftest PRIVATE Qt5::Core mysofa-static zlibstatic winmm)
    if(MSVC)
        target_compile_options(gbtts_audio_selftest PRIVATE /MP /utf-8 /wd4267 /wd4005 /wd4244)
    endif()
    set_target_properties(gbtts_audio_selftest PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/tests")

    # Settings migration / round-trip / write cost (isolated registry key).
    add_executable(gbtts_settings_selftest tests/settings_selftest.cpp src/core/Settings.cpp)
    target_include_directories(gbtts_settings_selftest PRIVATE src)
    target_link_libraries(gbtts_settings_selftest PRIVATE Qt5::Core)
    set_target_properties(gbtts_settings_selftest PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/tests")

    # Auto-updater: feed parsing, versions, notes, checksum, and the helper script
    # really executed against a fake client process. --validate-feed <xml> checks a
    # generated version.xml with the same parser the plugin uses (CI).
    add_executable(gbtts_update_selftest tests/update_selftest.cpp src/core/UpdateFeed.cpp)
    target_include_directories(gbtts_update_selftest PRIVATE src)
    target_compile_definitions(gbtts_update_selftest PRIVATE GBTTS_VERSION="${GBTTS_VERSION}")
    target_link_libraries(gbtts_update_selftest PRIVATE Qt5::Core)
    if(MSVC)
        target_compile_options(gbtts_update_selftest PRIVATE /utf-8)
    endif()
    set_target_properties(gbtts_update_selftest PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/tests")

    # Loads the real plugin DLL like the TeamSpeak client: lifecycle, leaks, crash.
    add_executable(gbtts_host_harness tests/host_harness.cpp)
    target_include_directories(gbtts_host_harness PRIVATE ${SB_SDK})
    target_compile_definitions(gbtts_host_harness PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(gbtts_host_harness PRIVATE Qt5::Widgets psapi dbghelp)
    add_dependencies(gbtts_host_harness gb_tts)
    if(MSVC)
        target_compile_options(gbtts_host_harness PRIVATE /utf-8)
        target_compile_options(gbtts_settings_selftest PRIVATE /utf-8)
    endif()
    set_target_properties(gbtts_host_harness PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/tests")
endif()
