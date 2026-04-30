#include "Logger.h"

#include <cstdio>

namespace phobos {

Logger& Logger::instance() noexcept
{
    static Logger inst;                         // C++11 thread-safe init
    return inst;
}

int Logger::drainOnce(int maxLines)
{
    char line[kLineLen];
    int  level = 0;
    int  drained = 0;

    while (drained < maxLines && pop(level, line))
    {
        const char* prefix =
            level == static_cast<int>(Level::Error) ? "[ERR]"  :
            level == static_cast<int>(Level::Warn)  ? "[WARN]" :
                                                      "[INFO]";

        std::fprintf(stderr, "%s %s\n", prefix, line);

        if (forwarder)
            forwarder(level, line);

        ++drained;
    }

    if (drained > 0)
        std::fflush(stderr);

    return drained;
}

} // namespace phobos
