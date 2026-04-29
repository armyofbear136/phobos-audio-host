#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>
#include <juce_core/juce_core.h>

#include "Logger.h"

namespace phobos {

// TCP control server bound to 127.0.0.1:16332.
//
// Wire format: 4-byte big-endian length prefix, then UTF-8 JSON body.
//   request:  { "id": <int>, "op": "<name>", "args": {...} }
//   response: { "id": <int>, "ok": true,  "result": {...} }
//   error:    { "id": <int>, "ok": false, "error": "..."  }
//   event:    { "evt": "<name>", ... }
//
// Single persistent connection. New connections supersede the old one
// (matches the spec's "re-establishes if PhobosHost restarts" semantics).
class ControlServer
{
public:
    static constexpr int kPort = 16332;

    // Op handler signature. Returns the JSON result on success, or sets
    // errorOut and returns a null var on failure. Called on the control
    // thread — must not block on the audio thread.
    using OpHandler = std::function<juce::var(const juce::var& args,
                                              juce::String&  errorOut)>;

    ControlServer();
    ~ControlServer();

    // Register a handler. Override-on-collision (last writer wins) — keeps
    // op-table installation simple from Main.cpp.
    void registerOp(const juce::String& opName, OpHandler handler);

    bool start();
    void stop();

    // Send an asynchronous event to the connected client. No-op if no
    // client is connected. Thread-safe.
    void sendEvent(const juce::String& evt, const juce::DynamicObject::Ptr& payload);

    // Convenience: forward a Logger line as a `log` event. Wired into
    // Logger::setForwarder() by Main.
    void forwardLog(int level, const char* line);

    bool isRunning() const noexcept { return running.load(std::memory_order_acquire); }

private:
    void runAcceptLoop();
    void runConnectionLoop(std::unique_ptr<juce::StreamingSocket> conn);
    bool readFrame(juce::StreamingSocket& sock, std::vector<char>& buf);
    bool writeFrame(juce::StreamingSocket& sock, const juce::String& body);
    void dispatchRequest(const juce::var& request, juce::String& responseJson);

    juce::StreamingSocket            listenSocket;
    std::thread                      acceptor;
    std::atomic<bool>                running { false };
    std::atomic<bool>                quit    { false };

    std::unordered_map<std::string, OpHandler> ops;

    // Active connection. Owned by the connection thread; the acceptor
    // hands ownership over and only retains a non-owning pointer for
    // sendEvent. Guarded by connMutex.
    juce::CriticalSection            connMutex;
    juce::StreamingSocket*           activeConn = nullptr;

    // Pre-allocated frame-read scratch. Resized once on first big frame.
    std::vector<char>                readScratch;
};

} // namespace phobos
