#pragma once

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>

namespace vmm {

enum class LogLevel { Debug, Info, Warning, Error };

// Thread-safe diagnostic log. Messages go to stderr when a console is
// attached and, optionally, to a log file next to the VM.
class Log final {
public:
    static void Open(const std::filesystem::path& path);
    static void SetVerbose(bool verbose) noexcept;
    static bool Verbose() noexcept;
    static void Write(LogLevel level, const char* format, ...);
    static void WriteV(LogLevel level, const char* format, va_list args);

private:
    static std::mutex mutex_;
    static std::FILE* file_;
    static bool verbose_;
};

} // namespace vmm

#define VMM_DEBUG(...) do { if (::vmm::Log::Verbose()) ::vmm::Log::Write(::vmm::LogLevel::Debug, __VA_ARGS__); } while (0)
#define VMM_INFO(...) ::vmm::Log::Write(::vmm::LogLevel::Info, __VA_ARGS__)
#define VMM_WARN(...) ::vmm::Log::Write(::vmm::LogLevel::Warning, __VA_ARGS__)
#define VMM_ERROR(...) ::vmm::Log::Write(::vmm::LogLevel::Error, __VA_ARGS__)
