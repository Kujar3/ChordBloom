cmake_minimum_required(VERSION 3.22)

project(Chordbloom VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(MSVC)
    add_compile_options(/MP /utf-8)
endif()

set(JUCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JUCE" CACHE PATH "Path to the JUCE checkout")
add_subdirectory("${JUCE_PATH}" JUCE EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Source lists
# ---------------------------------------------------------------------------
set(CB_CORE_SOURCES
    src/core/Theory.cpp
    src/core/Palette.cpp
    src/core/Suggest.cpp
    src/core/Voicing.cpp
    src/core/Pattern.cpp
    src/core/Progression.cpp)

set(CB_PLUGIN_SOURCES
    src/plugin/PluginProcessor.cpp
    src/plugin/PreviewSynth.cpp
    src/plugin/MidiExport.cpp)

set(CB_UI_SOURCES
    src/ui/Theme.cpp
    src/ui/PluginEditor.cpp
    src/ui/HeaderBar.cpp
    src/ui/ChordPaletteView.cpp
    src/ui/ProgressionLane.cpp
    src/ui/ControlsPanel.cpp
    src/ui/KeyboardGuide.cpp
    src/ui/ExportPanel.cpp)

# ---------------------------------------------------------------------------
# Embedded resources (fonts, logo)
# ---------------------------------------------------------------------------
file(GLOB CB_RESOURCE_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts/*.ttf"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/images/*.png")
juce_add_binary_data(ChordbloomData
    NAMESPACE ChordbloomBinary
    HEADER_NAME ChordbloomBinary.h
    SOURCES ${CB_RESOURCE_FILES})
set_target_properties(ChordbloomData PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(CB_JUCE_MODULES
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_dsp
    juce::juce_gui_extra)

set(CB_JUCE_DEFINES
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

# ---------------------------------------------------------------------------
# The plugin (VST3 + Standalone)
# ---------------------------------------------------------------------------
juce_add_plugin(Chordbloom
    COMPANY_NAME "Kujar3"
    PRODUCT_NAME "Chordbloom"
    BUNDLE_ID "com.kujar3.chordbloom"
    PLUGIN_MANUFACTURER_CODE Kuja
    PLUGIN_CODE Cblm
    FORMATS VST3 Standalone
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES Instrument Generator
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(Chordbloom PRIVATE ${CB_CORE_SOURCES} ${CB_PLUGIN_SOURCES} ${CB_UI_SOURCES})
target_include_directories(Chordbloom PRIVATE src)
target_compile_definitions(Chordbloom PUBLIC ${CB_JUCE_DEFINES})
target_link_libraries(Chordbloom
    PRIVATE ChordbloomData ${CB_JUCE_MODULES}
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Pure C++ core unit tests (no JUCE, compiles in seconds)
# ---------------------------------------------------------------------------
add_executable(CoreTests
    tests/CoreTestsMain.cpp
    tests/TheoryTests.cpp
    tests/VoicingPatternTests.cpp
    ${CB_CORE_SOURCES})
target_include_directories(CoreTests PRIVATE src tests)

# ---------------------------------------------------------------------------
# Dev tool: processor self-test + editor snapshot renderer
#   ChordbloomDev selftest
#   ChordbloomDev snapshot <outDir>
# ---------------------------------------------------------------------------
juce_add_console_app(ChordbloomDev PRODUCT_NAME "ChordbloomDev")
target_sources(ChordbloomDev PRIVATE
    tools/dev/DevMain.cpp
    tools/dev/SelfTest.cpp
    tools/dev/Snapshot.cpp
    ${CB_CORE_SOURCES} ${CB_PLUGIN_SOURCES} ${CB_UI_SOURCES})
target_include_directories(ChordbloomDev PRIVATE src)
target_compile_definitions(ChordbloomDev PRIVATE
    ${CB_JUCE_DEFINES}
    JucePlugin_Name="Chordbloom"
    CHORDBLOOM_DEV_TOOL=1)
target_link_libraries(ChordbloomDev
    PRIVATE ChordbloomData ${CB_JUCE_MODULES}
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
