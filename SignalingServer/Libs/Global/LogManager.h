#ifndef LOG_MANAGER_H
#define LOG_MANAGER_H

#include <string>
#include <memory>
#include <boost/log/trivial.hpp>
#include <boost/log/sources/severity_logger.hpp>
#include <boost/log/expressions.hpp>
#include <fmt/core.h>
#include "singletion.h"

// ---- Boost.Log 命名空间简写 ----
namespace logging = boost::log;
namespace sinks = boost::log::sinks;
namespace src = boost::log::sources;
namespace expr = boost::log::expressions;
namespace keywords = boost::log::keywords;
namespace attrs = boost::log::attributes;
namespace trivial = boost::log::trivial;

class LogManager : public Singleton<LogManager>
{
    friend class Singleton<LogManager>;

public:
    void Init(const std::string& log_dir = "logs",
        trivial::severity_level level = trivial::debug);
    void Init(const std::string& log_dir, const std::string& level_str);     // 字符串级别: trace/debug/info/warning/error
    void Shutdown();

public:
    ~LogManager();

private:
    LogManager() = default;

    bool initialized_{ false };
};

// ---- 线程本地日志记录器 ----
inline src::severity_logger<trivial::severity_level>& GetThreadLogger()
{
    static thread_local src::severity_logger<trivial::severity_level> lg;
    return lg;
}

// ---- 日志宏 ----
// 用法: LOG_INFO("auth", "user {} login success", user_id);
// 输出: 2026-07-06 16:04:22.718 [info] [auth] user 1001 login success

#define LOG_TRACE(module, fmt_str, ...)                                                    \
    BOOST_LOG_SEV(GetThreadLogger(), trivial::trace)                                       \
        << "[" << module << "] " << fmt::format(fmt_str, ##__VA_ARGS__)

#define LOG_DEBUG(module, fmt_str, ...)                                                    \
    BOOST_LOG_SEV(GetThreadLogger(), trivial::debug)                                       \
        << "[" << module << "] " << fmt::format(fmt_str, ##__VA_ARGS__)

#define LOG_INFO(module, fmt_str, ...)                                                     \
    BOOST_LOG_SEV(GetThreadLogger(), trivial::info)                                        \
        << "[" << module << "] " << fmt::format(fmt_str, ##__VA_ARGS__)

#define LOG_WARN(module, fmt_str, ...)                                                     \
    BOOST_LOG_SEV(GetThreadLogger(), trivial::warning)                                     \
        << "[" << module << "] " << fmt::format(fmt_str, ##__VA_ARGS__)

#define LOG_ERROR(module, fmt_str, ...)                                                    \
    BOOST_LOG_SEV(GetThreadLogger(), trivial::error)                                       \
        << "[" << module << "] " << fmt::format(fmt_str, ##__VA_ARGS__)

#define LOG_CRITICAL(module, fmt_str, ...)                                                 \
    BOOST_LOG_SEV(GetThreadLogger(), trivial::fatal)                                       \
        << "[" << module << "] " << fmt::format(fmt_str, ##__VA_ARGS__)

#endif // LOG_MANAGER_H