#include "LogManager.h"
#include <filesystem>
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#endif

#include <boost/log/core.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_file_backend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/log/expressions/formatters/date_time.hpp>
#include <boost/log/support/date_time.hpp>
#include <boost/log/attributes/clock.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>

namespace fs = std::filesystem;

// 只声明 Severity 和 TimeStamp 这两个全局 attribute，Module 不通过 attribute 传输
BOOST_LOG_ATTRIBUTE_KEYWORD(severity, "Severity", trivial::severity_level)
BOOST_LOG_ATTRIBUTE_KEYWORD(timestamp, "TimeStamp", boost::posix_time::ptime)

LogManager::~LogManager()
{
    Shutdown();
}

// ---- 字符串级别转换 ----
static trivial::severity_level ParseLogLevel(std::string level_str)
{
    // 大小写不敏感
    for (auto& c : level_str) c = static_cast<char>(std::tolower(c));

    if (level_str == "trace")  return trivial::trace;
    if (level_str == "debug")  return trivial::debug;
    if (level_str == "info")   return trivial::info;
    if (level_str == "warning")return trivial::warning;
    if (level_str == "error")  return trivial::error;
    if (level_str == "fatal")  return trivial::fatal;
    if (level_str == "off")    return trivial::fatal;
    return trivial::info;
}

void LogManager::Init(const std::string& log_dir, const std::string& level_str)
{
    Init(log_dir, ParseLogLevel(level_str));
}

void LogManager::Init(const std::string& log_dir, trivial::severity_level level)
{
    if (initialized_) return;
    initialized_ = true;

#ifdef _WIN32
    SetConsoleOutputCP(65001);          // 控制台切到 UTF-8，避免中文乱码
    SetConsoleCP(65001);                // 同时设置输入编码
#endif

    fs::create_directories(log_dir);

    // ---- 添加全局时间戳属性 ----
    logging::core::get()->add_global_attribute("TimeStamp",
        attrs::local_clock());

    // ================================================================
    // 1. 控制台 sink
    // ================================================================
    {
        using console_sink_t = sinks::synchronous_sink<sinks::text_ostream_backend>;

        auto sink = boost::make_shared<console_sink_t>();
        sink->locked_backend()->add_stream(
            boost::make_shared<std::ostream>(std::cerr.rdbuf()));

        sink->set_formatter(
            expr::stream
            << expr::format_date_time<boost::posix_time::ptime>("TimeStamp",
                "%Y-%m-%d %H:%M:%S.%f")
            << " [" << severity
            << "] " << expr::smessage
        );

        sink->set_filter(severity >= level);
        logging::core::get()->add_sink(sink);
    }

    // ================================================================
    // 2. 文件 sink（每日轮转）
    // ================================================================
    {
        using file_sink_t = sinks::synchronous_sink<sinks::text_file_backend>;

        auto sink = boost::make_shared<file_sink_t>(
            keywords::file_name = log_dir + "/signaling_%Y%m%d.log",
            keywords::rotation_size = 10 * 1024 * 1024,
            keywords::time_based_rotation =
            sinks::file::rotation_at_time_point(0, 0, 0),
            keywords::min_free_space = 100 * 1024 * 1024,
            keywords::auto_flush = true
        );

        sink->set_formatter(
            expr::stream
            << expr::format_date_time<boost::posix_time::ptime>("TimeStamp",
                "%Y-%m-%d %H:%M:%S.%f")
            << " [" << severity
            << "] " << expr::smessage
        );

        sink->set_filter(severity >= level);
        logging::core::get()->add_sink(sink);
    }

    // 启动日志后打印一条确认
    LOG_INFO("log", "日志系统初始化完成，目录: {}", log_dir);
    LOG_INFO("log", "  文件格式: {}signaling_YYYYMMDD.log", log_dir + "/");
}

void LogManager::Shutdown()
{
    if (!initialized_) return;
    initialized_ = false;

    logging::core::get()->flush();
    logging::core::get()->remove_all_sinks();
}