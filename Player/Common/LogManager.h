#ifndef LOGMANAGER_H
#define LOGMANAGER_H

#include <string>
#include <mutex>
#include <cstdio>
#include <filesystem>

// ================================================================
// 全局日志管理器
//   1. 捕获 qDebug/qWarning/qCritical
//   2. 日志按日期命名：Log/2026_7_30.txt
//   3. 线程安全
// ================================================================
class LogManager
{
public:
    static void Init(const char* exe_dir = nullptr);
    static void Shutdown();
    static void Log(const char* level, const char* fmt, ...);

    static std::string GetLogFilePath() { return log_file_path_; }
    static void SetConsoleEcho(bool enable) { console_echo_ = enable; }

    // 内部接口
    static void WriteLine(const char* level, const char* text);
    static std::string MakeDailyLogPath(const std::filesystem::path& exe_dir);

private:
    static std::mutex  write_mutex_;
    static FILE* log_file_;
    static std::string log_file_path_;
    static bool        installed_;
    static bool        console_echo_;
};

#endif