cmake_minimum_required(VERSION 3.22)

project(VESSEL VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

# Point this at a local JUCE checkout to skip the download, e.g.
#   cmake -B build -DVESSEL_JUCE_PATH=$HOME/Documents/JUCE
set(VESSEL_JUCE_PATH "" CACHE PATH "Local JUCE checkout (empty = download JUCE 8.0.9)")
if(VESSEL_JUCE_PATH)
  add_subdirectory(${VESSEL_JUCE_PATH} ${CMAKE_BINARY_DIR}/JUCE)
else()
  include(FetchContent)
  FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.9
    GIT_SHALLOW ON)
  FetchContent_MakeAvailable(JUCE)
endif()

set(VESSEL_FORMATS VST3 Standalone)
if(APPLE)
  list(APPEND VESSEL_FORMATS AU)
endif()

juce_add_plugin(Vessel
  COMPANY_NAME                "iisben"
  BUNDLE_ID                   "audio.vessel.vessel"
  PLUGIN_MANUFACTURER_CODE    Vsla
  PLUGIN_CODE                 Vssl
  FORMATS                     ${VESSEL_FORMATS}
  PRODUCT_NAME                "VESSEL"
  IS_SYNTH                    TRUE
  NEEDS_MIDI_INPUT            TRUE
  NEEDS_MIDI_OUTPUT           FALSE
  IS_MIDI_EFFECT              FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS TRUE
  COPY_PLUGIN_AFTER_BUILD     FALSE
  VST3_CATEGORIES             Instrument Synth Fx
  AU_MAIN_TYPE                kAudioUnitType_MusicDevice)

# JUCE's AU bundle comes out with only the linker's ad-hoc signature on its binary, which `codesign -v` rejects for
# the bundle. Seal the whole bundle ad hoc (the VST3 already is); release builds re-sign with a real identity.
if(APPLE AND TARGET Vessel_AU)
  add_custom_command(TARGET Vessel_AU POST_BUILD
    COMMAND codesign --force --sign - "$<TARGET_BUNDLE_DIR:Vessel_AU>"
    COMMENT "Ad-hoc signing the AU bundle"
    VERBATIM)
endif()

# Factory preset bank + embedded SIL OFL typography (Space Grotesk + JetBrains Mono), compiled in
juce_add_binary_data(VesselData SOURCES
  Presets/FactoryBank.xml
  Resources/Fonts/SpaceGrotesk-Bold.ttf
  Resources/Fonts/SpaceGrotesk-Medium.ttf
  Resources/Fonts/JetBrainsMono-Medium.ttf)
set_target_properties(VesselData PROPERTIES POSITION_INDEPENDENT_CODE TRUE)

target_sources(Vessel PRIVATE
  Source/Parameters.cpp
  Source/Presets.cpp
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp
  Source/dsp/Wavetable.cpp
  Source/dsp/Tables.cpp
  Source/ui/Reliquary.cpp
  Source/ui/Controls.cpp
  Source/ui/HeroCenterpiece.cpp
  Source/ui/Torch.cpp
  Source/ui/Panels.cpp
  Source/ui/Routing.cpp)

target_include_directories(Vessel PRIVATE Source)

target_compile_definitions(Vessel PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

if(WIN32)
  target_compile_definitions(Vessel PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

target_link_libraries(Vessel
  PRIVATE
    VesselData
    juce::juce_audio_utils
    juce::juce_dsp
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# Wavetables (CC0, 16-bit FLAC): copied into each bundle's Contents/Resources/Wavetables
foreach(fmt VST3 AU Standalone)
  if(TARGET Vessel_${fmt})
    juce_add_bundle_resources_directory(Vessel_${fmt} Wavetables)
  endif()
endforeach()

# juce_add_bundle_resources_directory only works on Apple. On Windows/Linux copy Wavetables beside targets.
if(WIN32 OR UNIX AND NOT APPLE)
  if(TARGET Vessel_VST3)
    add_custom_command(TARGET Vessel_VST3 POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/Wavetables" "$<TARGET_FILE_DIR:Vessel_VST3>/../Resources/Wavetables"
      VERBATIM)
  endif()
  if(TARGET Vessel_Standalone)
    add_custom_command(TARGET Vessel_Standalone POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/Wavetables" "$<TARGET_FILE_DIR:Vessel_Standalone>/Wavetables"
      VERBATIM)
  endif()
endif()

# Offline render + UI snapshot harness (not shipped):  cmake -B build -DVESSEL_BUILD_CHECK=ON
option(VESSEL_BUILD_CHECK "Build the VesselCheck offline test tool" OFF)
if(VESSEL_BUILD_CHECK)
  add_executable(VesselCheck tools/VesselCheck.cpp)
  target_link_libraries(VesselCheck PRIVATE Vessel)
  target_include_directories(VesselCheck PRIVATE Source $<TARGET_PROPERTY:Vessel,INCLUDE_DIRECTORIES>)
  target_compile_definitions(VesselCheck PRIVATE $<TARGET_PROPERTY:Vessel,COMPILE_DEFINITIONS> VESSEL_TABLES_DIR="${CMAKE_SOURCE_DIR}/Wavetables")

  # Objective DSP measurements (zipper, toggles, aliasing, per-stage CPU):  VesselLab [all|zipper|toggles|alias|cpu]
  add_executable(VesselLab tools/VesselLab.cpp)
  target_link_libraries(VesselLab PRIVATE Vessel)
  target_include_directories(VesselLab PRIVATE Source $<TARGET_PROPERTY:Vessel,INCLUDE_DIRECTORIES>)
  target_compile_definitions(VesselLab PRIVATE $<TARGET_PROPERTY:Vessel,COMPILE_DEFINITIONS> VESSEL_TABLES_DIR="${CMAKE_SOURCE_DIR}/Wavetables")

  # Loads the built .vst3 as a host would:  HostProbe path/to/VESSEL.vst3
  juce_add_console_app(HostProbe PRODUCT_NAME "HostProbe")
  target_sources(HostProbe PRIVATE tools/HostProbe.cpp)
  target_compile_definitions(HostProbe PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
  target_link_libraries(HostProbe PRIVATE juce::juce_audio_utils juce::juce_audio_processors
                        juce::juce_recommended_config_flags juce::juce_recommended_lto_flags)
endif()

# Permanent headless 8-gate DSP QA harness (zero GUI, OpenGL, or OS audio-device dependencies)
option(VESSEL_BUILD_QA "Build the headless Vessel2QA 8-gate DSP QA executable" ON)
if(VESSEL_BUILD_QA)
  juce_add_console_app(Vessel2QA PRODUCT_NAME "Vessel2QA")
  target_sources(Vessel2QA PRIVATE
    tools/Vessel2QA.cpp
    Source/Parameters.cpp
    Source/Presets.cpp
    Source/PluginProcessor.cpp
    Source/dsp/Wavetable.cpp
    Source/dsp/Tables.cpp)
  target_include_directories(Vessel2QA PRIVATE Source)
  target_compile_definitions(Vessel2QA PRIVATE
    VESSEL_HEADLESS=1
    VESSEL_TABLES_DIR="${CMAKE_SOURCE_DIR}/Wavetables"
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_USE_FLAC=1)
  if(WIN32)
    target_compile_definitions(Vessel2QA PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  endif()
  target_link_libraries(Vessel2QA PRIVATE
    VesselData
    juce::juce_audio_processors
    juce::juce_audio_formats
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

  # Headless Retina UI snapshot QA runner (ScopedJuceInitialiser_GUI, zero OpenGL/audio-device deps)
  juce_add_console_app(Vessel2UiQA PRODUCT_NAME "Vessel2UiQA")
  target_sources(Vessel2UiQA PRIVATE
    tools/Vessel2UiQA.cpp
    Source/Parameters.cpp
    Source/Presets.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/dsp/Wavetable.cpp
    Source/dsp/Tables.cpp
    Source/ui/Reliquary.cpp
    Source/ui/Controls.cpp
    Source/ui/HeroCenterpiece.cpp
    Source/ui/Torch.cpp
    Source/ui/Panels.cpp
    Source/ui/Routing.cpp)
  target_include_directories(Vessel2UiQA PRIVATE Source)
  target_compile_definitions(Vessel2UiQA PRIVATE
    VESSEL_TABLES_DIR="${CMAKE_SOURCE_DIR}/Wavetables"
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_USE_FLAC=1)
  if(WIN32)
    target_compile_definitions(Vessel2UiQA PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  endif()
  target_link_libraries(Vessel2UiQA PRIVATE
    VesselData
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)
endif()
