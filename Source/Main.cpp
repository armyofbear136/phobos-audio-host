// PhobosHost — headless JUCE-based VST3 host.
// Session 2: scan VST3 directories, instantiate plugins, route MIDI through
// per-channel chains (instrument → fx → fx → out).

#include <cstdio>

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#if JUCE_WINDOWS
 #include <windows.h>                              // SetConsoleOutputCP, CP_UTF8
 #include <io.h>                                   // _setmode, _fileno
 #include <fcntl.h>                                // _O_BINARY
#endif

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
        // open UI for that slot and cancel any active sequences targeting it.
        // Auto-cancel matches the cascading-unload contract — sequences that
        // were emitting MIDI to a now-removed slot would otherwise dispatch
        // into a void slotId.
        dawGraph.setSlotRemovedCallback(
            [this](int slotId) {
                windowManager.removeSlot(slotId);
                dawGraph.getScheduler().cancelByTargetSlot(slotId);
            });

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
        drainTimer->dawGraphPtr = &dawGraph;
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

        // Single-file equivalent of scanVst3Path. The TS-side filesystem
        // scanner uses this to fill in `category` and `isInstrument` for
        // plugins whose moduleinfo.json was missing or uninformative — the
        // JUCE deep-probe (findAllTypesForFile) is the authoritative source
        // for those fields. Returns the same `plugins` array shape; one
        // .vst3 may contain multiple classes (rare, but the SDK allows it),
        // hence still an array.
        controlServer.registerOp("scanFile",
            [this](const juce::var& args, juce::String& errorOut) -> juce::var
        {
            const auto pathStr = args.getProperty("path", juce::var()).toString();
            if (pathStr.isEmpty()) { errorOut = "missing 'path'"; return {}; }

            const auto types = scanner.scanFile(juce::File(pathStr));

            juce::Array<juce::var> arr;
            for (const auto& d : types)
                arr.add(juce::var(descToJson(d)));

            auto* res = new juce::DynamicObject();
            res->setProperty("plugins", arr);
            res->setProperty("failed",  types.empty());
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

            // Channel 0's INSTRUMENT slot is reserved for the Phobos synth
            // (the system-level always-mounted instrument). Use loadPhobosSynth
            // for that. FX slots on channel 0 are deliberately open — users may
            // chain effects after the synth, which is how the Phobos Crystal
            // global FX attaches via loadPhobosCrystal too.
            if (channelIdx == 0 && kind == DawGraph::Kind::Instrument)
            {
                errorOut = "channel 0 instrument is reserved for the Phobos synth — use 'loadPhobosSynth'";
                return {};
            }

            const auto r = dawGraph.loadPlugin(channelIdx, pluginPath, kind, fxIndex);
            if (r.error.isNotEmpty()) { errorOut = r.error; return {}; }

            auto* res = new juce::DynamicObject();
            res->setProperty("slotId", r.slotId);
            return juce::var(res);
        });

        // Reserved-channel mount: the Phobos synth (system-level, always-on
        // instrument) lives on channel 0. Only this op may target channel 0
        // as an instrument. Rejected if channel 0 already has an instrument —
        // the manager owns the mount/unmount lifecycle and is expected to
        // call this exactly once per host startup.
        controlServer.registerOp("loadPhobosSynth",
            [this](const juce::var& args, juce::String& errorOut) -> juce::var
        {
            const auto pluginPath = args.getProperty("pluginPath", juce::var()).toString();
            if (pluginPath.isEmpty()) { errorOut = "missing 'pluginPath'"; return {}; }

            const auto r = dawGraph.loadPlugin(0, pluginPath, DawGraph::Kind::Instrument, -1);
            if (r.error.isNotEmpty()) { errorOut = r.error; return {}; }

            auto* res = new juce::DynamicObject();
            res->setProperty("slotId", r.slotId);
            return juce::var(res);
        });

        // Phobos Crystal global FX mount: appended to channel 0's FX chain
        // after the synth. Symmetric to loadPhobosSynth but for the global-FX
        // role. Channel 0's FX slots are not strictly reserved — anyone could
        // load FX there via 'loadPlugin' with kind=fx — but the manager uses
        // this op to keep the system-mount path expressive and discoverable.
        controlServer.registerOp("loadPhobosCrystal",
            [this](const juce::var& args, juce::String& errorOut) -> juce::var
        {
            const auto pluginPath = args.getProperty("pluginPath", juce::var()).toString();
            if (pluginPath.isEmpty()) { errorOut = "missing 'pluginPath'"; return {}; }

            // Append: fxIndex == -1 means "tail of chain", which is what we
            // want — Crystal sits at the end so any future user FX inserted
            // ahead of it are processed first, then Crystal does its global
            // pass.
            const auto r = dawGraph.loadPlugin(0, pluginPath, DawGraph::Kind::Fx, -1);
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

            // Reservation defense-in-depth: refuse to unload the Phobos synth.
            // The synth is the unique instrument on channel 0; FX on channel 0
            // (e.g. Phobos Crystal, future user-mounted master FX) are
            // unloadable like any other slot. There is intentionally no
            // unloadPhobosSynth op — the synth lives until the host process
            // exits.
            if (dawGraph.getChannelForSlot(slotId) == 0
             && dawGraph.getKindForSlot(slotId) == static_cast<int>(DawGraph::Kind::Instrument))
            {
                errorOut = "slot is the Phobos synth — refusing unload";
                return {};
            }

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

        // ── Sequencer ops ────────────────────────────────────────────────────
        //
        // playMidiSequence accepts an event list compiled from ALDA (or any
        // future tick-space MIDI source) and queues it for sample-accurate
        // playback through the host's SchedulerNode. Events are addressed at
        // a target slotId, which is resolved to its host channelIdx so the
        // outgoing MIDI lands on midiChannel = channelIdx + 1 (matching the
        // OSC convention and the per-channel MidiChannelFilter).
        //
        // Tick → sample conversion uses the device sample rate captured at
        // host startup. Tempo is single-valued per sequence (last-set BPM
        // from the ALDA emitter); mid-sequence tempo changes are deferred.
        //
        // Returns a monotonic sequenceId that callers retain for stopSequence.
        // Sequences that complete naturally retire themselves; explicit stop
        // emits a notes-off sweep for any currently sounding notes.
        controlServer.registerOp("playMidiSequence",
            [this](const juce::var& args, juce::String& errorOut) -> juce::var
        {
            const int    slotId       = static_cast<int>(args.getProperty("slotId", -1));
            const int    ticksPerBeat = static_cast<int>(args.getProperty("ticksPerBeat", 0));
            const double tempoBpm     = static_cast<double>(args.getProperty("tempoBpm", 0.0));
            const auto   eventsVar    = args.getProperty("events", juce::var());

            if (slotId < 0)        { errorOut = "missing/invalid 'slotId'"; return {}; }
            if (ticksPerBeat <= 0) { errorOut = "missing/invalid 'ticksPerBeat'"; return {}; }
            if (tempoBpm <= 0.0)   { errorOut = "missing/invalid 'tempoBpm'"; return {}; }
            if (! eventsVar.isArray()) { errorOut = "missing/invalid 'events'"; return {}; }

            // Resolve target slot → host channel. -1 means "no such slot."
            const int channelIdx = dawGraph.getChannelForSlot(slotId);
            if (channelIdx < 0) { errorOut = "unknown slot " + juce::String(slotId); return {}; }
            const int midiChannel = channelIdx + 1;       // 1-indexed

            // Tick → sample conversion. samplesPerTick = sr * 60 / (bpm * tpb).
            // Computed once; the scheduler stores absolute sample offsets.
            const double sr = dawGraph.getRootProcessor().getSampleRate();
            if (sr <= 0.0) { errorOut = "audio device sample rate not available"; return {}; }
            const double samplesPerTick = (sr * 60.0) / (tempoBpm * ticksPerBeat);

            const auto* arr = eventsVar.getArray();
            std::vector<SchedulerNode::MidiEvent> events;
            events.reserve((size_t) arr->size());

            juce::int64 lastStart = 0;
            for (const auto& evVar : *arr)
            {
                const int  midiNote      = static_cast<int>(evVar.getProperty("midiNote", -1));
                const int  velocity      = static_cast<int>(evVar.getProperty("velocity", 100));
                const int  startTicks    = static_cast<int>(evVar.getProperty("startTicks", -1));
                const int  durationTicks = static_cast<int>(evVar.getProperty("durationTicks", 0));

                if (midiNote   < 0 || midiNote   > 127) { errorOut = "event midiNote out of range";   return {}; }
                if (startTicks < 0)                     { errorOut = "event startTicks out of range"; return {}; }
                if (durationTicks <= 0)                 { errorOut = "event durationTicks must be > 0"; return {}; }

                SchedulerNode::MidiEvent ev;
                ev.midiNote        = midiNote;
                ev.velocity        = juce::jlimit(0, 127, velocity);
                ev.startSamples    = (juce::int64) (startTicks * samplesPerTick);
                ev.durationSamples = (juce::int64) (durationTicks * samplesPerTick);
                events.push_back(ev);

                // The audio thread walks events in array order and stops at the
                // first event whose startSamples >= blockEnd. That's correct
                // only if events are sorted by startSamples — verify here on
                // MT before handing off, so a malformed payload fails clearly.
                if (ev.startSamples < lastStart) {
                    errorOut = "events must be sorted ascending by startTicks";
                    return {};
                }
                lastStart = ev.startSamples;
            }

            const int sequenceId = dawGraph.getScheduler().allocateSequenceId();
            const bool queued = dawGraph.getScheduler().addSequence(
                sequenceId, slotId, midiChannel, std::move(events), sr);
            if (! queued) {
                errorOut = "scheduler command queue full — try again";
                return {};
            }

            PHOBOS_LOG("playMidiSequence: seq=%d slot=%d ch=%d events=%d bpm=%.2f tpb=%d",
                       sequenceId, slotId, midiChannel,
                       (int) arr->size(), tempoBpm, ticksPerBeat);

            auto* res = new juce::DynamicObject();
            res->setProperty("sequenceId", sequenceId);
            return juce::var(res);
        });

        controlServer.registerOp("stopSequence",
            [this](const juce::var& args, juce::String& errorOut) -> juce::var
        {
            const int sequenceId = static_cast<int>(args.getProperty("sequenceId", -1));
            if (sequenceId < 0) { errorOut = "missing/invalid 'sequenceId'"; return {}; }

            dawGraph.getScheduler().removeSequence(sequenceId);
            // Best-effort: even if the sequence already finished, this is
            // idempotent on the audio thread (no matching id → no-op).
            PHOBOS_LOG("stopSequence: seq=%d", sequenceId);
            return juce::var(new juce::DynamicObject());
        });

        // ── File-player ops ──────────────────────────────────────────────────
        //
        // playAudioFile attaches an audio file (WAV/AIFF/FLAC/OGG/MP3) to a
        // new FilePlayerNode and wires it into channel 0's audioSumNode. The
        // file decodes on a background reader thread; processBlock pulls
        // pre-buffered samples without disk I/O.
        //
        // audioIds are monotonic and never reused — same pattern as slotId
        // and sequenceId. They share no namespace with slot ids; a frontend
        // that holds an audioId in one variable and a slotId in another must
        // not confuse them, but the host never compares them.

        controlServer.registerOp("playAudioFile",
            [this](const juce::var& args, juce::String& errorOut) -> juce::var
        {
            const auto path     = args.getProperty("path", juce::var()).toString();
            const double startMs = static_cast<double>(args.getProperty("startMs", 0.0));
            const bool   loop    = static_cast<bool>(args.getProperty("loop", false));

            if (path.isEmpty()) { errorOut = "missing 'path'"; return {}; }

            const auto r = dawGraph.playAudioFile(path, startMs, loop);
            if (r.error.isNotEmpty()) { errorOut = r.error; return {}; }

            auto* res = new juce::DynamicObject();
            res->setProperty("audioId",    r.audioId);
            res->setProperty("durationMs", r.durationSec * 1000.0);
            return juce::var(res);
        });

        controlServer.registerOp("pauseAudio",
            [this](const juce::var& args, juce::String& errorOut) -> juce::var
        {
            const int audioId = static_cast<int>(args.getProperty("audioId", -1));
            if (audioId < 0) { errorOut = "missing/invalid 'audioId'"; return {}; }
            const auto err = dawGraph.pauseAudio(audioId);
            if (err.isNotEmpty()) { errorOut = err; return {}; }
            return juce::var(new juce::DynamicObject());
        });

        controlServer.registerOp("resumeAudio",
            [this](const juce::var& args, juce::String& errorOut) -> juce::var
        {
            const int audioId = static_cast<int>(args.getProperty("audioId", -1));
            if (audioId < 0) { errorOut = "missing/invalid 'audioId'"; return {}; }
            const auto err = dawGraph.resumeAudio(audioId);
            if (err.isNotEmpty()) { errorOut = err; return {}; }
            return juce::var(new juce::DynamicObject());
        });

        controlServer.registerOp("seekAudio",
            [this](const juce::var& args, juce::String& errorOut) -> juce::var
        {
            const int    audioId    = static_cast<int>(args.getProperty("audioId", -1));
            const double positionMs = static_cast<double>(args.getProperty("positionMs", 0.0));
            if (audioId < 0) { errorOut = "missing/invalid 'audioId'"; return {}; }
            const auto err = dawGraph.seekAudio(audioId, positionMs);
            if (err.isNotEmpty()) { errorOut = err; return {}; }
            return juce::var(new juce::DynamicObject());
        });

        controlServer.registerOp("stopAudio",
            [this](const juce::var& args, juce::String& errorOut) -> juce::var
        {
            const int audioId = static_cast<int>(args.getProperty("audioId", -1));
            if (audioId < 0) { errorOut = "missing/invalid 'audioId'"; return {}; }
            const auto err = dawGraph.stopAudio(audioId);
            if (err.isNotEmpty()) { errorOut = err; return {}; }
            return juce::var(new juce::DynamicObject());
        });

        controlServer.registerOp("getAudioStatus",
            [this](const juce::var& args, juce::String& errorOut) -> juce::var
        {
            const int audioId = static_cast<int>(args.getProperty("audioId", -1));
            if (audioId < 0) { errorOut = "missing/invalid 'audioId'"; return {}; }

            const auto s = dawGraph.getAudioStatus(audioId);
            if (! s.exists) { errorOut = "unknown audioId " + juce::String(audioId); return {}; }

            auto* res = new juce::DynamicObject();
            res->setProperty("playing",    s.playing);
            res->setProperty("positionMs", s.positionMs);
            res->setProperty("durationMs", s.durationMs);
            res->setProperty("finished",   s.finished);
            return juce::var(res);
        });

        controlServer.registerOp("setAudioFileVolume",
            [this](const juce::var& args, juce::String& errorOut) -> juce::var
        {
            const int   audioId = static_cast<int>(args.getProperty("audioId", -1));
            const float gain    = static_cast<float>(static_cast<double>(args.getProperty("gain", 1.0)));
            if (audioId < 0) { errorOut = "missing/invalid 'audioId'"; return {}; }
            const auto err = dawGraph.setAudioFileVolume(audioId, gain);
            if (err.isNotEmpty()) { errorOut = err; return {}; }
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
        DawGraph* dawGraphPtr { nullptr };
        void timerCallback() override
        {
            Logger::instance().drainOnce();
            if (dawGraphPtr != nullptr)
                dawGraphPtr->getScheduler().drainFinishedSequences();
        }
    };
    std::unique_ptr<DrainTimer> drainTimer;
};

} // namespace phobos

int main(int /*argc*/, char* /*argv*/[])
{
   #if JUCE_WINDOWS
    // Windows console output of UTF-8 byte streams requires TWO fixes that
    // are easy to confuse:
    //
    //   1. Console code page — `SetConsoleOutputCP(CP_UTF8)` tells the
    //      console to interpret byte sequences as UTF-8 when rendering
    //      glyphs.
    //
    //   2. CRT stream mode — by default stdout/stderr are in `_O_TEXT`
    //      mode, which on Windows causes the CRT to translate the byte
    //      stream through the process's ANSI code page BEFORE handing it
    //      to the console. The CRT also rewrites '\n' to "\r\n". For
    //      UTF-8 strings the ANSI translation step corrupts every byte
    //      whose value is >= 0x80 — concretely, a UTF-8 path like
    //      "C:\Users\armyo" gets turned into CJK soup ("㩃啜敳獲慜浲潹")
    //      because the bytes are reinterpreted as wide characters.
    //      Switching to `_O_BINARY` makes the CRT pass bytes through
    //      verbatim. Combined with the UTF-8 console code page above,
    //      the console then receives clean UTF-8 and renders it
    //      correctly.
    //
    // Both calls are idempotent and safe to issue unconditionally on
    // Windows.
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
   #endif

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
