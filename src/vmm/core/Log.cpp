#include "Log.h"

#include <Windows.h>
#include <share.h>

#include <chrono>
#include <ctime>

namespace vmm {

std::mutex Log::mutex_;
std::FILE* Log::file_ = nullptr;
bool Log::verbose_ = false;

void Log::Open(const std::filesystem::path& path)
{
    const std::lock_guard lock(mutex_);
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    file_ = _wfsopen(path.c_str(), L"wb", _SH_DENYNO); // Readable while the VM runs.
}

void Log::SetVerbose(bool verbose) noexcept { verbose_ = verbose; }
bool Log::Verbose() noexcept { return verbose_; }

void Log::Write(LogLevel level, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    WriteV(level, format, args);
    va_end(args);
}

void Log::WriteV(LogLevel level, const char* format, va_list args)
{
    char message[2048];
    std::vsnprintf(message, sizeof(message), format, args);

    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    std::tm local{};
    localtime_s(&local, &seconds);
    const char* tag = level == LogLevel::Debug ? "debug" :
                      level == LogLevel::Info ? "info" :
                      level == LogLevel::Warning ? "warn" : "error";

    char line[2200];
    std::snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] [%s] %s\n",
                  local.tm_hour, local.tm_min, local.tm_sec,
                  static_cast<int>(milliseconds), tag, message);

    const std::lock_guard lock(mutex_);
    if (GetConsoleWindow() != nullptr || GetStdHandle(STD_ERROR_HANDLE) != nullptr) {
        std::fputs(line, stderr);
        std::fflush(stderr);
    }
    if (file_ != nullptr) {
        std::fputs(line, file_);
        std::fflush(file_);
    }
}

} // namespace vmm
