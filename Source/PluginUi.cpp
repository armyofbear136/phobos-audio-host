#include "PluginUi.h"
#include "Logger.h"

namespace phobos {

namespace {

// JUCE DocumentWindow subclass that hides instead of destroying on close.
//
// The editor is added as the window's content component. JUCE's window
// owns the content component, so we use setContentNonOwned + manual
// management — that way `setVisible(false)` doesn't take the editor with
// it, and we control the editor's lifetime ourselves.
class PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow(const juce::String& title, juce::AudioProcessorEditor* editor)
        : juce::DocumentWindow(title,
                               juce::Colours::darkgrey,
                               juce::DocumentWindow::closeButton,
                               /*addToDesktop*/ true),
          ownedEditor(editor)
    {
        setContentNonOwned(ownedEditor.get(), /*resizeToFit*/ true);
        setUsingNativeTitleBar(true);
        setResizable(editor->isResizable(), /*useBottomRightCornerResizer*/ false);
    }

    ~PluginWindow() override
    {
        // Detach the editor before our owned-pointer destroys it; otherwise
        // the window's own destructor would try to clear the content.
        clearContentComponent();
        ownedEditor.reset();
    }

    // Hide-on-close: window stays alive, editor stays attached, just invisible.
    void closeButtonPressed() override
    {
        setVisible(false);
    }

private:
    std::unique_ptr<juce::AudioProcessorEditor> ownedEditor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindow)
};

} // namespace

WindowManager::~WindowManager() { closeAll(); }

juce::String WindowManager::show(int slotId, juce::AudioProcessor* processor,
                                  const juce::String& titleSuffix)
{
    if (processor == nullptr) return "no processor for slot";
    if (! processor->hasEditor()) return "plugin has no editor";

    auto it = windows.find(slotId);
    if (it != windows.end())
    {
        // Already created — bring forward.
        it->second->setVisible(true);
        it->second->toFront(/*makeActive*/ true);
        return {};
    }

    auto* editor = processor->createEditorIfNeeded();
    if (editor == nullptr) return "createEditorIfNeeded returned null";

    const auto title = processor->getName() + titleSuffix;
    auto window = std::make_unique<PluginWindow>(title, editor);

    window->centreWithSize(window->getWidth(), window->getHeight());
    window->setVisible(true);

    windows[slotId] = std::move(window);

    PHOBOS_LOG("WindowManager: opened UI for slot=%d (%s)",
               slotId, title.toRawUTF8());
    return {};
}

void WindowManager::hide(int slotId)
{
    auto it = windows.find(slotId);
    if (it == windows.end()) return;
    it->second->setVisible(false);

    PHOBOS_LOG("WindowManager: hid UI for slot=%d", slotId);
}

void WindowManager::removeSlot(int slotId)
{
    auto it = windows.find(slotId);
    if (it == windows.end()) return;
    windows.erase(it);                           // unique_ptr destructor closes the window

    PHOBOS_LOG("WindowManager: closed UI for slot=%d (slot removed)", slotId);
}

void WindowManager::closeAll()
{
    windows.clear();
}

} // namespace phobos
