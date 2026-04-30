#pragma once

#include <memory>
#include <unordered_map>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

namespace phobos {

// Per-slot plugin UI windows. Each entry maps a slotId to a JUCE
// DocumentWindow holding the plugin's native AudioProcessorEditor.
//
// Hide-on-close semantics: clicking the close button hides the window but
// preserves the window + editor objects, so reopening is instant and the
// editor's internal state (window position, scroll positions, etc.) is
// retained. Only `removeSlot` actually destroys the window.
//
// All public methods are message-thread only.
class WindowManager
{
public:
    WindowManager() = default;
    ~WindowManager();

    // Opens (or brings forward) the UI window for a slot. Returns empty on
    // success, or an error string if the plugin has no editor.
    juce::String show(int slotId, juce::AudioProcessor* processor,
                      const juce::String& titleSuffix);

    // Hides the window (without destroying it). No-op if nothing open.
    void hide(int slotId);

    // Destroys the window for a slot. Called from DawGraph's slot-removed
    // callback when a plugin is unloaded. No-op if nothing open.
    void removeSlot(int slotId);

    // Closes everything. Called at shutdown.
    void closeAll();

private:
    // Stored as the public DocumentWindow base. The actual subclass with
    // hide-on-close behavior lives entirely in the .cpp anonymous namespace.
    // ~DocumentWindow is virtual, so destruction via the base pointer
    // correctly invokes the subclass destructor.
    std::unordered_map<int, std::unique_ptr<juce::DocumentWindow>> windows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WindowManager)
};

} // namespace phobos
