#pragma once

#include <atomic>
#include <thread>
#include <juce_core/juce_core.h>

namespace phobos {

// UDP/OSC listener bound to 127.0.0.1:16331.
//
// Owns a worker thread that blocks on recv. Decoded events are dispatched
// to a sink set by the owner — Session 1 dispatches into a logging stub;
// Session 2 will wire it to the DAW graph's MIDI input node.
//
// OSC wire format mirrors phobos-core/phobos/OscClient.ts but with the
// phobos-era address scheme:
//   /phobos/note_on     i i i i   slotId, midiChannel, note, velocity
//   /phobos/note_off    i i i     slotId, midiChannel, note
//   /phobos/cc          i i i i   slotId, midiChannel, controller, value
class OscServer
{
public:
    static constexpr int kPort = 16331;

    struct NoteOn  { int slotId, channel, note, velocity; };
    struct NoteOff { int slotId, channel, note;           };
    struct Cc      { int slotId, channel, controller, value; };

    // Dispatch sinks. Called from the OSC worker thread — sinks must be
    // either lock-free or post to the message thread themselves.
    // nullptr is safe (event is logged + dropped).
    std::function<void(const NoteOn&)>  onNoteOn;
    std::function<void(const NoteOff&)> onNoteOff;
    std::function<void(const Cc&)>      onCc;

    OscServer();
    ~OscServer();

    // Returns false if bind fails. Logs the reason on failure.
    bool start();
    void stop();

    bool isRunning() const noexcept { return running.load(std::memory_order_acquire); }

private:
    void runLoop();
    void handlePacket(const char* data, int size);
    bool decodeMessage(const char* data, int size);

    juce::DatagramSocket socket { false };       // false = not exclusive
    std::thread          worker;
    std::atomic<bool>    running { false };
    std::atomic<bool>    quit    { false };

    // Pre-allocated receive buffer. OSC messages over loopback never
    // exceed a few hundred bytes; 2 KB is plenty.
    static constexpr int kRecvBufSize = 2048;
    char recvBuf[kRecvBufSize]{};
};

} // namespace phobos
