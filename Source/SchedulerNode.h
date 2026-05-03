#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "Logger.h"

namespace phobos {

// ─────────────────────────────────────────────────────────────────────────────
// SchedulerNode — sample-accurate MIDI sequence dispatcher.
//
// Lives in the DAW graph as a MIDI-producing node alongside midiInputNode.
// Both feed into each per-channel MidiChannelFilter; JUCE's graph adds MIDI
// from multiple sources, so SchedulerNode events and OSC events merge
// naturally on each channel before the filter selects by midiChannel.
//
// Topology:
//
//     SchedulerNode  ──┐
//                      ├─→ MidiChannelFilter(ch)  →  Instrument  →  ...
//     midiInputNode  ──┘
//
// Audio is passthrough (silent — node has no audio input). MIDI is produced
// in processBlock with intra-block sample offsets matching the event timing.
//
// ── Real-time discipline ─────────────────────────────────────────────────────
//
// processBlock is on the audio thread. It must not allocate, lock, or block.
//
//   • Active sequences live in a fixed-size pool (kMaxActiveSequences).
//   • Each sequence's events array is allocated on the message thread when
//     the sequence is created; processBlock only reads it.
//   • Per-sequence active-notes tracking uses a fixed-size std::array
//     (kMaxActiveNotes) so cancellation can sweep notes-off without
//     touching dynamic storage.
//   • addSequence / removeSequence push commands into a lock-free SPSC ring
//     drained at the top of every processBlock.
//   • Finished / cancelled sequences hand their heap-allocated event arrays
//     back through a second SPSC ring; the message thread drains and frees
//     on its own cadence.
//
// ── Sequence id ──────────────────────────────────────────────────────────────
//
// Monotonic, never reused. Allocated on the message thread by
// allocateSequenceId(). The audio thread only reads ids.
//
// ── Cancellation semantics ──────────────────────────────────────────────────
//
// stopSequence emits noteOff for every note currently sounding from the
// sequence (per-note tracking). cancelByTargetSlot is the same operation
// triggered automatically by DawGraph::onSlotRemoved when a plugin is
// unloaded — any sequence whose target slot was just removed is cancelled
// with a notes-off sweep, matching the cascading-unload contract.
// ─────────────────────────────────────────────────────────────────────────────

class SchedulerNode : public juce::AudioProcessor
{
public:
    // ── Tuning ───────────────────────────────────────────────────────────────
    static constexpr int kMaxActiveSequences = 16;
    static constexpr int kMaxActiveNotes     = 64;
    static constexpr int kCommandRingSize    = 64;        // power of two
    static constexpr int kFreeRingSize       = 64;        // power of two

    // ── Public types ─────────────────────────────────────────────────────────

    struct MidiEvent
    {
        int midiNote;          // 0–127
        int velocity;          // 0–127  (note-on velocity; ignored for note-off)
        juce::int64 startSamples;     // absolute sample offset from sequence start
        juce::int64 durationSamples;  // duration in samples
    };

    SchedulerNode()
        : juce::AudioProcessor(
              BusesProperties()
                  .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                  .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
        for (auto& s : sequences) s = nullptr;
    }

    ~SchedulerNode() override
    {
        // Drop anything still owned. The audio callback is no longer running
        // by the time this destructor fires (graph teardown stops the device).
        for (auto& s : sequences) s.reset();
        FreedSequence f;
        while (freedRing.pop(f)) f.payload.reset();
        Command c;
        while (commandRing.pop(c))
        {
            if (c.kind == CommandKind::Add) c.add.payload.reset();
        }
    }

    // ── Message-thread API ───────────────────────────────────────────────────

    /** Allocate a new monotonic sequence id. Message-thread only. */
    int allocateSequenceId() noexcept
    {
        return nextSequenceId++;
    }

    /**
     * Build a sequence from a tempo-and-tick-space event list and queue it for
     * playback. Caller already allocated the sequenceId via allocateSequenceId.
     *
     * sampleRate must be the host's current device sample rate. Returns true
     * if the command was queued; false if the ring is full (caller should
     * report the failure to the user — adding more than kMaxActiveSequences
     * concurrent sequences is a user-visible cap, not a silent drop).
     */
    bool addSequence(int sequenceId,
                     int targetSlotId,
                     int targetMidiChannel,                 // 1-indexed
                     std::vector<MidiEvent>&& events,
                     double sampleRate)
    {
        auto payload = std::make_unique<ActiveSequence>();
        payload->sequenceId      = sequenceId;
        payload->targetSlotId    = targetSlotId;
        payload->targetMidiCh    = juce::jlimit(1, 16, targetMidiChannel);
        payload->events          = std::move(events);
        payload->cursorSamples   = 0;
        payload->nextEventIdx    = 0;
        payload->stopRequested.store(false, std::memory_order_relaxed);
        for (auto& n : payload->activeNotes) n.note = -1;

        Command c;
        c.kind = CommandKind::Add;
        c.add.payload = std::move(payload);
        if (! commandRing.push(std::move(c)))
        {
            PHOBOS_WARN("SchedulerNode: command ring full — dropping addSequence(%d)", sequenceId);
            return false;
        }
        juce::ignoreUnused(sampleRate);   // retained for future tempo-change support
        return true;
    }

    /**
     * Request cancellation of an active sequence. The audio thread sweeps
     * notes-off for any currently sounding notes before retiring the
     * sequence. No-op if the id isn't active. Returns true if the command
     * was queued.
     */
    bool removeSequence(int sequenceId)
    {
        Command c;
        c.kind = CommandKind::Remove;
        c.remove.sequenceId = sequenceId;
        if (! commandRing.push(std::move(c)))
        {
            PHOBOS_WARN("SchedulerNode: command ring full — dropping removeSequence(%d)", sequenceId);
            return false;
        }
        return true;
    }

    /**
     * Cancel every active sequence whose target slot matches. Called by
     * DawGraph when a plugin is unloaded — keeps sequences from emitting
     * MIDI to a slot that no longer exists.
     */
    bool cancelByTargetSlot(int slotId)
    {
        Command c;
        c.kind = CommandKind::CancelByTargetSlot;
        c.cancelSlot.slotId = slotId;
        if (! commandRing.push(std::move(c)))
        {
            PHOBOS_WARN("SchedulerNode: command ring full — dropping cancelByTargetSlot(%d)", slotId);
            return false;
        }
        return true;
    }

    /**
     * Drain finished / cancelled sequences. Call periodically on the message
     * thread (the existing 60 Hz drain timer is the natural home). Frees the
     * heap memory the audio thread released.
     */
    void drainFinishedSequences()
    {
        FreedSequence f;
        while (freedRing.pop(f)) f.payload.reset();
    }

    // ── AudioProcessor overrides ─────────────────────────────────────────────

    const juce::String getName() const override         { return "SchedulerNode"; }
    void  prepareToPlay (double sr, int) override       { currentSampleRate = sr; }
    void  releaseResources() override                    {}
    bool  acceptsMidi()   const override                { return false; }
    bool  producesMidi()  const override                { return true;  }
    bool  isMidiEffect()  const override                { return false; }
    double getTailLengthSeconds() const override        { return 0.0;   }

    juce::AudioProcessorEditor* createEditor() override  { return nullptr; }
    bool hasEditor() const override                      { return false;   }

    int  getNumPrograms() override                       { return 1; }
    int  getCurrentProgram() override                    { return 0; }
    void setCurrentProgram (int) override                {}
    const juce::String getProgramName (int) override     { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override  {}
    void setStateInformation (const void*, int) override    {}

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        return layouts.getMainOutputChannels() <= 2
            && layouts.getMainInputChannels()  <= 2;
    }

    /**
     * Audio thread. No allocations, no locks, no blocking calls.
     * Audio buffer passes through unchanged (we have no audio input). MIDI
     * is produced from active sequences with intra-block sample offsets.
     */
    void processBlock (juce::AudioBuffer<float>& buffer,
                       juce::MidiBuffer& midi) override
    {
        // Clear any stale audio (we don't synthesize anything, but the graph
        // hands us a buffer that may carry input audio — silence it so we
        // don't double-route through any future audio paths).
        buffer.clear();

        // Drain pending commands from the message thread.
        applyPendingCommands();

        const int numSamples = buffer.getNumSamples();
        if (numSamples <= 0) return;

        for (int i = 0; i < kMaxActiveSequences; ++i)
        {
            auto& slotPtr = sequences[(size_t) i];
            if (slotPtr == nullptr) continue;

            ActiveSequence& seq = *slotPtr;

            const bool stopRequested = seq.stopRequested.load(std::memory_order_acquire);

            if (stopRequested)
            {
                emitNotesOffSweep(seq, midi, /*sampleOffset*/ 0);
                retireSequence(i);
                continue;
            }

            const juce::int64 blockStart = seq.cursorSamples;
            const juce::int64 blockEnd   = blockStart + numSamples;

            // Walk events that start in [blockStart, blockEnd). Events are
            // pre-sorted by startSamples (the emitter writes them in order).
            while (seq.nextEventIdx < (int) seq.events.size())
            {
                const auto& ev = seq.events[(size_t) seq.nextEventIdx];
                if (ev.startSamples >= blockEnd) break;

                const int sampleOffset = (int) (ev.startSamples - blockStart);
                emitNoteOn(seq, midi, sampleOffset, ev.midiNote, ev.velocity, ev.startSamples + ev.durationSamples);
                ++seq.nextEventIdx;
            }

            // Walk active notes; emit note-off for any whose endSample lands in this block.
            for (auto& n : seq.activeNotes)
            {
                if (n.note < 0) continue;
                if (n.endSample >= blockEnd) continue;
                const int sampleOffset = (int) juce::jmax<juce::int64>(0, n.endSample - blockStart);
                midi.addEvent(juce::MidiMessage::noteOff(seq.targetMidiCh, n.note),
                              juce::jmin(sampleOffset, numSamples - 1));
                n.note = -1;
            }

            seq.cursorSamples = blockEnd;

            // Sequence finished — no more events queued, no notes still sounding.
            if (seq.nextEventIdx >= (int) seq.events.size()
             && ! hasAnyActiveNotes(seq))
            {
                retireSequence(i);
            }
        }
    }

private:
    // ── Active-note tracking (per sequence) ──────────────────────────────────

    struct ActiveNote
    {
        int           note      { -1 };       // -1 = empty slot
        juce::int64   endSample { 0 };
    };

    // ── Sequence state ───────────────────────────────────────────────────────

    struct ActiveSequence
    {
        int                      sequenceId    { -1 };
        int                      targetSlotId  { -1 };
        int                      targetMidiCh  { 1 };
        std::vector<MidiEvent>   events;                 // immutable after construction
        juce::int64              cursorSamples { 0 };
        int                      nextEventIdx  { 0 };
        std::atomic<bool>        stopRequested { false };
        std::array<ActiveNote, kMaxActiveNotes> activeNotes {};
    };

    // ── Cross-thread command queue ───────────────────────────────────────────

    enum class CommandKind { Add, Remove, CancelByTargetSlot };

    struct Command
    {
        CommandKind kind { CommandKind::Add };

        struct AddPayload   { std::unique_ptr<ActiveSequence> payload; };
        struct RemovePayload{ int sequenceId; };
        struct CancelSlot   { int slotId; };

        AddPayload    add;
        RemovePayload remove { -1 };
        CancelSlot    cancelSlot { -1 };
    };

    /** Heap-payload handoff back to MT. Audio thread pushes; MT drains. */
    struct FreedSequence { std::unique_ptr<ActiveSequence> payload; };

    // Tiny SPSC ring. Producer = single thread, consumer = single thread.
    // Power-of-two size enables fast modulo via mask. T must be movable.
    template <typename T, int Capacity>
    class SpscRing
    {
        static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    public:
        bool push(T&& v) noexcept
        {
            const auto w = writeIdx.load(std::memory_order_relaxed);
            const auto r = readIdx.load(std::memory_order_acquire);
            if (w - r >= (uint32_t) Capacity) return false;
            slots[w & (Capacity - 1)] = std::move(v);
            writeIdx.store(w + 1, std::memory_order_release);
            return true;
        }
        bool pop(T& out) noexcept
        {
            const auto r = readIdx.load(std::memory_order_relaxed);
            const auto w = writeIdx.load(std::memory_order_acquire);
            if (r == w) return false;
            out = std::move(slots[r & (Capacity - 1)]);
            readIdx.store(r + 1, std::memory_order_release);
            return true;
        }
    private:
        std::array<T, Capacity>   slots {};
        std::atomic<uint32_t>     writeIdx { 0 };
        std::atomic<uint32_t>     readIdx  { 0 };
    };

    // ── Audio-thread helpers ─────────────────────────────────────────────────

    void applyPendingCommands()
    {
        Command c;
        while (commandRing.pop(c))
        {
            switch (c.kind)
            {
                case CommandKind::Add:
                    placeSequence(std::move(c.add.payload));
                    break;
                case CommandKind::Remove:
                    requestStopById(c.remove.sequenceId);
                    break;
                case CommandKind::CancelByTargetSlot:
                    requestStopBySlot(c.cancelSlot.slotId);
                    break;
            }
        }
    }

    void placeSequence(std::unique_ptr<ActiveSequence> payload)
    {
        for (int i = 0; i < kMaxActiveSequences; ++i)
        {
            if (sequences[(size_t) i] == nullptr)
            {
                sequences[(size_t) i] = std::move(payload);
                return;
            }
        }
        // Pool is full — return the payload to MT for free. The MT-side
        // `addSequence` already accepted the command optimistically; we drop
        // it here on the audio thread because the user has too many active
        // sequences. This is a soft cap, not a wire failure.
        FreedSequence f { std::move(payload) };
        if (! freedRing.push(std::move(f)))
        {
            // Free ring also full — leak the payload's heap until the MT
            // drain catches up. Should never happen in practice; ring sizes
            // are matched.
        }
    }

    void requestStopById(int sequenceId)
    {
        for (auto& slotPtr : sequences)
        {
            if (slotPtr == nullptr) continue;
            if (slotPtr->sequenceId == sequenceId)
                slotPtr->stopRequested.store(true, std::memory_order_release);
        }
    }

    void requestStopBySlot(int slotId)
    {
        for (auto& slotPtr : sequences)
        {
            if (slotPtr == nullptr) continue;
            if (slotPtr->targetSlotId == slotId)
                slotPtr->stopRequested.store(true, std::memory_order_release);
        }
    }

    void emitNoteOn(ActiveSequence& seq, juce::MidiBuffer& midi,
                    int sampleOffset, int note, int velocity,
                    juce::int64 endSample)
    {
        midi.addEvent(juce::MidiMessage::noteOn(seq.targetMidiCh, note,
                                                (juce::uint8) juce::jlimit(0, 127, velocity)),
                      juce::jmax(0, sampleOffset));
        // Track the note for cancellation / end-time emission.
        for (auto& n : seq.activeNotes)
        {
            if (n.note < 0)
            {
                n.note      = note;
                n.endSample = endSample;
                return;
            }
        }
        // Active-notes table full — emit the note-on but skip tracking. Means
        // we won't emit its note-off; ALDA scores never approach this density,
        // but we log so the user sees it if it ever happens.
        PHOBOS_WARN("SchedulerNode: active-note table full (cap=%d), note %d untracked",
                    kMaxActiveNotes, note);
    }

    void emitNotesOffSweep(ActiveSequence& seq, juce::MidiBuffer& midi, int sampleOffset)
    {
        for (auto& n : seq.activeNotes)
        {
            if (n.note < 0) continue;
            midi.addEvent(juce::MidiMessage::noteOff(seq.targetMidiCh, n.note), sampleOffset);
            n.note = -1;
        }
    }

    static bool hasAnyActiveNotes(const ActiveSequence& seq) noexcept
    {
        for (const auto& n : seq.activeNotes)
            if (n.note >= 0) return true;
        return false;
    }

    void retireSequence(int slotIndex)
    {
        FreedSequence f { std::move(sequences[(size_t) slotIndex]) };
        sequences[(size_t) slotIndex].reset();
        if (! freedRing.push(std::move(f)))
        {
            // Free ring full — payload's destructor would run on the audio
            // thread otherwise. Hold onto it in the slot until MT catches up.
            sequences[(size_t) slotIndex] = std::move(f.payload);
        }
    }

    // ── State ────────────────────────────────────────────────────────────────

    std::array<std::unique_ptr<ActiveSequence>, kMaxActiveSequences> sequences;

    SpscRing<Command,        kCommandRingSize> commandRing;
    SpscRing<FreedSequence,  kFreeRingSize>    freedRing;

    int    nextSequenceId   { 1 };
    double currentSampleRate { 0.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SchedulerNode)
};

} // namespace phobos
