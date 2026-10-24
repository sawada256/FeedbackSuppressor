cmake_minimum_required(VERSION 3.20)
project(FeedbackSuppressor VERSION 3.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# macOS Universal Binary support (Intel x86_64 & Apple Silicon arm64)
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build architectures for Mac OS X" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum OS X deployment version" FORCE)
endif()

include(FetchContent)

# Fetch JUCE 8.0.6 (Supports macOS 15+ SDK and Xcode 16)
message(STATUS "Fetching JUCE 8.0.6...")
FetchContent_Declare(
    juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.6
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(juce)

# Define the Audio Plugin Target
juce_add_plugin(FeedbackSuppressor
    COMPANY_NAME "LiveAudioLab"
    BUNDLE_ID "io.github.sawada256.feedbacksuppressorv3"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE LALb
    PLUGIN_CODE Fdb3
    FORMATS VST3 AU Standalone
    PRODUCT_NAME "Feedback Suppressor V3"
)

# The linker ad-hoc signs the binary, but JUCE adds Contents/Resources/moduleinfo.json
# afterwards, which invalidates the bundle seal. Re-sign the finished VST3 bundle
# (ad-hoc) so `codesign --verify --deep --strict` passes and hosts load it cleanly.
if(APPLE)
    foreach(format_target FeedbackSuppressor_VST3 FeedbackSuppressor_AU FeedbackSuppressor_Standalone)
        if(TARGET ${format_target})
            add_custom_command(TARGET ${format_target} POST_BUILD
                COMMAND codesign --force --deep --sign - "$<TARGET_BUNDLE_DIR:${format_target}>"
                COMMENT "Ad-hoc signing ${format_target} bundle"
                VERBATIM)
        endif()
    endforeach()
endif()

# Source files
target_sources(FeedbackSuppressor PRIVATE
    source/PluginProcessor.cpp
    source/PluginProcessor.h
    source/PluginEditor.cpp
    source/PluginEditor.h
    source/dsp/BiquadNotchFilter.h
    source/dsp/FeedbackDetector.h
    source/dsp/FeedbackDetector.cpp
    source/dsp/FilterManager.h
    source/dsp/FilterManager.cpp
    source/dsp/PanicLimiter.h
    source/ui/SpectrumVisualizer.h
    source/ui/SpectrumVisualizer.cpp
    source/ui/FilterListView.h
    source/ui/FilterListView.cpp
    source/ui/CustomControls.h
    source/ui/CustomControls.cpp
)

target_include_directories(FeedbackSuppressor PRIVATE
    source
    source/dsp
    source/ui
)

target_compile_definitions(FeedbackSuppressor PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
)

target_link_libraries(FeedbackSuppressor PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_dsp
    juce::juce_gui_basics
    juce::juce_gui_extra
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

# Verification Test Executable
add_executable(dsp_test
    tests/dsp_test.cpp
    source/dsp/FeedbackDetector.cpp
    source/dsp/FilterManager.cpp
)

target_include_directories(dsp_test PRIVATE
    source
    source/dsp
)

target_link_libraries(dsp_test PRIVATE
    juce::juce_audio_basics
    juce::juce_dsp
    juce::juce_core
)

# Diagnostic Sine Simulation Test
add_executable(sine_sim_test
    tests/sine_sim_test.cpp
    source/dsp/FeedbackDetector.cpp
    source/dsp/FilterManager.cpp
)

target_include_directories(sine_sim_test PRIVATE
    source
    source/dsp
)

target_link_libraries(sine_sim_test PRIVATE
    juce::juce_audio_basics
    juce::juce_dsp
    juce::juce_core
)


add_executable(music_detection_test
    tests/music_detection_test.cpp
    source/dsp/FeedbackDetector.cpp
    source/dsp/FilterManager.cpp
)
target_include_directories(music_detection_test PRIVATE source source/dsp)
target_link_libraries(music_detection_test PRIVATE juce::juce_audio_basics juce::juce_dsp juce::juce_core)
add_executable(v3_test tests/v3_test.cpp source/dsp/FeedbackDetector.cpp source/dsp/FilterManager.cpp)
target_include_directories(v3_test PRIVATE source source/dsp)
target_link_libraries(v3_test PRIVATE juce::juce_audio_basics juce::juce_dsp juce::juce_core)

# Processor-level smoke test (threads, state, latency, editor) as a console app
add_executable(processor_test
    tests/processor_test.cpp
    source/PluginProcessor.cpp
    source/PluginEditor.cpp
    source/dsp/FeedbackDetector.cpp
    source/dsp/FilterManager.cpp
    source/ui/SpectrumVisualizer.cpp
    source/ui/FilterListView.cpp
    source/ui/CustomControls.cpp)
target_include_directories(processor_test PRIVATE source source/dsp source/ui)
target_compile_definitions(processor_test PRIVATE
    JucePlugin_Name="Feedback Suppressor V3"
    JucePlugin_VersionString="${PROJECT_VERSION}"
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(processor_test PRIVATE juce::juce_audio_utils juce::juce_audio_processors juce::juce_dsp
    juce::juce_gui_basics juce::juce_gui_extra juce::juce_recommended_config_flags)

enable_testing()
add_test(NAME legacy_dsp COMMAND dsp_test)
add_test(NAME music_detection COMMAND music_detection_test)
add_test(NAME v3_regression COMMAND v3_test)
add_test(NAME processor_smoke COMMAND processor_test)

# Metrics benchmark (detection time, false positives, frequency error, CPU)
add_executable(fs_benchmark tests/benchmark.cpp source/dsp/FeedbackDetector.cpp source/dsp/FilterManager.cpp)
target_include_directories(fs_benchmark PRIVATE source source/dsp)
target_link_libraries(fs_benchmark PRIVATE juce::juce_audio_basics juce::juce_dsp juce::juce_core)
