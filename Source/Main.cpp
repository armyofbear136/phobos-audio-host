// PhobosHost — headless JUCE-based VST3 host.
// Session 2: scan VST3 directories, instantiate plugins, route MIDI through
// per-channel chains (instrument → fx → fx → out).

#include <cstdio>

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include "ControlServer.h"
#include "DawGraph.h"
#include "Logger.h"
#include "OscServer.h"
#include "PluginScanner.h"
#include "PluginUi.h"

namespace phobos {

// Packs a JUCE PluginDescription into a JSON object for the scan result.
static juce::DynamicObject* descToJson(const juce::PluginDescription& d)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("name",          d.name);
    o->setProperty("vendor",        d.manufacturerName);
    o->setProperty("version",       d.version);
    o->setProperty("category",      d.category);
    o->setProperty("format",        d.pluginFormatName);
    o->setProperty("isInstrument",  d.isInstrument);
    o->setProperty("numInputs",     d.numInputChannels);
    o->setProperty("numOutputs",    d.numOutputChannels);
    o->setProperty("uid",           juce::String::toHexString(d.uniqueId));
    o->setProperty("path",          d.fileOrIdentifier);
    return o;
}

class App
{
public:
    bool init()
    {
        // ── Audio device + graph ─────────────────────────────────────────────
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

        dawGraph.initialize(scanner.formatManager(), sampleRate, blockSize);

        // When DawGraph drops a slot (unload, channel teardown), close any
        // open UI for that slot.
        dawGraph.setSlotRemovedCallback(
            [this](int slotId) { windowManager.removeSlot(slotId); });

        dawPlayer.setProcessor(&dawGraph.getRootProcessor());
        deviceManager.addAudioCallback(&dawPlayer);

        // The player's MIDI collector is the way OSC events get into the
        // graph. Configure it for our sample rate. We do NOT register the
        // player as a MIDI input callback — there's no hardware MIDI source;
        // the only path into the collector is via OSC dispatch below.
        dawPlayer.getMidiMessageCollector().reset(sampleRate);

        // ── OSC dispatch → player's MIDI collector ───────────────────────────
        oscServer.onNoteOn = [this](const OscServer::NoteOn& ev) {
            const int ch  = juce::jlimit(1, 16, ev.channel);
            const int n   = juce::jlimit(0, 127, ev.note);
            const int v   = juce::jlimit(0, 127, ev.velocity);
            dawPlayer.getMidiMessageCollector()
                     .addMessageToQueue(juce::MidiMessage::noteOn(ch, n, static_cast<juce::uint8>(v)));
        };
        oscServer.onNoteOff = [this](const OscServer::NoteOff& ev) {
            const int ch = juce::jlimit(1, 16, ev.channel);
            const int n  = juce::jlimit(0, 127, ev.note);
            dawPlayer.getMidiMessageCollector()
                     .addMessageToQueue(juce::MidiMessage::noteOff(ch, n));
        };
        oscServer.onCc = [this](const OscServer::Cc& ev) {
            const int ch  = juce::jlimit(1, 16, ev.channel);
            const int cc  = juce::jlimit(0, 127, ev.controller);
            const int val = juce::jlimit(0, 127, ev.value);
            dawPlayer.getMidiMessageCollector()
                     .addMessageToQueue(juce::MidiMessage::controllerEvent(ch, cc, val));
        };

        // ── Servers ──────────────────────────────────────────────────────────
        installControlOps();

        if (! controlServer.start()) return false;
        if (! oscServer.start())     { controlServer.stop(); return false; }

        Logger::instance().setForwarder(
            [this](int level, const char* line) { controlServer.forwardLog(level, line); });

        drainTimer = std::make_unique<DrainTimer>();
        drainTimer->startTimerHz(60);

        PHOBOS_LOG("PhobosHost: ready");
        return true;
    }

    void shutdown()
    {
        if (drainTimer != nullptr) drainTimer->stopTimer();

        Logger::instance().setForwarder({});
        Logger::instance().drainOnce(Logger::kSlots);

        oscServer.stop();
        controlServer.stop();

        deviceManager.removeAudioCallback(&dawPlayer);
        dawPlayer.setProcessor(nullptr);

        dawGraph.getRootProcessor().releaseResources();
    }

private:
    void installControlOps()
    {
        controlServer.registerOp("ping", [](const juce::var&, juce::String&) {
            auto* res = new juce::DynamicObject();
            res->setProperty("uptimeMs",
                static_cast<int64_t>(juce::Time::getMillisecondCounter()));
            res->setProperty("version", "0.3.0");
            return juce::var(res);
        });

        controlServer.registerOp("shutdown", [](const juce::var&, juce::String&) {
            // We're on the message thread (per ControlServer dispatch). Setting
            // the dispatch-loop quit flag from here is reentrant-safe — the loop
            // checks the flag between iterations, not mid-callback.
            if (auto* mm = juce::MessageManager::getInstanceWithoutCreating())
                mm->stopDispatchLoop();
            return juce::var(new juce::DynamicObject());
        });

        // ── Plugin ops ───────────────────────────────────────────────────────

        controlServer.registerOp("scanVst3Path",
            [this](const juce::var& args, juce::String& errorOut) -> juce::var
        {
            const auto pathStr = args.getProperty("path", juce::var()).toString();
            if (pathStr.isEmpty()) { errorOut = "missing 'path'"; return {}; }

            const auto result = scanner.scanDirectory(juce::File(pathStr));

            juce::Array<juce::var> arr;
            for (const auto& d : result.plugins)
                arr.add(juce::var(descToJson(d)));

            auto* res = new juce::DynamicObject();
            res->setProperty("plugins",      arr);
            res->setProperty("scannedFiles", result.scannedFiles);
            res->setProperty("failedFiles",  result.failedFiles);
            return juce::var(res);
        });

        controlServer.registerOp("loadPlugin",
            [this](const juce::var& args, juce::String& errorOut) -> juce::var
        {
            const int  channelIdx = static_cast<int>(args.getProperty("channelIdx", -1));
            const auto pluginPath = args.getProperty("pluginPath", juce::var()).toString();
            const auto kindStr    = args.getProperty("kind", juce::var("instrument")).toString();
            const int  fxIndex    = static_cast<int>(args.getProperty("fxIndex", -1));

            if (channelIdx < 0)        { errorOut = "missing/invalid 'channelIdx'"; return {}; }
            if (pluginPath.isEmpty())  { errorOut = "missing 'pluginPath'";          return {}; }

            DawGraph::Kind kind;
            if      (kindStr == "instrument") kind = DawGraph::Kind::Instrument;
            else if (kindStr == "fx")         kind = DawGraph::Kind::Fx;
            else { errorOut = "invalid 'kind' (want 'instrument' or 'fx')"; return {}; }

            const auto r = dawGraph.loadPlugin(channelIdx, pluginPath, kind, fxIndex);
            if (r.error.isNotEmpty()) { errorOut = r.error; return {}; }

            auto* res = new juce::DynamicObject();
            res->setProperty("slotId", r.slotId);
            return juce::var(res);
        });

        controlServer.registerOp("unloadPlugin",
            [this](const juce::var& args, juce::String& errorOut) -> juce::var
        {
            const int slotId = static_cast<int>(args.getProperty("slotId", -1));
            if (slotId < 0) { errorOut = "missing/invalid 'slotId'"; return {}; }

            const auto err = dawGraph.unloadPlugin(slotId);
            if (err.isNotEmpty()) { errorOut = err; return {}; }
            return juce::var(new juce::DynamicObject());
        });

        controlServer.registerOp("setPluginActive",
            [this](const juce::var& args, juce::String& errorOut) -> juce::var
        {
            const int  slotId = static_cast<int>(args.getProperty("slotId", -1));
            const bool active = static_cast<bool>(args.getProperty("active", true));
            if (slotId < 0) { errorOut = "missing/invalid 'slotId'"; return {}; }

            const auto err = dawGraph.setPluginActive(slotId, active);
            if (err.isNotEmpty()) { errorOut = err; return {}; }
            return juce::var(new juce::DynamicObject());
        });

        controlServer.registerOp("reorderFx",
            [this](const juce::var& args, juce::String& errorOut) -> juce::var
        {
            const int slotId      = static_cast<int>(args.getProperty("slotId", -1));
            const int newFxIndex  = static_cast<int>(args.getProperty("newFxIndex", -1));
            if (slotId < 0) { errorOut = "missing/invalid 'slotId'"; return {}; }

            const auto err = dawGraph.reorderFx(slotId, newFxIndex);
            if (err.isNotEmpty()) { errorOut = err; return {}; }
            return juce::var(new juce::DynamicObject());
        });

        controlServer.registerOp("getPluginState",
            [this](const juce::var& args, juce::String& errorOut) -> juce::var
        {
            const int slotId = static_cast<int>(args.getProperty("slotId", -1));
            if (slotId < 0) { errorOut = "missing/invalid 'slotId'"; return {}; }

            juce::MemoryBlock state;
            const auto err = dawGraph.getPluginState(slotId, state);
            if (err.isNotEmpty()) { errorOut = err; return {}; }

            auto* res = new juce::DynamicObject();
            res->setProperty("state", state.toBase64Encoding());
            return juce::var(res);
        });

        controlServer.registerOp("setPluginState",
            [this](const juce::var& args, juce::String& errorOut) -> juce::var
        {
            const int  slotId   = static_cast<int>(args.getProperty("slotId", -1));
            const auto stateB64 = args.getProperty("state", juce::var()).toString();
            if (slotId < 0)        { errorOut = "missing/invalid 'slotId'"; return {}; }
            if (stateB64.isEmpty()){ errorOut = "missing 'state'";          return {}; }

            juce::MemoryBlock decoded;
            if (! decoded.fromBase64Encoding(stateB64))
            {
                errorOut = "invalid base64 in 'state'";
                return {};
            }

            const auto err = dawGraph.setPluginState(slotId, decoded.getData(),
                                                     static_cast<int>(decoded.getSize()));
            if (err.isNotEmpty()) { errorOut = err; return {}; }
            return juce::var(new juce::DynamicObject());
        });

        // ── UI ops ───────────────────────────────────────────────────────────
        //
        // These touch JUCE GUI objects, which are message-thread-only. The op
        // handler itself runs on the control thread, so we hop via
        // callFunctionOnMessageThread (synchronous — blocks the control
        // thread until MT runs the lambda).

        controlServer.registerOp("showPluginUi",
            [this](const juce::var& args, juce::String& errorOut) -> juce::var
        {
            const int slotId = static_cast<int>(args.getProperty("slotId", -1));
            if (slotId < 0) { errorOut = "missing/invalid 'slotId'"; return {}; }

            auto* proc = dawGraph.getProcessorForSlot(slotId);
            if (proc == nullptr) {
                errorOut = "unknown slot " + juce::String(slotId);
                return {};
            }
            const auto suffix = juce::String::formatted(" [slot %d]", slotId);
            const auto err = windowManager.show(slotId, proc, suffix);
            if (err.isNotEmpty()) { errorOut = err; return {}; }
            return juce::var(new juce::DynamicObject());
        });

        controlServer.registerOp("closePluginUi",
            [this](const juce::var& args, juce::String& errorOut) -> juce::var
        {
            const int slotId = static_cast<int>(args.getProperty("slotId", -1));
            if (slotId < 0) { errorOut = "missing/invalid 'slotId'"; return {}; }

            windowManager.hide(slotId);
            return juce::var(new juce::DynamicObject());
        });
    }

    // ── State ────────────────────────────────────────────────────────────────
    juce::AudioDeviceManager      deviceManager;
    juce::AudioProcessorPlayer    dawPlayer;

    PluginScanner                 scanner;
    DawGraph                      dawGraph;
    WindowManager                 windowManager;

    OscServer                     oscServer;
    ControlServer                 controlServer;

    struct DrainTimer : public juce::Timer
    {
        void timerCallback() override { Logger::instance().drainOnce(); }
    };
    std::unique_ptr<DrainTimer> drainTimer;
};

} // namespace phobos

int main(int /*argc*/, char* /*argv*/[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    phobos::App app;
    if (! app.init())
    {
        phobos::Logger::instance().drainOnce(phobos::Logger::kSlots);
        return 1;
    }

    juce::MessageManager::getInstance()->runDispatchLoop();

    app.shutdown();
    phobos::Logger::instance().drainOnce(phobos::Logger::kSlots);
    return 0;
}
