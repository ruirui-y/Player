#include "LogManager.h"

#include <cstring>
#include <cstdarg>
#include <ctime>
#include <chrono>
#include <cstdio>
#include <cerrno>

// ================================================================
// Windows API（用于获取 exe 路径）
// ================================================================
#ifdef _WIN32
#include <windows.h>
#endif

// ================================================================
// Qt 头文件（用于消息处理器注册）
// ================================================================
#include <QtCore/QtGlobal>
#include <QtCore/QMessageLogContext>
#include <QtCore/QString>
#include <QtCore/QByteArray>
#include <QtCore/QFileInfo>

// ================================================================
// 静态成员初始化
// ================================================================
std::mutex  LogManager::write_mutex_;
FILE* LogManager::log_file_ = nullptr;
std::string LogManager::log_file_path_;
bool        LogManager::installed_ = false;
bool        LogManager::console_echo_ = false;

// ================================================================
// 生成当天日志文件路径：exe_dir/Log/2026_7_30.txt
// ================================================================
std::string LogManager::MakeDailyLogPath(const std::filesystem::path& exe_dir)
{
    // ---- 取当前日期 ----
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif

    char date_str[32];
    std::snprintf(date_str, sizeof(date_str), "%d_%d_%d",
        tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday);

    // ---- 用 filesystem::path 拼接，彻底告别 / \ 烦恼 ----
    std::filesystem::path log_dir = exe_dir / "Log";

    // ---- 递归创建 Log 目录 ----
    std::error_code ec;
    std::filesystem::create_directories(log_dir, ec);
    if (ec)
    {
        printf("[LogManager] 创建目录失败: %s (errno=%d)\n",
            log_dir.string().c_str(), ec.value());
    }

    // ---- 拼接最终文件名 ----
    std::filesystem::path full_path = log_dir / (std::string(date_str) + ".txt");
    return full_path.string();
}

// ================================================================
// 内部写一条带时间戳的日志行
// 统一格式：[12:34:56.789] [INFO] 内容
// 线程安全，多线程并发不会交错
// ================================================================
void LogManager::WriteLine(const char* level, const char* text)
{
    std::lock_guard<std::mutex> lock(write_mutex_);

    // ---- 取当前时间（精确到毫秒） ----
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif

    char time_str[32];
    std::strftime(time_str, sizeof(time_str), "%H:%M:%S", &tm_buf);

    // ---- 拼装完整日志行 ----
    char line[4096];
    std::snprintf(line, sizeof(line), "[%s.%03d] [%s] %s\n",
        time_str, (int)ms.count(), level, text);

    // ---- 写入文件 ----
    if (log_file_)
    {
        std::fputs(line, log_file_);
        std::fflush(log_file_);
    }

    // ---- 同时输出到控制台（服务器模式） ----
    if (console_echo_)
        std::fputs(line, stdout);
}

// ================================================================
// 手动写日志（printf 风格）
// ================================================================
void LogManager::Log(const char* level, const char* fmt, ...)
{
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    WriteLine(level, buf);
}

// ================================================================
// Qt 消息处理器
// ================================================================
static void QtMsgHandler(QtMsgType type, const QMessageLogContext& ctxt, const QString& msg)
{
    // ---- 过滤垃圾日志 ----
    if (msg.contains("Unknown property letter-spacing") ||
        msg.contains("libpng warning: iCCP") ||
        msg.contains("was already closed") ||
        msg.startsWith("Unable to find any suggestion for") ||
        msg == "attempted to send message with network family 10 (probably IPv6) on IPv4 socket")
    {
        return;
    }

    // ---- 提取文件名 ----
    QString file_name = QFileInfo(QString::fromUtf8(ctxt.file)).fileName();

    // ---- 映射日志级别 ----
    const char* level = "DBG";
    switch (type)
    {
    case QtDebugMsg:       level = "DBG";   break;
    case QtInfoMsg:        level = "INFO";  break;
    case QtWarningMsg:     level = "WARN";  break;
    case QtCriticalMsg:    level = "ERR";   break;
    case QtFatalMsg:       level = "FATAL"; break;
    default:               level = "DBG";   break;
    }

    // ---- 拼装：文件名:行号 内容 ----
    QString formatted = QString("%1:%2 %3")
        .arg(file_name)
        .arg(ctxt.line)
        .arg(msg);

    QByteArray utf8 = formatted.toUtf8();
    LogManager::WriteLine(level, utf8.constData());

    if (type == QtFatalMsg)
        abort();
}

// ================================================================
// 初始化日志系统
// ================================================================
void LogManager::Init(const char* exe_dir)
{
    if (installed_)
        return;

    // ---- 第一步：获取 exe 所在目录 ----
    std::filesystem::path base_dir;

    if (exe_dir && exe_dir[0])
    {
        // 调用方明确传入路径
        base_dir = std::filesystem::path(exe_dir);
    }
    else
    {
        // C++17 标准做法：获取当前可执行文件的规范路径
        std::error_code ec;
        base_dir = std::filesystem::canonical("/proc/self/exe", ec);  // Linux
        if (ec)
        {
#ifdef _WIN32
            // Windows 备选方案
            char module_path[MAX_PATH] = {};
            DWORD len = GetModuleFileNameA(nullptr, module_path, MAX_PATH);
            if (len > 0 && len < MAX_PATH)
            {
                base_dir = std::filesystem::path(module_path);
            }
#endif
        }
        if (base_dir.empty())
        {
            base_dir = std::filesystem::current_path();
        }
        else
        {
            // 去掉文件名，只保留目录
            base_dir = base_dir.parent_path();
        }

        printf("[LogManager] exe 所在目录: %s\n", base_dir.string().c_str());
    }

    // ---- 第二步：生成当天日志路径，尝试打开文件 ----
    // 先试 exe 目录，失败则 fallback 到当前工作目录
    log_file_path_ = MakeDailyLogPath(base_dir);
    log_file_ = std::fopen(log_file_path_.c_str(), "a");

    if (!log_file_)
    {
        printf("[LogManager] exe 目录创建失败: %s (errno=%d)\n",
            log_file_path_.c_str(), errno);

        // fallback 到当前工作目录
        std::filesystem::path fallback_dir = std::filesystem::current_path();
        log_file_path_ = MakeDailyLogPath(fallback_dir);
        log_file_ = std::fopen(log_file_path_.c_str(), "a");
    }

    if (!log_file_)
    {
        printf("[LogManager] 日志文件创建失败，仅控制台输出 (errno=%d)\n", errno);
        qInstallMessageHandler(QtMsgHandler);
        installed_ = true;
        return;
    }

    printf("[LogManager] 日志文件: %s\n", log_file_path_.c_str());

    // ---- 第三步：写入启动分隔线 ----
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        std::fprintf(log_file_, "\n");
        std::fprintf(log_file_, "========================================================\n");
        std::fprintf(log_file_, "  日志启动 - %s\n", log_file_path_.c_str());
        std::fprintf(log_file_, "========================================================\n");
        std::fflush(log_file_);
    }

    // ---- 第四步：安装 Qt 消息处理器 ----
    qInstallMessageHandler(QtMsgHandler);

    installed_ = true;

    Log("INFO", "LogManager 初始化完成，日志文件：%s", log_file_path_.c_str());
}

// ================================================================
// 关闭日志系统
// ================================================================
void LogManager::Shutdown()
{
    if (!installed_)
        return;

    qInstallMessageHandler(nullptr);

    std::lock_guard<std::mutex> lock(write_mutex_);

    if (log_file_)
    {
        std::fprintf(log_file_, "\n[日志关闭]\n");
        std::fflush(log_file_);
        std::fclose(log_file_);
        log_file_ = nullptr;
    }

    installed_ = false;
}