#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace switchrecomp::logging
{

enum class LogLevel
{
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Critical,
};

enum class LogCategory
{
    General,
    Format,
    Target,
    Memory,
    Analysis,
};

struct LogRecord
{
    LogLevel level;
    LogCategory category;
    std::string message;
};

using Sink = std::function<void(const LogRecord&)>;

[[nodiscard]] std::string_view level_name(LogLevel level) noexcept;
[[nodiscard]] std::string_view category_name(LogCategory category) noexcept;
void set_minimum_level(LogLevel level) noexcept;
void set_sink(Sink sink);
void reset_sink();
void log(LogLevel level, LogCategory category, std::string_view message);

inline void log_trace(LogCategory category, std::string_view message)
{
    log(LogLevel::Trace, category, message);
}

inline void log_debug(LogCategory category, std::string_view message)
{
    log(LogLevel::Debug, category, message);
}

inline void log_info(LogCategory category, std::string_view message)
{
    log(LogLevel::Info, category, message);
}

inline void log_warning(LogCategory category, std::string_view message)
{
    log(LogLevel::Warning, category, message);
}

inline void log_error(LogCategory category, std::string_view message)
{
    log(LogLevel::Error, category, message);
}

inline void log_critical(LogCategory category, std::string_view message)
{
    log(LogLevel::Critical, category, message);
}

} // namespace switchrecomp::logging
