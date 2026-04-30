#include "OscServer.h"
#include "Logger.h"

#include <cstdio>
#include <cstring>

namespace phobos {

namespace {

// Round a raw-byte length (string content + 1 null) up to the next 4-byte
// boundary. When already aligned, no extra padding is added — matches the
// OSC 1.0 spec ("the total number of bytes is a multiple of 4").
inline int oscPaddedLen(int len) noexcept { return (len + 3) & ~3; }

// Read a length-prefixed OSC string from buf. Returns the cursor after the
// string + padding, or -1 on malformed input. Sets out to point at the
// in-buffer string (zero-terminated by the OSC framing).
int readOscString(const char* buf, int size, int cursor, const char*& out) noexcept
{
    if (cursor >= size) return -1;
    out = buf + cursor;

    // Find terminating null within the buffer.
    int end = cursor;
    while (end < size && buf[end] != '\0') ++end;
    if (end >= size) return -1;

    const int rawLen = (end - cursor) + 1;        // include null
    return cursor + oscPaddedLen(rawLen);
}

// Read big-endian int32. Returns -1 on overflow.
int readOscInt32(const char* buf, int size, int cursor, int& out) noexcept
{
    if (cursor + 4 > size) return -1;
    const auto* u = reinterpret_cast<const unsigned char*>(buf + cursor);
    out = static_cast<int>((u[0] << 24) | (u[1] << 16) | (u[2] << 8) | u[3]);
    return cursor + 4;
}

} // namespace

OscServer::OscServer() = default;

OscServer::~OscServer() { stop(); }

bool OscServer::start()
{
    if (running.load(std::memory_order_acquire))
        return true;

    if (! socket.bindToPort(kPort, "127.0.0.1"))
    {
        PHOBOS_ERR("OscServer: failed to bind 127.0.0.1:%d", kPort);
        return false;
    }

    quit.store(false, std::memory_order_release);
    running.store(true, std::memory_order_release);

    worker = std::thread([this]() { runLoop(); });

    PHOBOS_LOG("OscServer: listening on 127.0.0.1:%d", kPort);
    return true;
}

void OscServer::stop()
{
    if (! running.load(std::memory_order_acquire))
        return;

    quit.store(true, std::memory_order_release);
    socket.shutdown();                            // unblocks waitUntilReady

    if (worker.joinable())
        worker.join();

    running.store(false, std::memory_order_release);
}

void OscServer::runLoop()
{
    while (! quit.load(std::memory_order_acquire))
    {
        // Wait up to 100ms for incoming data — keeps shutdown responsive.
        const int ready = socket.waitUntilReady(true, 100);
        if (ready < 0)  break;                    // socket closed
        if (ready == 0) continue;                 // timeout — re-check quit

        juce::String   senderHost;
        int            senderPort = 0;
        const int read = socket.read(recvBuf, kRecvBufSize, false,
                                     senderHost, senderPort);
        if (read <= 0) continue;

        handlePacket(recvBuf, read);
    }
}

void OscServer::handlePacket(const char* data, int size)
{
    if (! decodeMessage(data, size))
        PHOBOS_WARN("OscServer: malformed packet (%d bytes)", size);
}

bool OscServer::decodeMessage(const char* data, int size)
{
    // OSC message: <address>\0<pad> ,<typetags>\0<pad> <args>
    const char* address = nullptr;
    int cursor = readOscString(data, size, 0, address);
    if (cursor < 0) return false;

    const char* typetags = nullptr;
    cursor = readOscString(data, size, cursor, typetags);
    if (cursor < 0) return false;

    if (typetags[0] != ',') return false;

    // Dispatch by address. Pure char* compare — addresses are short
    // and fixed; this is the hot path.
    if (std::strcmp(address, "/phobos/note_on") == 0)
    {
        if (std::strcmp(typetags, ",iiii") != 0) return false;
        NoteOn ev{};
        cursor = readOscInt32(data, size, cursor, ev.slotId);   if (cursor < 0) return false;
        cursor = readOscInt32(data, size, cursor, ev.channel);  if (cursor < 0) return false;
        cursor = readOscInt32(data, size, cursor, ev.note);     if (cursor < 0) return false;
        cursor = readOscInt32(data, size, cursor, ev.velocity); if (cursor < 0) return false;

        if (onNoteOn) onNoteOn(ev);
        else PHOBOS_LOG("note_on slot=%d ch=%d note=%d vel=%d",
                        ev.slotId, ev.channel, ev.note, ev.velocity);
        return true;
    }

    if (std::strcmp(address, "/phobos/note_off") == 0)
    {
        if (std::strcmp(typetags, ",iii") != 0) return false;
        NoteOff ev{};
        cursor = readOscInt32(data, size, cursor, ev.slotId);  if (cursor < 0) return false;
        cursor = readOscInt32(data, size, cursor, ev.channel); if (cursor < 0) return false;
        cursor = readOscInt32(data, size, cursor, ev.note);    if (cursor < 0) return false;

        if (onNoteOff) onNoteOff(ev);
        else PHOBOS_LOG("note_off slot=%d ch=%d note=%d",
                        ev.slotId, ev.channel, ev.note);
        return true;
    }

    if (std::strcmp(address, "/phobos/cc") == 0)
    {
        if (std::strcmp(typetags, ",iiii") != 0) return false;
        Cc ev{};
        cursor = readOscInt32(data, size, cursor, ev.slotId);     if (cursor < 0) return false;
        cursor = readOscInt32(data, size, cursor, ev.channel);    if (cursor < 0) return false;
        cursor = readOscInt32(data, size, cursor, ev.controller); if (cursor < 0) return false;
        cursor = readOscInt32(data, size, cursor, ev.value);      if (cursor < 0) return false;

        if (onCc) onCc(ev);
        else PHOBOS_LOG("cc slot=%d ch=%d ctl=%d val=%d",
                        ev.slotId, ev.channel, ev.controller, ev.value);
        return true;
    }

    PHOBOS_WARN("OscServer: unknown address '%s'", address);
    return false;
}

} // namespace phobos
