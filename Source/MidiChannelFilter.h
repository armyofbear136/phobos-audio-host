#pragma once

#include <atomic>
#include <juce_audio_processors/juce_audio_processors.h>

namespace phobos {

// Sits between midiInputNode and an instrument plugin in the DAW graph.
// Lets MIDI events through only when their channel matches the configured
// filterChannel. Audio passes through untouched.
//
// One instance per instrument-bearing channel. setFilterChannel is safe to
// call from the message thread; the audio thread sees the new value on the
// next block.
class MidiChannelFilter : public juce::AudioProcessor
{
public:
    MidiChannelFilter()
        : juce::AudioProcessor(
              BusesProperties()
                  .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                  .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {}

    void setFilterChannel(int midiChannel1Based) noexcept
    {
        // Clamp to the legal MIDI range; 0 means "let everything through".
        if (midiChannel1Based < 0)  midiChannel1Based = 0;
        if (midiChannel1Based > 16) midiChannel1Based = 16;
        filterChannel.store(midiChannel1Based, std::memory_order_release);
    }

    int getFilterChannel() const noexcept
    {
        return filterChannel.load(std::memory_order_acquire);
    }

    // ── AudioProcessor overrides ─────────────────────────────────────────────
    const juce::String getName() const override            { return "MidiChannelFilter"; }
    void  prepareToPlay (double, int) override             {}
    void  releaseResources() override                       {}
    bool  acceptsMidi() const override                      { return true;  }
    bool  producesMidi() const override                     { return true;  }
    bool  isMidiEffect() const override                     { return false; }
    double getTailLengthSeconds() const override            { return 0.0;   }

    juce::AudioProcessorEditor* createEditor() override     { return nullptr; }
    bool hasEditor() const override                         { return false;   }

    int  getNumPrograms() override                          { return 1; }
    int  getCurrentProgram() override                       { return 0; }
    void setCurrentProgram (int) override                   {}
    const juce::String getProgramName (int) override        { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override  {}
    void setStateInformation (const void*, int) override    {}

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        // Accept anything reasonable. Audio is a passthrough so we don't care.
        return layouts.getMainOutputChannels() <= 2
            && layouts.getMainInputChannels()  <= 2;
    }

    void processBlock (juce::AudioBuffer<float>& /*buffer*/,
                       juce::MidiBuffer& midi) override
    {
        const int wanted = filterChannel.load(std::memory_order_acquire);
        if (wanted == 0) return;                            // pass-through

        // Filter in place: copy matching events into scratch, swap.
        scratch.clear();
        for (const auto m : midi)
        {
            const auto& msg = m.getMessage();
            if (msg.getChannel() == wanted)
                scratch.addEvent(msg, m.samplePosition);
        }
        midi.swapWith(scratch);
    }

private:
    std::atomic<int>  filterChannel { 0 };                  // 0 = no filter
    juce::MidiBuffer  scratch;                              // pre-allocated, mutated each block

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiChannelFilter)
};

} // namespace phobos
