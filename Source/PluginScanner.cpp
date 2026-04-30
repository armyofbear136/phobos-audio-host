#include "PluginScanner.h"
#include "Logger.h"

namespace phobos {

PluginScanner::PluginScanner()
{
    // We only host VST3 today (per spec §3 / CMakeLists). Adding other formats
    // is one line per format here, plus the corresponding JUCE_PLUGINHOST_*
    // define in CMakeLists.
    auto vst3 = std::make_unique<juce::VST3PluginFormat>();
    vst3Format = vst3.get();
    formats.addFormat(std::move(vst3));
}

PluginScanner::Result PluginScanner::scanDirectory(const juce::File& directory)
{
    Result out;

    if (! directory.isDirectory())
    {
        PHOBOS_WARN("PluginScanner: not a directory: %s",
                    directory.getFullPathName().toRawUTF8());
        return out;
    }

    // findChildFiles with `findFilesAndDirectories | searchRecursively` returns
    // both the file-style plugins (`Helm.vst3`) AND the bundle directories
    // (`PhobosCrystal.vst3/`). Both are valid inputs to findAllTypesForFile.
    const auto entries = directory.findChildFiles(
        juce::File::findFilesAndDirectories | juce::File::ignoreHiddenFiles,
        /* searchRecursively */ true,
        /* wildcard */ "*.vst3");

    for (const auto& entry : entries)
    {
        ++out.scannedFiles;

        juce::OwnedArray<juce::PluginDescription> typesHere;
        vst3Format->findAllTypesForFile(typesHere, entry.getFullPathName());

        if (typesHere.isEmpty())
        {
            ++out.failedFiles;
            PHOBOS_WARN("PluginScanner: no types in %s",
                        entry.getFullPathName().toRawUTF8());
            continue;
        }

        for (auto* desc : typesHere)
            out.plugins.push_back(*desc);
    }

    PHOBOS_LOG("PluginScanner: scanned %s — %d plugin(s) from %d file(s), %d failed",
               directory.getFullPathName().toRawUTF8(),
               static_cast<int>(out.plugins.size()),
               out.scannedFiles, out.failedFiles);

    return out;
}

std::vector<juce::PluginDescription>
PluginScanner::scanFile(const juce::File& vst3Path)
{
    std::vector<juce::PluginDescription> result;

    juce::OwnedArray<juce::PluginDescription> typesHere;
    vst3Format->findAllTypesForFile(typesHere, vst3Path.getFullPathName());

    for (auto* desc : typesHere)
        result.push_back(*desc);

    return result;
}

} // namespace phobos
