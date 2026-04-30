#include "DawGraph.h"
#include "Logger.h"

namespace phobos {

namespace {

using GraphIO = juce::AudioProcessorGraph::AudioGraphIOProcessor;

constexpr int kStereoChannels = 2;

} // namespace

DawGraph::DawGraph() = default;
DawGraph::~DawGraph() = default;

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

void DawGraph::rewireChannel(int channelIdx)
{
    auto chainIt = channels.find(channelIdx);
    if (chainIt == channels.end()) return;
    ChannelChain& chain = chainIt->second;

    // Collect every nodeId that participates in this channel.
    std::vector<juce::AudioProcessorGraph::NodeID> nodesInChannel;
    nodesInChannel.reserve(2 + chain.fxSlots.size());

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

    // No instrument → channel is silent. Filter (if any) stays in the graph
    // but disconnected; that's fine.
    if (chain.instrumentSlot < 0) return;

    const auto instSlotIt = slots.find(chain.instrumentSlot);
    if (instSlotIt == slots.end()) return;
    const auto instNodeId = instSlotIt->second.nodeId;

    // MIDI: midiInput → filter → instrument
    if (chain.midiFilterNode.uid != 0)
    {
        graph.addConnection({{ midiInputNodeId,         juce::AudioProcessorGraph::midiChannelIndex },
                             { chain.midiFilterNode,   juce::AudioProcessorGraph::midiChannelIndex }});
        graph.addConnection({{ chain.midiFilterNode,   juce::AudioProcessorGraph::midiChannelIndex },
                             { instNodeId,             juce::AudioProcessorGraph::midiChannelIndex }});
    }
    else
    {
        // No filter — wire MIDI directly. (Shouldn't happen in normal flow,
        // since loadPlugin(instrument) creates the filter, but be safe.)
        graph.addConnection({{ midiInputNodeId, juce::AudioProcessorGraph::midiChannelIndex },
                             { instNodeId,     juce::AudioProcessorGraph::midiChannelIndex }});
    }

    // Audio: instrument → fx[0] → fx[1] → ... → audioOutput
    auto prevNodeId = instNodeId;
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

} // namespace phobos
