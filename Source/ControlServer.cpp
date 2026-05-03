#include "ControlServer.h"

#include <cstring>

#include <juce_events/juce_events.h>

namespace phobos {

namespace {

// Big-endian uint32 read/write — matches the spec's frame header.
inline uint32_t readBE32(const char* p) noexcept
{
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    return (uint32_t(u[0]) << 24) | (uint32_t(u[1]) << 16)
         | (uint32_t(u[2]) <<  8) |  uint32_t(u[3]);
}

inline void writeBE32(char* p, uint32_t v) noexcept
{
    auto* u = reinterpret_cast<unsigned char*>(p);
    u[0] = (v >> 24) & 0xFF;
    u[1] = (v >> 16) & 0xFF;
    u[2] = (v >>  8) & 0xFF;
    u[3] =  v        & 0xFF;
}

// Bounded read — drains exactly `n` bytes or returns false on close/error.
bool readExact(juce::StreamingSocket& sock, char* dst, int n)
{
    int got = 0;
    while (got < n)
    {
        const int r = sock.read(dst + got, n - got, true);
        if (r <= 0) return false;
        got += r;
    }
    return true;
}

constexpr int kMaxFrameBytes = 8 * 1024 * 1024;   // 8 MB hard cap

} // namespace

ControlServer::ControlServer()
{
    readScratch.reserve(64 * 1024);
}

ControlServer::~ControlServer() { stop(); }

void ControlServer::registerOp(const juce::String& opName, OpHandler handler)
{
    ops[opName.toStdString()] = std::move(handler);
}

bool ControlServer::start()
{
    if (running.load(std::memory_order_acquire))
        return true;

    if (! listenSocket.createListener(kPort, "127.0.0.1"))
    {
        PHOBOS_ERR("ControlServer: failed to listen on 127.0.0.1:%d", kPort);
        return false;
    }

    quit.store(false, std::memory_order_release);
    running.store(true, std::memory_order_release);

    acceptor = std::thread([this]() { runAcceptLoop(); });

    PHOBOS_LOG("ControlServer: listening on 127.0.0.1:%d", kPort);
    return true;
}

void ControlServer::stop()
{
    if (! running.load(std::memory_order_acquire))
        return;

    quit.store(true, std::memory_order_release);
    listenSocket.close();

    {
        const juce::ScopedLock lock(connMutex);
        if (activeConn != nullptr)
            activeConn->close();
    }

    if (acceptor.joinable())
        acceptor.join();

    running.store(false, std::memory_order_release);
}

void ControlServer::runAcceptLoop()
{
    while (! quit.load(std::memory_order_acquire))
    {
        std::unique_ptr<juce::StreamingSocket> conn { listenSocket.waitForNextConnection() };
        if (conn == nullptr) break;                // listener closed

        // Supersede any existing connection.
        {
            const juce::ScopedLock lock(connMutex);
            if (activeConn != nullptr)
                activeConn->close();
        }

        // Hand ownership to a per-connection thread. Detach — the thread
        // will exit when the client disconnects.
        std::thread([this, c = std::move(conn)]() mutable {
            runConnectionLoop(std::move(c));
        }).detach();
    }
}

void ControlServer::runConnectionLoop(std::unique_ptr<juce::StreamingSocket> conn)
{
    {
        const juce::ScopedLock lock(connMutex);
        activeConn = conn.get();
    }

    PHOBOS_LOG("ControlServer: client connected");

    while (! quit.load(std::memory_order_acquire))
    {
        if (! readFrame(*conn, readScratch)) break;

        const juce::String body(readScratch.data(), readScratch.size());
        const juce::var request = juce::JSON::parse(body);

        juce::String responseJson;
        dispatchRequest(request, responseJson);

        // Serialize against any concurrent sendEvent on the same socket.
        // Without this, a [length][body] write here can be sliced apart
        // by a sendEvent's own [length][body] write, mashing two frames
        // together on the wire. connMutex covers both connection-pointer
        // identity and exclusive write access — one lock, one invariant:
        // "I have exclusive use of the active connection right now."
        bool ok;
        {
            const juce::ScopedLock lock(connMutex);
            ok = writeFrame(*conn, responseJson);
        }
        if (! ok) break;
    }

    {
        const juce::ScopedLock lock(connMutex);
        if (activeConn == conn.get())
            activeConn = nullptr;
    }

    PHOBOS_LOG("ControlServer: client disconnected");
}

bool ControlServer::readFrame(juce::StreamingSocket& sock, std::vector<char>& buf)
{
    char header[4];
    if (! readExact(sock, header, 4)) return false;

    const uint32_t len = readBE32(header);
    if (len == 0 || len > static_cast<uint32_t>(kMaxFrameBytes))
    {
        PHOBOS_WARN("ControlServer: refusing frame of %u bytes", len);
        return false;
    }

    buf.resize(len);
    return readExact(sock, buf.data(), static_cast<int>(len));
}

bool ControlServer::writeFrame(juce::StreamingSocket& sock, const juce::String& body)
{
    const int len = static_cast<int>(body.getNumBytesAsUTF8());
    char header[4];
    writeBE32(header, static_cast<uint32_t>(len));

    if (sock.write(header, 4) != 4) return false;
    if (sock.write(body.toRawUTF8(), len) != len) return false;
    return true;
}

void ControlServer::dispatchRequest(const juce::var& request, juce::String& responseJson)
{
    auto* obj = request.getDynamicObject();
    if (obj == nullptr)
    {
        // No id available — respond with a parse-error envelope.
        auto err = new juce::DynamicObject();
        err->setProperty("id", -1);
        err->setProperty("ok", false);
        err->setProperty("error", "request not an object");
        responseJson = juce::JSON::toString(juce::var(err), true);
        return;
    }

    const int id  = static_cast<int>(obj->getProperty("id"));
    const auto op = obj->getProperty("op").toString();

    auto reply = new juce::DynamicObject();
    reply->setProperty("id", id);

    auto it = ops.find(op.toStdString());
    if (it == ops.end())
    {
        reply->setProperty("ok", false);
        reply->setProperty("error", "unknown op: " + op);
        responseJson = juce::JSON::toString(juce::var(reply), true);
        return;
    }

    juce::String  errorOut;
    const auto    args   = obj->getProperty("args");

    // All op handlers run on the JUCE message thread. Most ops mutate
    // DAW graph state, plugin instances, or window manager state — none
    // of which are thread-safe. Hopping here means individual handlers
    // don't have to worry about it. Cost: one MT round-trip per op (~µs).
    juce::var result;

    struct DispatchCtx {
        const OpHandler* handler;
        const juce::var* args;
        juce::String*    errorOut;
        juce::var*       result;
    };
    DispatchCtx ctx { &it->second, &args, &errorOut, &result };

    juce::MessageManager::getInstance()->callFunctionOnMessageThread(
        [](void* p) -> void* {
            auto& c = *static_cast<DispatchCtx*>(p);
            *c.result = (*c.handler)(*c.args, *c.errorOut);
            return nullptr;
        },
        &ctx);

    if (errorOut.isNotEmpty())
    {
        reply->setProperty("ok", false);
        reply->setProperty("error", errorOut);
    }
    else
    {
        reply->setProperty("ok", true);
        reply->setProperty("result", result);
    }
    responseJson = juce::JSON::toString(juce::var(reply), true);
}

void ControlServer::sendEvent(const juce::String& evt, const juce::DynamicObject::Ptr& payload)
{
    juce::DynamicObject::Ptr env = payload;
    if (env == nullptr) env = new juce::DynamicObject();
    env->setProperty("evt", evt);

    const juce::String body = juce::JSON::toString(juce::var(env.get()), true);

    const juce::ScopedLock lock(connMutex);
    if (activeConn == nullptr) return;
    writeFrame(*activeConn, body);                // best-effort — disconnects are handled by readFrame
}

void ControlServer::forwardLog(int level, const char* line)
{
    juce::DynamicObject::Ptr payload { new juce::DynamicObject() };
    payload->setProperty("level",   level);
    payload->setProperty("message", juce::String::fromUTF8(line));
    sendEvent("log", payload);
}

} // namespace phobos
