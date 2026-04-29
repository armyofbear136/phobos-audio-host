// PhobosHost — headless JUCE-based VST3 host.
// Session 1: binary launches, opens default audio device, plays silence,
// listens on TCP/16332 (control) and UDP/16331 (OSC notes).
//
// See: PHOBOS-PhobosHost-Spec.md §6 Session 1, §3.

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include "ControlServer.h"
#include "Logger.h"
#include "OscServer.h"

namespace phobos {

class App
{
public:
    bool init()
    {
        // ── Audio device + graphs ────────────────────────────────────────
        auto initErr = deviceManager.initialiseWithDefaultDevices(0, 2);
        if (initErr.isNotEmpty())
        {
            PHOBOS_ERR("AudioDeviceManager init failed: %s", initErr.toRawUTF8());
            return false;
        }

        auto* device = deviceManager.getCurrentAudioDevice();
        if (device == nullptr)
        {
            PHOBOS_ERR("AudioDeviceManager: no current device after init");
            return false;
        }

        const double sampleRate = device->getCurrentSampleRate();
        const int    blockSize  = device->getCurrentBufferSizeSamples();

        PHOBOS_LOG("Audio device: %s, %.0f Hz, %d samples",
                   device->getName().toRawUTF8(), sampleRate, blockSize);

        configureGraph(dawGraph,       sampleRate, blockSize, /*withMidiIn*/ true);
        configureGraph(accessoryGraph, sampleRate, blockSize, /*withMidiIn*/ false);

        dawPlayer.setProcessor(&dawGraph);
        deviceManager.addAudioCallback(&dawPlayer);

        // ── Servers ──────────────────────────────────────────────────────
        installControlOps();

        if (! controlServer.start()) return false;
        if (! oscServer.start())     { controlServer.stop(); return false; }

        // Wire Logger → ControlServer log-event forwarder. Ring buffer is
        // drained on the message thread; this hop is safe.
        Logger::instance().setForwarder(
            [this](int level, const char* line) { controlServer.forwardLog(level, line); });

        // ── Drain timer (message thread, 60 Hz) ──────────────────────────
        drainTimer = std::make_unique<DrainTimer>();
        drainTimer->startTimerHz(60);

        PHOBOS_LOG("PhobosHost: ready");
        return true;
    }

    void shutdown()
    {
        if (drainTimer != nullptr) drainTimer->stopTimer();

        // Drain anything still buffered before tearing down the forwarder.
        Logger::instance().setForwarder({});
        Logger::instance().drainOnce(Logger::kSlots);

        oscServer.stop();
        controlServer.stop();

        deviceManager.removeAudioCallback(&dawPlayer);
        dawPlayer.setProcessor(nullptr);

        dawGraph.releaseResources();
        accessoryGraph.releaseResources();
    }

private:
    using GraphIO = juce::AudioProcessorGraph::AudioGraphIOProcessor;

    static void configureGraph(juce::AudioProcessorGraph& graph,
                               double sampleRate, int blockSize, bool withMidiIn)
    {
        graph.clear();
        graph.setPlayConfigDetails(0, 2, sampleRate, blockSize);
        graph.prepareToPlay(sampleRate, blockSize);

        auto outNode = graph.addNode(
            std::make_unique<GraphIO>(GraphIO::audioOutputNode));

        if (withMidiIn)
        {
            (void) graph.addNode(
                std::make_unique<GraphIO>(GraphIO::midiInputNode));
        }

        // No connections — output reads silence. Session 2 will add plugin
        // nodes between midiInput and audioOutput.
        juce::ignoreUnused(outNode);
    }

    void installControlOps()
    {
        controlServer.registerOp("ping", [](const juce::var&, juce::String&) {
            auto* res = new juce::DynamicObject();
            res->setProperty("uptimeMs",
                static_cast<int64_t>(juce::Time::getMillisecondCounter()));
            res->setProperty("version", "0.1.0");
            return juce::var(res);
        });

        controlServer.registerOp("shutdown", [](const juce::var&, juce::String&) {
            // Post to message thread — we're already on the control thread,
            // but the dispatch loop only quits when invoked from there.
            juce::MessageManager::callAsync([]() {
                if (auto* mm = juce::MessageManager::getInstanceWithoutCreating())
                    mm->stopDispatchLoop();
            });
            return juce::var(new juce::DynamicObject());
        });
    }

    // ── State ────────────────────────────────────────────────────────────
    juce::AudioDeviceManager      deviceManager;
    juce::AudioProcessorGraph     dawGraph;
    juce::AudioProcessorGraph     accessoryGraph;
    juce::AudioProcessorPlayer    dawPlayer;

    OscServer                     oscServer;
    ControlServer                 controlServer;

    // Drain Logger 60×/s on the message thread.
    struct DrainTimer : public juce::Timer
    {
        void timerCallback() override { Logger::instance().drainOnce(); }
    };
    std::unique_ptr<DrainTimer> drainTimer;
};

} // namespace phobos

int main(int /*argc*/, char* /*argv*/[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;     // creates MessageManager + audio bits

    phobos::App app;
    if (! app.init())
    {
        // Drain whatever errors got logged before we exit.
        phobos::Logger::instance().drainOnce(phobos::Logger::kSlots);
        return 1;
    }

    juce::MessageManager::getInstance()->runDispatchLoop();

    app.shutdown();
    phobos::Logger::instance().drainOnce(phobos::Logger::kSlots);
    return 0;
}
