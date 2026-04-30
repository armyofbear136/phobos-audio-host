#pragma once

#include <memory>
#include <unordered_map>
#include <vector>
#include <juce_audio_processors/juce_audio_processors.h>

#include "MidiChannelFilter.h"
#include "PluginScanner.h"

namespace phobos {

// Owns the DAW AudioProcessorGraph and all per-channel state.
//
// Topology per routed channel:
//
//   midiInputNode
//       │  (all MIDI from backend)
//       ▼
//   MidiChannelFilter (channel-1-based)
//       │  (filtered MIDI)
//       ▼
//   Instrument plugin     ← slot, kind=instrument
//       │  (audio L/R)
//       ▼
//   FX plugin 0           ← slot, kind=fx
//       │
//       ▼
//   FX plugin 1           ← slot, kind=fx
//       │
//       ▼
//   audioOutputNode
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

    // Internal helpers.
    int  allocateSlotId() noexcept { return nextSlotId++; }
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
    void rewireChannel(int channelIdx);

    // Tears down a channel completely: removes instrument + every fx +
    // the MIDI filter node. Empties the ChannelChain entry.
    void teardownChannel(int channelIdx);

    // ── State ────────────────────────────────────────────────────────────────
    juce::AudioProcessorGraph                    graph;
    juce::AudioProcessorGraph::NodeID            midiInputNodeId  {};
    juce::AudioProcessorGraph::NodeID            audioOutputNodeId{};
    juce::AudioPluginFormatManager*              formats { nullptr };
    double                                       sampleRate { 0 };
    int                                          blockSize  { 0 };

    int                                          nextSlotId { 1 };       // host-assigned, monotonic
    std::unordered_map<int, SlotInfo>            slots;                  // slotId → info
    std::unordered_map<int, ChannelChain>        channels;               // channelIdx → chain

    SlotRemovedFn                                onSlotRemoved;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DawGraph)
};

} // namespace phobos
