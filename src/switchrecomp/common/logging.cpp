#include "switchrecomp/common/logging.hpp"

#include <iostream>
#include <mutex>
#include <utility>

namespace switchrecomp::logging
{
namespace
{

struct LoggerState
{
    std::mutex mutex;
    LogLevel minimum_level = LogLevel::Info;
    Sink sink;
};

LoggerState& state()
{
    static LoggerState instance;
    return instance;
}

bool is_enabled(LogLevel level, LogLevel minimum) noexcept
{
    return static_cast<int>(level) >= static_cast<int>(minimum);
}

void default_sink(const LogRecord& record)
{
    std::cerr << '[' << level_name(record.level) << "] [" << category_name(record.category) << "] "
              << record.message << '\n';
}

} // namespace

std::string_view level_name(LogLevel level) noexcept
{
    switch (level)
    {
    case LogLevel::Trace:
        return "trace";
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warning:
        return "warning";
    case LogLevel::Error:
        return "error";
    case LogLevel::Critical:
        return "critical";
    }
    return "unknown";
}

std::string_view category_name(LogCategory category) noexcept
{
    switch (category)
    {
    case LogCategory::General:
        return "general";
    case LogCategory::Format:
        return "format";
    case LogCategory::Target:
        return "target";
    case LogCategory::Memory:
        return "memory";
    case LogCategory::Analysis:
        return "analysis";
    }
    return "unknown";
}

void set_minimum_level(LogLevel level) noexcept
{
    auto& logger = state();
    std::lock_guard lock(logger.mutex);
    logger.minimum_level = level;
}

void set_sink(Sink sink)
{
    auto& logger = state();
    std::lock_guard lock(logger.mutex);
    logger.sink = std::move(sink);
}

void reset_sink()
{
    auto& logger = state();
    std::lock_guard lock(logger.mutex);
    logger.sink = {};
}

void log(LogLevel level, LogCategory category, std::string_view message)
{
    auto& logger = state();
    std::lock_guard lock(logger.mutex);
    if (!is_enabled(level, logger.minimum_level))
    {
        return;
    }

    const LogRecord record{level, category, std::string(message)};
    if (logger.sink)
    {
        logger.sink(record);
    }
    else
    {
        default_sink(record);
    }
}

} // namespace switchrecomp::logging
