// Custom cross-platform JUCE Standalone application for VESSEL (macOS / Windows / Linux).
// Ensures Standalone:
//   1. Disables audio input on launch (0 input channels, 2 output channels) so it never grabs
//      a microphone or triggers mic permissions/feedback loops - only MIDI control is needed.
//   2. Automatically detects and enables all connected & hot-plugged MIDI input devices.

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_plugin_client/detail/juce_CreatePluginFilter.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>

namespace
{
void stripSavedAudioInput (juce::PropertiesFile* settings)
{
    if (settings == nullptr)
        return;

    if (auto xml = settings->getXmlValue ("audioSetup"))
    {
        bool changed = false;
        if (xml->hasAttribute ("audioInputDeviceName") && xml->getStringAttribute ("audioInputDeviceName").isNotEmpty())
        {
            xml->setAttribute ("audioInputDeviceName", "");
            changed = true;
        }
        if (xml->hasAttribute ("audioDeviceInChans") && xml->getStringAttribute ("audioDeviceInChans").isNotEmpty())
        {
            xml->setAttribute ("audioDeviceInChans", "");
            changed = true;
        }
        if (changed)
        {
            settings->setValue ("audioSetup", xml.get());
            settings->saveIfNeeded();
        }
    }
}

class VesselStandaloneApp final : public juce::JUCEApplication
{
public:
    VesselStandaloneApp()
    {
        juce::PropertiesFile::Options options;
        options.applicationName     = juce::CharPointer_UTF8 (JucePlugin_Name);
        options.filenameSuffix      = ".settings";
        options.osxLibrarySubFolder = "Application Support";
       #if JUCE_LINUX || JUCE_BSD
        options.folderName          = "~/.config";
       #else
        options.folderName          = "";
       #endif
        appProperties.setStorageParameters (options);
    }

    const juce::String getApplicationName() override    { return juce::CharPointer_UTF8 (JucePlugin_Name); }
    const juce::String getApplicationVersion() override { return JucePlugin_VersionString; }
    bool moreThanOneInstanceAllowed() override          { return true; }
    void anotherInstanceStarted (const juce::String&) override {}

    std::unique_ptr<juce::StandalonePluginHolder> createPluginHolder()
    {
        auto* userSettings = appProperties.getUserSettings();
        stripSavedAudioInput (userSettings);

        // 0 audio input channels, 2 output channels -> never opens a microphone on launch.
        // shouldAutoOpenMidiDevices = true -> auto-enables all connected & hot-plugged MIDI keyboards.
        juce::Array<juce::StandalonePluginHolder::PluginInOuts> channelConfig;
        channelConfig.add ({ 0, 2 });

        auto holder = std::make_unique<juce::StandalonePluginHolder> (
            userSettings,
            false,
            juce::String {},
            nullptr,
            channelConfig,
            true);

        // Ensure no stray audio input device remains open even from legacy settings
        auto setup = holder->deviceManager.getAudioDeviceSetup();
        if (setup.inputDeviceName.isNotEmpty() || setup.inputChannels.countNumberOfSetBits() > 0)
        {
            setup.inputDeviceName.clear();
            setup.inputChannels.clear();
            setup.useDefaultInputChannels = false;
            holder->deviceManager.setAudioDeviceSetup (setup, true);
        }

        // Immediately open all currently connected MIDI input devices (hot-plugging is also
        // handled every 500 ms by StandalonePluginHolder::timerCallback when autoOpenMidiDevices is true).
        const auto midiInputs = juce::MidiInput::getAvailableDevices();
        for (const auto& dev : midiInputs)
            holder->deviceManager.setMidiInputDeviceEnabled (dev.identifier, true);
        holder->lastMidiDevices = midiInputs;

        return holder;
    }

    juce::StandaloneFilterWindow* createWindow()
    {
        if (juce::Desktop::getInstance().getDisplays().displays.isEmpty())
        {
            jassertfalse;
            return nullptr;
        }

        return new juce::StandaloneFilterWindow (
            getApplicationName(),
            juce::LookAndFeel::getDefaultLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId),
            createPluginHolder());
    }

    void initialise (const juce::String&) override
    {
        mainWindow = juce::rawToUniquePtr (createWindow());

        if (mainWindow != nullptr)
        {
           #if JUCE_STANDALONE_FILTER_WINDOW_USE_KIOSK_MODE
            juce::Desktop::getInstance().setKioskModeComponent (mainWindow.get(), false);
           #endif
            mainWindow->setVisible (true);
            mainWindow->toFront (true);
        }
        else
        {
            pluginHolder = createPluginHolder();
        }
    }

    void shutdown() override
    {
        pluginHolder = nullptr;
        mainWindow = nullptr;
        appProperties.saveIfNeeded();
    }

    void systemRequestedQuit() override
    {
        if (pluginHolder != nullptr)
            pluginHolder->savePluginState();

        if (mainWindow != nullptr)
            mainWindow->pluginHolder->savePluginState();

        if (juce::ModalComponentManager::getInstance()->cancelAllModalComponents())
            juce::Timer::callAfterDelay (100, [] {
                if (auto* app = juce::JUCEApplicationBase::getInstance())
                    app->systemRequestedQuit();
            });
        else
            quit();
    }

private:
    juce::ApplicationProperties appProperties;
    std::unique_ptr<juce::StandaloneFilterWindow> mainWindow;
    std::unique_ptr<juce::StandalonePluginHolder> pluginHolder;
};
} // namespace

START_JUCE_APPLICATION (VesselStandaloneApp)
