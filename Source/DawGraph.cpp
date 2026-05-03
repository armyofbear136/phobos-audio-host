#include "DawGraph.h"
#include "Logger.h"

namespace phobos {

namespace {

using GraphIO = juce::AudioProcessorGraph::AudioGraphIOProcessor;

constexpr int kStereoChannels = 2;

} // namespace

DawGraph::DawGraph()  { audioFormats.registerBasicFormats(); }
DawGraph::~DawGraph()
{
    // Stop the reader thread before any file-player nodes destruct so the
    // background prefetch is joined cleanly.
    if (readerThread != nullptr) readerThread->stopThread(2000);
}

void DawGraph::initialize(juce::AudioPluginFormatManager& fmts,
                          double sr, int bs)
{
    formats    = &fmts;
    sampleRate = sr;
    blockSize  = bs;

    graph.clear();
    graph.setPlayConfigDetails(0, kStereoChannels, sr, bs);
    graph.prepareToPlay(sr, bs);

    auto midiIn  = graph.addNode(std::make_unique<GraphIO>(GraphIO::midiInputNode));
    auto audioOut= graph.addNode(std::make_unique<GraphIO>(GraphIO::audioOutputNode));

    midiInputNodeId   = midiIn  ->nodeID;
    audioOutputNodeId = audioOut->nodeID;

    // SchedulerNode is a permanent MIDI-producing node. Its MIDI output is
    // wired into every per-channel MidiChannelFilter alongside midiInputNode
    // by rewireChannel — when a channel is created (instrument loaded), the
    // scheduler joins as a sibling MIDI source for that channel.
    auto schedulerOwned = std::make_unique<SchedulerNode>();
    scheduler = schedulerOwned.get();
    auto schedulerNode = graph.addNode(std::move(schedulerOwned));
    schedulerNodeId = schedulerNode->nodeID;

    // AudioSumNode is the channel-0 mix point. Permanent — instrument and
    // any number of file players (and future game-audio sources) sum into
    // it before flowing through the channel-0 FX chain (Crystal). JUCE adds
    // multiple incoming connections automatically; the sum node itself is a
    // passthrough that exists purely as a named topology anchor.
    auto sumOwned = std::make_unique<AudioSumNode>();
    auto sumNode = graph.addNode(std::move(sumOwned));
    audioSumNodeId = sumNode->nodeID;
}

DawGraph::ChannelChain& DawGraph::chainFor(int channelIdx)
{
    return channels[channelIdx];                            // map default-constructs on miss
}

std::unique_ptr<juce::AudioProcessor>
DawGraph::instantiatePlugin(const juce::String& pluginPath, juce::String& errorOut)
{
    if (formats == nullptr)
    {
        errorOut = "DawGraph not initialized";
        return nullptr;
    }

    // Resolve the path → PluginDescription via the format manager. Each
    // VST3 file/bundle can carry multiple types; we pick the first.
    // findAllTypesForFile is virtual on AudioPluginFormat, so we don't
    // need to downcast to VST3PluginFormat — and not needing that type
    // here means DawGraph.cpp doesn't have to drag in the format-types
    // header.
    if (formats->getNumFormats() == 0)
    {
        errorOut = "no plugin formats registered";
        return nullptr;
    }
    juce::AudioPluginFormat* fmt = formats->getFormat(0);
    if (fmt == nullptr)
    {
        errorOut = "format slot 0 is null";
        return nullptr;
    }

    juce::OwnedArray<juce::PluginDescription> types;
    fmt->findAllTypesForFile(types, pluginPath);
    if (types.isEmpty())
    {
        errorOut = "no plugin types in " + pluginPath;
        return nullptr;
    }

    juce::String createErr;
    auto instance = formats->createPluginInstance(*types.getFirst(),
                                                  sampleRate, blockSize, createErr);
    if (instance == nullptr)
    {
        errorOut = "createPluginInstance failed: " + createErr;
        return nullptr;
    }

    // Request stereo I/O. If the plugin only does mono, JUCE handles the
    // downmix at connect time — we don't need to special-case here.
    instance->setPlayConfigDetails(kStereoChannels, kStereoChannels, sampleRate, blockSize);

    // We deliberately do NOT call prepareToPlay here. The graph calls it
    // when the node is added (and again on topology changes). Calling it
    // ourselves first would just duplicate work.

    return instance;
}

int DawGraph::addPluginNode(std::unique_ptr<juce::AudioProcessor> proc,
                            int channelIdx, Kind kind)
{
    auto node = graph.addNode(std::move(proc));
    const int slotId = allocateSlotId();

    SlotInfo info;
    info.nodeId     = node->nodeID;
    info.channelIdx = channelIdx;
    info.kind       = kind;
    info.active     = true;
    slots[slotId]   = info;

    return slotId;
}

void DawGraph::removePluginNode(int slotId)
{
    auto it = slots.find(slotId);
    if (it == slots.end()) return;
    graph.removeNode(it->second.nodeId);
    slots.erase(it);

    if (onSlotRemoved) onSlotRemoved(slotId);
}

juce::AudioProcessor* DawGraph::getProcessorForSlot(int slotId)
{
    auto it = slots.find(slotId);
    if (it == slots.end()) return nullptr;
    auto* node = graph.getNodeForId(it->second.nodeId);
    return node != nullptr ? node->getProcessor() : nullptr;
}

int DawGraph::getChannelForSlot(int slotId) const noexcept
{
    const auto it = slots.find(slotId);
    return it == slots.end() ? -1 : it->second.channelIdx;
}

int DawGraph::getKindForSlot(int slotId) const noexcept
{
    const auto it = slots.find(slotId);
    return it == slots.end() ? -1 : static_cast<int>(it->second.kind);
}

void DawGraph::rewireChannel(int channelIdx)
{
    auto chainIt = channels.find(channelIdx);
    if (chainIt == channels.end()) return;
    ChannelChain& chain = chainIt->second;

    // Collect every nodeId that participates in this channel.
    std::vector<juce::AudioProcessorGraph::NodeID> nodesInChannel;
    nodesInChannel.reserve(4 + chain.fxSlots.size() + filePlayers.size());

    if (chain.midiFilterNode.uid != 0)
        nodesInChannel.push_back(chain.midiFilterNode);

    if (chain.instrumentSlot >= 0)
    {
        const auto sit = slots.find(chain.instrumentSlot);
        if (sit != slots.end()) nodesInChannel.push_back(sit->second.nodeId);
    }

    for (int fxSlot : chain.fxSlots)
    {
        const auto sit = slots.find(fxSlot);
        if (sit != slots.end()) nodesInChannel.push_back(sit->second.nodeId);
    }

    // Channel 0 also owns the audioSumNode and every active file player.
    // Pull those into the per-channel cleanup so old connections to/from the
    // sum node get refreshed when this channel rewires.
    const bool isChannelZero = (channelIdx == 0);
    if (isChannelZero)
    {
        if (audioSumNodeId.uid != 0)
            nodesInChannel.push_back(audioSumNodeId);
        for (const auto& kv : filePlayers)
            nodesInChannel.push_back(kv.second.nodeId);
    }

    // Drop every existing connection touching any of those nodes.
    for (const auto& cn : graph.getConnections())
    {
        for (const auto& id : nodesInChannel)
        {
            if (cn.source.nodeID == id || cn.destination.nodeID == id)
            {
                graph.removeConnection(cn);
                break;
            }
        }
    }

    // ── MIDI wiring ──────────────────────────────────────────────────────────
    //
    // No instrument → channel is silent. Filter (if any) stays in the graph
    // but disconnected; that's fine. Note: on channel 0, we still wire the
    // file-player sources into the sum node below even with no instrument,
    // so audio playback works without an instrument loaded. (Today the synth
    // is always mounted, but the graph topology shouldn't depend on that.)
    juce::AudioProcessorGraph::NodeID instNodeId {};
    const bool hasInstrument = (chain.instrumentSlot >= 0);
    if (hasInstrument)
    {
        const auto instSlotIt = slots.find(chain.instrumentSlot);
        if (instSlotIt == slots.end()) return;
        instNodeId = instSlotIt->second.nodeId;

        // MIDI: midiInput → filter → instrument; SchedulerNode also feeds into
        // the same filter so sequence events merge with OSC events on the per-
        // channel filter input. JUCE's graph adds MIDI from multiple sources,
        // so the merge is automatic.
        if (chain.midiFilterNode.uid != 0)
        {
            graph.addConnection({{ midiInputNodeId,         juce::AudioProcessorGraph::midiChannelIndex },
                                 { chain.midiFilterNode,   juce::AudioProcessorGraph::midiChannelIndex }});
            if (schedulerNodeId.uid != 0)
                graph.addConnection({{ schedulerNodeId,    juce::AudioProcessorGraph::midiChannelIndex },
                                     { chain.midiFilterNode, juce::AudioProcessorGraph::midiChannelIndex }});
            graph.addConnection({{ chain.midiFilterNode,   juce::AudioProcessorGraph::midiChannelIndex },
                                 { instNodeId,             juce::AudioProcessorGraph::midiChannelIndex }});
        }
        else
        {
            // No filter — wire MIDI directly. (Shouldn't happen in normal flow,
            // since loadPlugin(instrument) creates the filter, but be safe.)
            graph.addConnection({{ midiInputNodeId, juce::AudioProcessorGraph::midiChannelIndex },
                                 { instNodeId,     juce::AudioProcessorGraph::midiChannelIndex }});
            if (schedulerNodeId.uid != 0)
                graph.addConnection({{ schedulerNodeId, juce::AudioProcessorGraph::midiChannelIndex },
                                     { instNodeId,     juce::AudioProcessorGraph::midiChannelIndex }});
        }
    }

    // ── Audio wiring ─────────────────────────────────────────────────────────
    //
    // Channel 0:  (instrument + every file player) → audioSumNode → fx → out.
    // Others:     instrument → fx → out  (no sum node).
    //
    // The sum node is permanent on channel 0 and wired even when no
    // instrument is loaded — file players still need a path to the FX chain.
    // If neither an instrument nor any file players exist on channel 0, the
    // sum node has no upstream connections; it sits dormant.

    juce::AudioProcessorGraph::NodeID audioSourceTail {};

    if (isChannelZero && audioSumNodeId.uid != 0)
    {
        // Wire all upstream sources into the sum node.
        if (hasInstrument)
        {
            for (int ch = 0; ch < kStereoChannels; ++ch)
                graph.addConnection({{ instNodeId,    ch }, { audioSumNodeId, ch }});
        }
        for (const auto& kv : filePlayers)
        {
            for (int ch = 0; ch < kStereoChannels; ++ch)
                graph.addConnection({{ kv.second.nodeId, ch }, { audioSumNodeId, ch }});
        }
        audioSourceTail = audioSumNodeId;
    }
    else
    {
        // Non-zero channel: silent if no instrument.
        if (! hasInstrument) return;
        audioSourceTail = instNodeId;
    }

    // Audio: tail → fx[0] → fx[1] → ... → audioOutput
    auto prevNodeId = audioSourceTail;
    for (int fxSlot : chain.fxSlots)
    {
        const auto fxIt = slots.find(fxSlot);
        if (fxIt == slots.end()) continue;
        const auto fxNodeId = fxIt->second.nodeId;

        for (int ch = 0; ch < kStereoChannels; ++ch)
            graph.addConnection({{ prevNodeId, ch }, { fxNodeId, ch }});

        prevNodeId = fxNodeId;
    }

    for (int ch = 0; ch < kStereoChannels; ++ch)
        graph.addConnection({{ prevNodeId, ch }, { audioOutputNodeId, ch }});
}

void DawGraph::teardownChannel(int channelIdx)
{
    auto it = channels.find(channelIdx);
    if (it == channels.end()) return;
    ChannelChain& chain = it->second;

    if (chain.instrumentSlot >= 0)
    {
        removePluginNode(chain.instrumentSlot);
        chain.instrumentSlot = -1;
    }

    for (int fxSlot : chain.fxSlots)
        removePluginNode(fxSlot);
    chain.fxSlots.clear();

    if (chain.midiFilterNode.uid != 0)
    {
        graph.removeNode(chain.midiFilterNode);
        chain.midiFilterNode = {};
    }

    channels.erase(it);
}

DawGraph::LoadResult DawGraph::loadPlugin(int channelIdx,
                                          const juce::String& pluginPath,
                                          Kind kind,
                                          int  fxIndex)
{
    LoadResult r;

    juce::String err;
    auto instance = instantiatePlugin(pluginPath, err);
    if (instance == nullptr) { r.error = err; return r; }

    ChannelChain& chain = chainFor(channelIdx);

    if (kind == Kind::Instrument)
    {
        // Replace any existing instrument silently (per design choice).
        if (chain.instrumentSlot >= 0)
        {
            removePluginNode(chain.instrumentSlot);
            chain.instrumentSlot = -1;
        }

        // Lazily create the channel's MIDI filter.
        if (chain.midiFilterNode.uid == 0)
        {
            auto filter = std::make_unique<MidiChannelFilter>();
            filter->setFilterChannel(channelIdx + 1);       // slot N → MIDI ch N+1 (1-indexed)
            auto node = graph.addNode(std::move(filter));
            chain.midiFilterNode = node->nodeID;
        }

        const int slotId = addPluginNode(std::move(instance), channelIdx, Kind::Instrument);
        chain.instrumentSlot = slotId;
        rewireChannel(channelIdx);

        PHOBOS_LOG("loadPlugin: instrument slot=%d ch=%d path=%s",
                   slotId, channelIdx, pluginPath.toRawUTF8());
        r.slotId = slotId;
        return r;
    }

    // kind == Fx
    const int slotId = addPluginNode(std::move(instance), channelIdx, Kind::Fx);

    if (fxIndex < 0 || fxIndex > static_cast<int>(chain.fxSlots.size()))
        chain.fxSlots.push_back(slotId);
    else
        chain.fxSlots.insert(chain.fxSlots.begin() + fxIndex, slotId);

    rewireChannel(channelIdx);

    PHOBOS_LOG("loadPlugin: fx slot=%d ch=%d idx=%d path=%s",
               slotId, channelIdx,
               (fxIndex < 0 ? static_cast<int>(chain.fxSlots.size()) - 1 : fxIndex),
               pluginPath.toRawUTF8());
    r.slotId = slotId;
    return r;
}

juce::String DawGraph::unloadPlugin(int slotId)
{
    auto sit = slots.find(slotId);
    if (sit == slots.end()) return "unknown slot " + juce::String(slotId);

    const int  channelIdx = sit->second.channelIdx;
    const Kind kind       = sit->second.kind;

    auto chainIt = channels.find(channelIdx);
    if (chainIt == channels.end())
    {
        // Orphan — slot exists but no chain. Just remove the node.
        removePluginNode(slotId);
        return {};
    }

    if (kind == Kind::Instrument)
    {
        // Cascade: tear down the entire channel.
        teardownChannel(channelIdx);
        PHOBOS_LOG("unloadPlugin: instrument slot=%d ch=%d (channel torn down)",
                   slotId, channelIdx);
        return {};
    }

    // FX: remove from chain, drop the node, rewire.
    auto& fx = chainIt->second.fxSlots;
    fx.erase(std::remove(fx.begin(), fx.end(), slotId), fx.end());
    removePluginNode(slotId);
    rewireChannel(channelIdx);

    PHOBOS_LOG("unloadPlugin: fx slot=%d ch=%d", slotId, channelIdx);
    return {};
}

juce::String DawGraph::setPluginActive(int slotId, bool active)
{
    auto sit = slots.find(slotId);
    if (sit == slots.end()) return "unknown slot " + juce::String(slotId);

    sit->second.active = active;

    auto* node = graph.getNodeForId(sit->second.nodeId);
    if (node == nullptr) return "node missing for slot " + juce::String(slotId);

    // JUCE's bypass: when bypassed, the audio passes through (FX) or output
    // is silent (instrument — no input audio to pass).
    node->setBypassed(! active);

    PHOBOS_LOG("setPluginActive: slot=%d active=%d", slotId, active ? 1 : 0);
    return {};
}

juce::String DawGraph::reorderFx(int slotId, int newFxIndex)
{
    auto sit = slots.find(slotId);
    if (sit == slots.end())              return "unknown slot " + juce::String(slotId);
    if (sit->second.kind != Kind::Fx)    return "slot is not an fx";

    const int channelIdx = sit->second.channelIdx;
    auto chainIt = channels.find(channelIdx);
    if (chainIt == channels.end())       return "no chain for channel " + juce::String(channelIdx);

    auto& fx = chainIt->second.fxSlots;
    auto cur = std::find(fx.begin(), fx.end(), slotId);
    if (cur == fx.end())                 return "slot not in channel chain";

    fx.erase(cur);
    if (newFxIndex < 0 || newFxIndex > static_cast<int>(fx.size()))
        fx.push_back(slotId);
    else
        fx.insert(fx.begin() + newFxIndex, slotId);

    rewireChannel(channelIdx);

    PHOBOS_LOG("reorderFx: slot=%d ch=%d newIdx=%d", slotId, channelIdx, newFxIndex);
    return {};
}

juce::String DawGraph::getPluginState(int slotId, juce::MemoryBlock& outState)
{
    auto sit = slots.find(slotId);
    if (sit == slots.end()) return "unknown slot " + juce::String(slotId);

    auto* node = graph.getNodeForId(sit->second.nodeId);
    if (node == nullptr || node->getProcessor() == nullptr)
        return "node missing for slot " + juce::String(slotId);

    juce::MemoryBlock tmp;
    node->getProcessor()->getStateInformation(tmp);
    outState = std::move(tmp);
    return {};
}

juce::String DawGraph::setPluginState(int slotId, const void* data, int size)
{
    auto sit = slots.find(slotId);
    if (sit == slots.end()) return "unknown slot " + juce::String(slotId);

    auto* node = graph.getNodeForId(sit->second.nodeId);
    if (node == nullptr || node->getProcessor() == nullptr)
        return "node missing for slot " + juce::String(slotId);

    node->getProcessor()->setStateInformation(data, size);
    return {};
}

// ── File-player ops ─────────────────────────────────────────────────────────

void DawGraph::ensureReaderThreadStarted()
{
    if (readerThread != nullptr) return;
    readerThread = std::make_unique<juce::TimeSliceThread>("PhobosHost-AudioReader");
    readerThread->startThread();
}

DawGraph::PlayFileResult DawGraph::playAudioFile(const juce::String& filePath,
                                                  double startMs,
                                                  bool   loop)
{
    PlayFileResult r;

    juce::File file(filePath);
    if (! file.existsAsFile())
    {
        r.error = "file not found: " + filePath;
        return r;
    }

    // Reader is owned by the AudioFormatReaderSource (which owns by the
    // FilePlayerNode's transport). createReaderFor returns nullptr if the
    // format isn't recognised — covers WAV/AIFF/FLAC/OGG/MP3 via
    // registerBasicFormats() in the constructor.
    juce::AudioFormatReader* reader = audioFormats.createReaderFor(file);
    if (reader == nullptr)
    {
        r.error = "unsupported audio format or read error: " + filePath;
        return r;
    }

    ensureReaderThreadStarted();

    auto fpOwned  = std::make_unique<FilePlayerNode>();
    FilePlayerNode* fpRaw = fpOwned.get();
    auto fpNode   = graph.addNode(std::move(fpOwned));

    const double lengthSec = fpRaw->setSourceFromReader(reader, readerThread.get());
    if (lengthSec < 0.0)
    {
        graph.removeNode(fpNode->nodeID);
        r.error = "failed to attach reader to file player";
        return r;
    }

    if (startMs > 0.0) fpRaw->setPosition(startMs / 1000.0);
    fpRaw->setLooping(loop);
    fpRaw->start();

    const int audioId = allocateAudioId();
    FilePlayerEntry entry;
    entry.nodeId = fpNode->nodeID;
    entry.node   = fpRaw;
    filePlayers[audioId] = entry;

    // Re-wire channel 0 so the new file player joins the sum.
    rewireChannel(0);

    PHOBOS_LOG("playAudioFile: audioId=%d path=%s lengthSec=%.2f loop=%d",
               audioId, filePath.toRawUTF8(), lengthSec, loop ? 1 : 0);

    r.audioId     = audioId;
    r.durationSec = lengthSec;
    return r;
}

juce::String DawGraph::pauseAudio(int audioId)
{
    auto it = filePlayers.find(audioId);
    if (it == filePlayers.end()) return "unknown audioId " + juce::String(audioId);
    if (it->second.node != nullptr) it->second.node->stop();
    return {};
}

juce::String DawGraph::resumeAudio(int audioId)
{
    auto it = filePlayers.find(audioId);
    if (it == filePlayers.end()) return "unknown audioId " + juce::String(audioId);
    if (it->second.node != nullptr) it->second.node->start();
    return {};
}

juce::String DawGraph::seekAudio(int audioId, double positionMs)
{
    auto it = filePlayers.find(audioId);
    if (it == filePlayers.end()) return "unknown audioId " + juce::String(audioId);
    if (it->second.node != nullptr) it->second.node->setPosition(positionMs / 1000.0);
    return {};
}

juce::String DawGraph::stopAudio(int audioId)
{
    auto it = filePlayers.find(audioId);
    if (it == filePlayers.end()) return "unknown audioId " + juce::String(audioId);

    // Remove the node. The graph drops every connection involving it
    // (including the source connection into audioSumNode), and the unique_ptr
    // owned by the graph destructs — which detaches the transport's reader
    // source on the message thread (we are on MT here per the dispatcher
    // contract).
    graph.removeNode(it->second.nodeId);
    filePlayers.erase(it);

    rewireChannel(0);

    PHOBOS_LOG("stopAudio: audioId=%d", audioId);
    return {};
}


DawGraph::AudioStatus DawGraph::getAudioStatus(int audioId) const
{
    AudioStatus s;
    const auto it = filePlayers.find(audioId);
    if (it == filePlayers.end()) return s;        // exists=false

    s.exists = true;
    if (const auto* fp = it->second.node)
    {
        s.playing    = fp->isPlayingNow();
        s.positionMs = fp->getCurrentPositionSec() * 1000.0;
        s.durationMs = fp->getLengthSec()          * 1000.0;
        s.finished   = fp->hasFinished();
    }
    return s;
}

juce::String DawGraph::setAudioFileVolume(int audioId, float gain)
{
    auto it = filePlayers.find(audioId);
    if (it == filePlayers.end()) return "unknown audioId " + juce::String(audioId);
    if (it->second.node != nullptr)
        it->second.node->setGain(gain);
    PHOBOS_LOG("setAudioFileVolume: audioId=%d gain=%.3f", audioId, (double) gain);
    return {};
}

} // namespace phobos
