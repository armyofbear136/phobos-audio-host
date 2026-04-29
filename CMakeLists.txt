cmake_minimum_required(VERSION 3.22)
project(PhobosHost VERSION 0.1.0)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_subdirectory(JUCE)

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
)

target_include_directories(PhobosHost PRIVATE
    Source
)

target_link_libraries(PhobosHost PRIVATE
    juce::juce_audio_devices
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_events
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

target_compile_definitions(PhobosHost PRIVATE
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LV2=0
    JUCE_PLUGINHOST_AU=$<IF:$<PLATFORM_ID:Darwin>,1,0>
    $<$<PLATFORM_ID:Windows>:NOMINMAX>
    $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>
)
