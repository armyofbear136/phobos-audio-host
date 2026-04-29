#pragma once

#include <atomic>
#include <cstring>
#include <functional>
#include <juce_core/juce_core.h>

namespace phobos {

// Ring-buffer logger. Single producer (audio thread), single consumer
// (message-thread timer). No allocations, no mutexes. Drop-oldest on full.
//
// Pattern lifted from PhobosCrystal's PluginProcessor.h:170-207.
class Logger
{
public:
    static constexpr int kSlots   = 128;          // power of two — required for & masking
    static constexpr int kLineLen = 192;          // fits a JSON-escaped 160-char line + slop
    static_assert((kSlots & (kSlots - 1)) == 0, "kSlots must be a power of two");

    enum class Level : int { Info = 0, Warn = 1, Error = 2 };

    // Audio-thread safe. No alloc, no mutex, no blocking.
    void push(Level level, const char* line) noexcept
    {
        const int w = writeIdx.load(std::memory_order_relaxed);
        const int r = readIdx .load(std::memory_order_acquire);
        if (w - r >= kSlots) return;              // full — drop newest

        Slot& s = ring[w & (kSlots - 1)];
        s.level = static_cast<int>(level);
        std::strncpy(s.line, line, kLineLen - 1);
        s.line[kLineLen - 1] = '\0';

        writeIdx.store(w + 1, std::memory_order_release);
    }

    // Message-thread only. Returns false when ring is empty.
    // outLine must be at least kLineLen bytes.
    bool pop(int& outLevel, char* outLine) noexcept
    {
        const int r = readIdx .load(std::memory_order_acquire);
        const int w = writeIdx.load(std::memory_order_relaxed);
        if (r == w) return false;

        const Slot& s = ring[r & (kSlots - 1)];
        outLevel = s.level;
        std::memcpy(outLine, s.line, kLineLen);

        readIdx.store(r + 1, std::memory_order_release);
        return true;
    }

    // Process-wide singleton. Created at static init, never freed.
    static Logger& instance() noexcept;

    // Set ONCE during process init, before drainOnce is ever called. Not
    // synchronized — called concurrently with drainOnce is undefined.
    // nullptr is safe (drain still goes to stderr).
    using ForwardFn = std::function<void(int level, const char* line)>;
    void setForwarder(ForwardFn fn) { forwarder = std::move(fn); }

    // Called by Main's drain timer. Drains the ring → stderr (+ forwarder
    // when set). Bounded work per tick to prevent priority inversion if the
    // audio thread is hammering us.
    int drainOnce(int maxLines = 32);

private:
    struct Slot
    {
        int  level{};
        char line[kLineLen]{};
    };

    alignas(64) Slot ring[kSlots]{};
    std::atomic<int> writeIdx { 0 };
    std::atomic<int> readIdx  { 0 };

    ForwardFn forwarder;
};

// Convenience macros — message thread only (no audio-thread safety required
// because they go through a juce::String formatter that allocates).
// For audio-thread logging, call Logger::instance().push() with a stack
// char buffer prepared by std::snprintf into a fixed-size array.
#define PHOBOS_LOG(...)  ::phobos::Logger::instance().push( \
    ::phobos::Logger::Level::Info, \
    juce::String::formatted(__VA_ARGS__).toRawUTF8())

#define PHOBOS_WARN(...) ::phobos::Logger::instance().push( \
    ::phobos::Logger::Level::Warn, \
    juce::String::formatted(__VA_ARGS__).toRawUTF8())

#define PHOBOS_ERR(...)  ::phobos::Logger::instance().push( \
    ::phobos::Logger::Level::Error, \
    juce::String::formatted(__VA_ARGS__).toRawUTF8())

} // namespace phobos
