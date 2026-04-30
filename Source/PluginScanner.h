#pragma once

#include <vector>
#include <juce_audio_processors/juce_audio_processors.h>

namespace phobos {

// Recursive scanner for VST3 plugins under a directory. Finds both file-style
// (`Helm.vst3`) and bundle-style (`PhobosCrystal.vst3/Contents/...`) plugins.
//
// The scan is synchronous and runs on the message thread — each plugin's
// `findAllTypesForFile` involves loading the .vst3, instantiating its factory,
// and asking it for type metadata. Cost is roughly tens of milliseconds per
// plugin; whole-system scans are seconds.
class PluginScanner
{
public:
    PluginScanner();

    struct Result
    {
        std::vector<juce::PluginDescription> plugins;
        int                                  scannedFiles  { 0 };
        int                                  failedFiles   { 0 };
    };

    // Scans `directory` recursively. Returns descriptions for every plugin
    // type successfully identified. `failedFiles` counts .vst3 entries that
    // existed but couldn't be queried (broken plugin, wrong arch, etc.).
    Result scanDirectory(const juce::File& directory);

    // Convenience: scan a single .vst3 path (file or bundle).
    std::vector<juce::PluginDescription> scanFile(const juce::File& vst3Path);

    juce::AudioPluginFormatManager& formatManager() noexcept { return formats; }

private:
    juce::AudioPluginFormatManager formats;
    juce::VST3PluginFormat*        vst3Format { nullptr };  // non-owning view into `formats`
};

} // namespace phobos
