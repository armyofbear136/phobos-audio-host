#pragma once

#include <atomic>
#include <memory>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

namespace phobos {

// ─────────────────────────────────────────────────────────────────────────────
// FilePlayerNode — audio file playback as an AudioProcessor graph node.
//
// Wraps juce::AudioFormatReaderSource (decode) + juce::AudioTransportSource
// (transport / position / gain) and exposes them as an AudioProcessor so the
// node can be added to the DAW graph and routed into channel 0's audioSumNode.
//
// One instance per concurrent audio file. Multiple FilePlayerNodes can sum
// into the audioSumNode (JUCE's graph sums multiple inputs to a destination
// automatically). The sum then flows through the channel-0 FX chain (Crystal,
// any future user-mounted master FX) and out to the audio device.
//
// ── Real-time discipline ─────────────────────────────────────────────────────
//
// processBlock pulls samples from a transport source backed by a reader that
// reads ahead on a JUCE TimeSliceThread (configured by setSourceFromReader).
// No disk I/O happens on the audio thread; only memcpy from prefetched
// buffers.
//
// State queries (isPlaying, getCurrentPosition, getLengthInSeconds) are
// thread-safe per JUCE's AudioTransportSource contract — they return
// snapshots safe to call from the message thread while the audio thread is
// reading.
//
// ── Lifecycle ────────────────────────────────────────────────────────────────
//
// Construction is cheap (no source attached). setSourceFromReader takes
// ownership of the reader and attaches it; the source begins streaming on
// the next processBlock if start() has been called. Calling stop() halts
// transport but keeps the source attached, so resume() can pick up where it
// left off. Tearing down the node (releaseResources + destruction) detaches
// the source and stops any active reads.
// ─────────────────────────────────────────────────────────────────────────────

class FilePlayerNode : public juce::AudioProcessor
{
public:
    // ReadAhead buffer (samples) handed to AudioTransportSource. Large enough
    // that disk hiccups within a few hundred ms are absorbed; small enough
    // that the reader thread doesn't gulp memory needlessly. ~128KB stereo
    // float = ~32k samples per channel; 32768 is the JUCE convention.
    static constexpr int kReadAheadSamples = 32768;

    FilePlayerNode()
        : juce::AudioProcessor(
              BusesProperties()
                  .withInput  ("Input",  juce::AudioChannelSet::stereo(), false)
                  .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {}

    ~FilePlayerNode() override
    {
        // Detach the source before any contained members destruct. The
        // transport must release its reader source first so the reader's
        // background thread is joined cleanly.
        transport.setSource(nullptr);
        readerSource.reset();
    }

    /**
     * Attach an audio file via a pre-created reader. Ownership of the reader
     * transfers in. `readerThread` is the host's shared TimeSliceThread
     * (started before the node sees its first block); the transport uses it
     * for read-ahead so disk I/O stays off the audio thread.
     *
     * Returns the file's length in seconds, or -1.0 if the reader is invalid.
     */
    double setSourceFromReader(juce::AudioFormatReader* reader,
                               juce::TimeSliceThread*  readerThread)
    {
        if (reader == nullptr) return -1.0;

        const double fileSampleRate = reader->sampleRate;
        const double lengthSec      = reader->lengthInSamples / juce::jmax(1.0, fileSampleRate);

        // Wrap the reader. AudioFormatReaderSource owns the reader and handles
        // looping if requested; we leave loop off by default (set via setLooping).
        auto newSource = std::make_unique<juce::AudioFormatReaderSource>(reader, /*deleteReader*/ true);
        newSource->setLooping(loopRequested.load(std::memory_order_acquire));

        // Hand to the transport. setSource is documented thread-safe vs. the
        // audio callback (it swaps under an internal lock without blocking
        // long).
        transport.setSource(newSource.get(),
                            kReadAheadSamples,
                            readerThread,
                            fileSampleRate,
                            /*maxNumChannels*/ 2);

        readerSource = std::move(newSource);
        sourceLengthSec.store(lengthSec, std::memory_order_release);
        return lengthSec;
    }

    void start()                                 { transport.start(); }
    void stop()                                  { transport.stop();  }
    void setPosition(double seconds)             { transport.setPosition(juce::jmax(0.0, seconds)); }
    // gain is a linear multiplier: 1.0 = unity, 0.0 = silence.
    // AudioTransportSource applies it per-block with a one-block ramp so
    // there are no clicks on rapid changes. Safe to call from any thread.
    void setGain(float gain) noexcept            { transport.setGain(juce::jlimit(0.0f, 2.0f, gain)); }
    float getGain() const noexcept               { return transport.getGain(); }
    void setLooping(bool loop)
    {
        loopRequested.store(loop, std::memory_order_release);
        if (readerSource != nullptr) readerSource->setLooping(loop);
    }

    // ── Status queries (thread-safe, called from MT) ─────────────────────────

    bool   isPlayingNow()        const noexcept  { return transport.isPlaying(); }
    double getCurrentPositionSec() const noexcept { return transport.getCurrentPosition(); }
    double getLengthSec()        const noexcept  { return sourceLengthSec.load(std::memory_order_acquire); }
    bool   hasFinished()         const noexcept  { return transport.hasStreamFinished(); }

    // ── AudioProcessor overrides ─────────────────────────────────────────────

    const juce::String getName() const override        { return "FilePlayerNode"; }
    bool  acceptsMidi()  const override                { return false; }
    bool  producesMidi() const override                { return false; }
    bool  isMidiEffect() const override                { return false; }
    double getTailLengthSeconds() const override       { return 0.0;   }

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override                     { return false;   }

    int  getNumPrograms() override                      { return 1; }
    int  getCurrentProgram() override                   { return 0; }
    void setCurrentProgram (int) override               {}
    const juce::String getProgramName (int) override    { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override  {}
    void setStateInformation (const void*, int) override    {}

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        return layouts.getMainOutputChannels() <= 2;
    }

    void prepareToPlay (double sr, int bs) override
    {
        transport.prepareToPlay(bs, sr);
    }

    void releaseResources() override
    {
        transport.releaseResources();
    }

    void processBlock (juce::AudioBuffer<float>& buffer,
                       juce::MidiBuffer& midi) override
    {
        midi.clear();

        const int numSamples = buffer.getNumSamples();
        if (numSamples <= 0) { buffer.clear(); return; }

        // AudioSourceChannelInfo wraps the buffer into the AudioSource shape.
        // The transport pulls from the reader source (which is itself fed by
        // the read-ahead thread) and writes into our buffer. If transport is
        // stopped or no source is attached, getNextAudioBlock fills silence.
        juce::AudioSourceChannelInfo info(&buffer, 0, numSamples);
        transport.getNextAudioBlock(info);
    }

private:
    juce::AudioTransportSource                          transport;
    std::unique_ptr<juce::AudioFormatReaderSource>      readerSource;
    std::atomic<double>                                 sourceLengthSec { 0.0 };
    std::atomic<bool>                                   loopRequested   { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FilePlayerNode)
};

// ─────────────────────────────────────────────────────────────────────────────
// AudioSumNode — passthrough AudioProcessor at channel 0's mix point.
//
// Sits between (synth + file players + future game audio) and the channel-0
// FX chain. The graph automatically sums all incoming connections at this
// node's input, so we don't have to do any explicit mixing — we just receive
// the already-summed buffer and pass it through.
//
// Stereo only. Identity in / identity out. Exists purely as a named topology
// anchor that rewireChannel(0) can wire multiple sources into.
// ─────────────────────────────────────────────────────────────────────────────

class AudioSumNode : public juce::AudioProcessor
{
public:
    AudioSumNode()
        : juce::AudioProcessor(
              BusesProperties()
                  .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                  .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {}

    const juce::String getName() const override        { return "AudioSumNode"; }
    void  prepareToPlay (double, int) override         {}
    void  releaseResources() override                   {}
    bool  acceptsMidi() const override                  { return false; }
    bool  producesMidi() const override                 { return false; }
    bool  isMidiEffect() const override                 { return false; }
    double getTailLengthSeconds() const override        { return 0.0;   }

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override                     { return false;   }

    int  getNumPrograms() override                      { return 1; }
    int  getCurrentProgram() override                   { return 0; }
    void setCurrentProgram (int) override               {}
    const juce::String getProgramName (int) override    { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override  {}
    void setStateInformation (const void*, int) override    {}

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        return layouts.getMainOutputChannels() <= 2
            && layouts.getMainInputChannels()  <= 2;
    }

    void processBlock (juce::AudioBuffer<float>& /*buffer*/,
                       juce::MidiBuffer& midi) override
    {
        // Pure passthrough — the graph sums incoming connections into our
        // input buffer, which is the same buffer JUCE uses for our output.
        // No-op processBlock = identity.
        midi.clear();
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioSumNode)
};

} // namespace phobos
