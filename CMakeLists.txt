cmake_minimum_required(VERSION 3.22)

project(VESSEL VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
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

juce_add_plugin(Vessel
  COMPANY_NAME                "iisben"
  BUNDLE_ID                   "audio.vessel.vessel"
  PLUGIN_MANUFACTURER_CODE    Vsla
  PLUGIN_CODE                 Vssl
  FORMATS                     VST3 AU Standalone
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

# Factory preset bank (a juce::ValueTree XML document), compiled in
juce_add_binary_data(VesselData SOURCES Presets/FactoryBank.xml)
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
  Source/ui/Backdrop.cpp
  Source/ui/Panels.cpp
  Source/ui/Routing.cpp)

target_include_directories(Vessel PRIVATE Source)

target_compile_definitions(Vessel PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0
  JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(Vessel
  PRIVATE
    VesselData
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_opengl
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# The Echo Sound Works Core Tables (CC0), as 16-bit FLAC: copied into each bundle's Contents/Resources/Wavetables
foreach(fmt VST3 AU Standalone)
  if(TARGET Vessel_${fmt})
    juce_add_bundle_resources_directory(Vessel_${fmt} Wavetables)
  endif()
endforeach()

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
