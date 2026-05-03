cmake_minimum_required(VERSION 3.22)
project(PhobosHost VERSION 0.3.0)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_subdirectory(JUCE)

# Plugin-host flags MUST be set as INTERFACE definitions on the
# juce_audio_processors target itself — otherwise JUCE's own compilation of
# the module skips the VST3PluginFormat code, even if our sources see the
# define. This is a JUCE-CMake quirk; setting them via
# target_compile_definitions on our app target alone is not sufficient.
target_compile_definitions(juce_audio_processors INTERFACE
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LV2=0
    JUCE_PLUGINHOST_AU=$<IF:$<PLATFORM_ID:Darwin>,1,0>
)

juce_add_console_app(PhobosHost
    PRODUCT_NAME    "PhobosHost"
    COMPANY_NAME    "Phobos"
    BUNDLE_ID       com.phobos.host
)

juce_generate_juce_header(PhobosHost)

target_sources(PhobosHost PRIVATE
    Source/Main.cpp
    Source/ControlServer.cpp
    Source/OscServer.cpp
    Source/Logger.cpp
    Source/PluginScanner.cpp
    Source/DawGraph.cpp
    Source/PluginUi.cpp
)

target_include_directories(PhobosHost PRIVATE
    Source
)

target_link_libraries(PhobosHost PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_events
    juce::juce_gui_basics
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

target_compile_definitions(PhobosHost PRIVATE
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    # Repeat the plugin-host flags here so IntelliSense / clangd / MSVC's
    # squiggly-checker resolve VST3PluginFormat. The JUCE module's own
    # compilation reads them from the INTERFACE definition above; this
    # block is purely so the editor's view of our sources matches the
    # compiler's. Both must agree.
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LV2=0
    JUCE_PLUGINHOST_AU=$<IF:$<PLATFORM_ID:Darwin>,1,0>
    $<$<PLATFORM_ID:Windows>:NOMINMAX>
    $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>
)

