#pragma once

#include <memory>
#include <unordered_map>
#include <vector>
#include <juce_audio_processors/juce_audio_processors.h>

#include "MidiChannelFilter.h"
#include "PluginScanner.h"
#include "SchedulerNode.h"
#include "FilePlayerNode.h"

namespace phobos {

// Owns the DAW AudioProcessorGraph and all per-channel state.
//
// Topology per routed channel:
//
//   midiInputNode (OSC, etc.)        SchedulerNode (sequence playback)
//       │                                 │
//       └──────────────┬──────────────────┘
//                      │  (MIDI from both sources merges in JUCE's graph)
//                      ▼
//                  MidiChannelFilter (channel-1-based)
//                      │  (filtered MIDI)
//                      ▼
//                  Instrument plugin     ← slot, kind=instrument
//                      │  (audio L/R)
//                      ▼
//                  FX plugin 0           ← slot, kind=fx
//                      │
//                      ▼
//                  FX plugin 1           ← slot, kind=fx
//                      │
//                      ▼
//                  audioOutputNode
//
// Slots are host-assigned, monotonically increasing, never reused. The
// (channelIdx → ChannelChain) map is the source of truth for ordering;
// rewireChannel() reads it and writes connections.
class DawGraph
{
public:
    enum class Kind { Instrument, Fx };

    struct LoadResult
    {
        int          slotId  { -1 };
        juce::String error;                                 // empty on success
    };

    DawGraph();
    ~DawGraph();

    // Must be called before any plugin op. Builds the I/O nodes and the
    // ProcessorPlayer wiring.
    void initialize(juce::AudioPluginFormatManager& formats,
                    double sampleRate, int blockSize);

    juce::AudioProcessor& getRootProcessor() noexcept { return graph; }

    // ── Op surface ───────────────────────────────────────────────────────────
    //
    // All ops run on the message thread. Errors are returned via the
    // result struct's `error` field (empty = success).

    LoadResult loadPlugin(int channelIdx,
                          const juce::String& pluginPath,
                          Kind kind,
                          int  fxIndex /* -1 = append, ignored for instrument */);

    juce::String unloadPlugin(int slotId);
    juce::String setPluginActive(int slotId, bool active);
    juce::String reorderFx(int slotId, int newFxIndex);

    // State serialization. Returns empty string on success and writes to
    // `outState`; on failure, returns the error and `outState` is untouched.
    juce::String getPluginState(int slotId, juce::MemoryBlock& outState);
    juce::String setPluginState(int slotId, const void* data, int size);

    // ── Hooks ────────────────────────────────────────────────────────────────
    //
    // Set ONCE at init, before any plugin op runs. Called on the message
    // thread immediately after a slot's node leaves the graph. Subscribers
    // (currently: WindowManager) use it to clean up per-slot resources.
    using SlotRemovedFn = std::function<void(int slotId)>;
    void setSlotRemovedCallback(SlotRemovedFn fn) { onSlotRemoved = std::move(fn); }

    // Slot lookup for ops that need direct AudioProcessor access (PluginUi).
    juce::AudioProcessor* getProcessorForSlot(int slotId);

    // Slot lookup for ops that need to know which channel a slot lives on
    // (e.g., enforcing channel-0 reservation policy at the op-handler edge).
    // Returns -1 if the slot does not exist.
    int getChannelForSlot(int slotId) const noexcept;

    // Slot lookup for ops that need to know whether a slot is an instrument
    // or an fx (e.g., distinguishing the Phobos synth slot — which is the
    // only instrument allowed on channel 0 — from user-mountable fx slots
    // that may also live on channel 0). Returns -1 if the slot does not exist;
    // 0 for Instrument, 1 for Fx (matching the Kind enum's underlying values).
    int getKindForSlot(int slotId) const noexcept;

    // Access to the embedded SchedulerNode. Owned by DawGraph; lifetime tied
    // to the DAW graph itself. Used by the playMidiSequence / stopSequence
    // op handlers to queue and cancel MIDI sequences.
    //
    // The scheduler is wired into the graph in initialize(): its MIDI output
    // feeds every per-channel MidiChannelFilter alongside midiInputNode, so
    // sequence events and OSC events merge naturally before per-channel
    // routing.
    SchedulerNode& getScheduler() noexcept { return *scheduler; }

    // ── File-player ops ──────────────────────────────────────────────────────
    //
    // File players live on channel 0 alongside the synth, summing into the
    // channel-0 audioSumNode before the FX chain (Crystal). They have their
    // own monotonic audioId space — separate from slotIds — because they
    // aren't VST3 plugins and don't share lifecycle with instruments/FX.
    //
    // Multiple file players can run concurrently; each one is a graph node
    // wired into audioSumNode. JUCE sums incoming connections automatically.
    //
    // The audio file decode runs on the host's shared TimeSliceThread (kept
    // alive for the lifetime of the host); processBlock pulls from the
    // pre-buffered transport without touching disk.

    struct PlayFileResult
    {
        int           audioId      { -1 };
        double        durationSec  { 0.0 };
        juce::String  error;                                 // empty on success
    };

    /** Open `filePath`, attach to a new FilePlayerNode, wire to channel 0's
     *  audioSumNode, and start playback. `startMs` >= 0 seeks before start;
     *  `loop` enables seamless looping. Returns the audioId for subsequent
     *  pause/resume/seek/stop calls.
     */
    PlayFileResult playAudioFile(const juce::String& filePath,
                                 double startMs,
                                 bool   loop);

    juce::String pauseAudio          (int audioId);
    juce::String resumeAudio         (int audioId);
    juce::String seekAudio           (int audioId, double positionMs);
    juce::String stopAudio           (int audioId);

    /** Set playback gain for a file player. gain 1.0 = unity, 0.0 = silence.
     *  Clamped to [0.0, 2.0]. Thread-safe — AudioTransportSource applies the
     *  gain smoothly inside processBlock without locking. */
    juce::String setAudioFileVolume  (int audioId, float gain);

    struct AudioStatus
    {
        bool   playing      { false };
        double positionMs   { 0.0 };
        double durationMs   { 0.0 };
        bool   finished     { false };
        bool   exists       { false };       // false → audioId not known
    };
    AudioStatus getAudioStatus(int audioId) const;

private:
    // Per-slot bookkeeping.
    struct SlotInfo
    {
        juce::AudioProcessorGraph::NodeID nodeId;
        int   channelIdx { -1 };
        Kind  kind       { Kind::Fx };
        bool  active     { true };
    };

    // Per-channel chain. Instrument is optional; fx is ordered.
    struct ChannelChain
    {
        int                                    instrumentSlot { -1 };
        juce::AudioProcessorGraph::NodeID      midiFilterNode {};       // null until first instrument loaded
        std::vector<int>                       fxSlots;
    };

    // Per-file-player bookkeeping. File players are kept separate from slots
    // because they have a different addressing space (audioId vs slotId) and
    // different lifecycle hooks (no UI, no plugin state, no enumeration).
    struct FilePlayerEntry
    {
        juce::AudioProcessorGraph::NodeID nodeId;
        FilePlayerNode*                   node { nullptr };       // alias; node owned by graph
    };

    // Internal helpers.
    int  allocateSlotId() noexcept    { return nextSlotId++; }
    int  allocateAudioId() noexcept   { return nextAudioId++; }
    ChannelChain& chainFor(int channelIdx);                              // creates if missing

    // Constructs a plugin instance synchronously. Returns nullptr + sets
    // errorOut on failure.
    std::unique_ptr<juce::AudioProcessor>
        instantiatePlugin(const juce::String& pluginPath, juce::String& errorOut);

    // Adds a node (instrument or FX), records SlotInfo, returns slotId.
    int addPluginNode(std::unique_ptr<juce::AudioProcessor> proc,
                      int channelIdx, Kind kind);

    // Removes a node and its SlotInfo. Does NOT touch ChannelChain.
    void removePluginNode(int slotId);

    // Recomputes all connections for one channel from its ChannelChain.
    // Drops every existing connection touching the channel's nodes, then
    // adds the canonical chain wiring. Idempotent.
    //
    // Channel 0 is special: it has the audioSumNode permanently mounted, and
    // the wiring is (instrument + every file-player) → audioSumNode → fx
    // chain → audioOutput. Other channels skip the sum node.
    void rewireChannel(int channelIdx);

    // Tears down a channel completely: removes instrument + every fx +
    // the MIDI filter node. Empties the ChannelChain entry.
    void teardownChannel(int channelIdx);

    // Lazily start the audio reader thread. Called by playAudioFile on first
    // use — the thread runs until the host shuts down. Cheaper than spinning
    // it up at startup when most installs never play files.
    void ensureReaderThreadStarted();

    // ── State ────────────────────────────────────────────────────────────────
    juce::AudioProcessorGraph                    graph;
    juce::AudioProcessorGraph::NodeID            midiInputNodeId  {};
    juce::AudioProcessorGraph::NodeID            audioOutputNodeId{};
    juce::AudioProcessorGraph::NodeID            schedulerNodeId  {};
    juce::AudioProcessorGraph::NodeID            audioSumNodeId   {};
    SchedulerNode*                               scheduler        { nullptr };  // owned by graph
    juce::AudioPluginFormatManager*              formats { nullptr };
    double                                       sampleRate { 0 };
    int                                          blockSize  { 0 };

    int                                          nextSlotId  { 1 };      // host-assigned, monotonic
    int                                          nextAudioId { 1 };      // file-player addressing
    std::unordered_map<int, SlotInfo>            slots;                  // slotId → info
    std::unordered_map<int, ChannelChain>        channels;               // channelIdx → chain
    std::unordered_map<int, FilePlayerEntry>     filePlayers;            // audioId → entry

    // Audio file decoding. AudioFormatManager registers WAV/AIFF/FLAC/OGG/MP3
    // via registerBasicFormats(); the reader thread does the off-audio-thread
    // prefetch for AudioTransportSource.
    juce::AudioFormatManager                     audioFormats;
    std::unique_ptr<juce::TimeSliceThread>       readerThread;

    SlotRemovedFn                                onSlotRemoved;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DawGraph)
};

} // namespace phobos
