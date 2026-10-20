#ifndef VLAN_CLI_LOG_H
#define VLAN_CLI_LOG_H

#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>
#include <atomic>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace VLan {

extern bool g_verboseLog;

enum LogLevel { LOG_LVL_ERROR = 0, LOG_LVL_INFO = 1, LOG_LVL_DETAIL = 2 };

class CliLog {
public:
    static CliLog& instance() {
        static CliLog s;
        return s;
    }

    void setVerbose(bool v) { m_verbose = v; g_verboseLog = v; }
    bool verbose() const { return m_verbose.load(); }

    // An empty path disables file logging. stderr always remains enabled.
    bool setLogFile(const std::string& path) {
        std::lock_guard<std::mutex> lock(m_mutex);
        FILE* next = nullptr;
        if (!path.empty()) {
            createParentDirectories(path);
            next = openAppend(path);
            if (!next) return false;
        }
        if (m_file) fclose(m_file);
        m_file = next;
        m_logFile = path;
        m_fileBytes = fileSize(m_file);
        return true;
    }

    std::string logFile() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_logFile;
    }

    void error(const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        print(LOG_LVL_ERROR, fmt, ap);
        va_end(ap);
    }

    void info(const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        print(LOG_LVL_INFO, fmt, ap);
        va_end(ap);
    }

    void detail(const char* fmt, ...) {
        if (!m_verbose) return;
        va_list ap;
        va_start(ap, fmt);
        print(LOG_LVL_DETAIL, fmt, ap);
        va_end(ap);
    }

private:
    CliLog() : m_verbose(false), m_file(nullptr), m_fileBytes(0) {
#ifdef _WIN32
        wchar_t directory[32768] = {};
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", directory, 32768);
        if (length && length < 32768) {
            std::wstring path = std::wstring(directory) + L"\\VLan\\logs\\cli.log";
            int size = WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(path.size()),
                                           nullptr, 0, nullptr, nullptr);
            std::string utf8(size, '\0');
            if (size) WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(path.size()),
                                          &utf8[0], size, nullptr, nullptr);
            if (!utf8.empty() && !setLogFile(utf8))
                fprintf(stderr, "[log] Cannot open default CLI log file; stderr logging remains active\n");
        }
#endif
    }

    ~CliLog() { if (m_file) fclose(m_file); }

#ifdef _WIN32
    static std::wstring widePath(const std::string& path) {
        int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
                                        static_cast<int>(path.size()), nullptr, 0);
        if (!count) return std::wstring();
        std::wstring wide(count, L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
                            static_cast<int>(path.size()), &wide[0], count);
        return wide;
    }
#endif

    static FILE* openAppend(const std::string& path) {
#ifdef _WIN32
        std::wstring wide = widePath(path);
        return wide.empty() ? nullptr : _wfopen(wide.c_str(), L"ab");
#else
        int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (fd < 0) return nullptr;
        FILE* file = fdopen(fd, "ab");
        if (!file) close(fd);
        return file;
#endif
    }

    static void createParentDirectories(const std::string& path) {
        for (size_t i = 1; i < path.size(); ++i) {
            if (path[i] != '/' && path[i] != '\\') continue;
            const std::string directory = path.substr(0, i);
#ifdef _WIN32
            if (directory.size() == 2 && directory[1] == ':') continue;
            std::wstring wide = widePath(directory);
            if (!wide.empty()) CreateDirectoryW(wide.c_str(), nullptr);
#else
            mkdir(directory.c_str(), 0700);
#endif
        }
    }

    static size_t fileSize(FILE* file) {
        if (!file) return 0;
        fseek(file, 0, SEEK_END);
        const long size = ftell(file);
        return size > 0 ? static_cast<size_t>(size) : 0;
    }

    static bool moveLog(const std::string& from, const std::string& to) {
#ifdef _WIN32
        return MoveFileExW(widePath(from).c_str(), widePath(to).c_str(),
                           MOVEFILE_REPLACE_EXISTING) != 0;
#else
        return rename(from.c_str(), to.c_str()) == 0;
#endif
    }

    void rotateIfNeeded(size_t bytes) {
        const size_t maximum = 5 * 1024 * 1024;
        if (!m_file || m_fileBytes + bytes <= maximum) return;
        fclose(m_file);
        m_file = nullptr;
        for (int index = 3; index >= 2; --index)
            moveLog(m_logFile + "." + std::to_string(index - 1),
                    m_logFile + "." + std::to_string(index));
        if (!moveLog(m_logFile, m_logFile + ".1"))
            fprintf(stderr, "[log] CLI log rotation failed; continuing current file\n");
        m_file = openAppend(m_logFile);
        m_fileBytes = fileSize(m_file);
        if (!m_file) fprintf(stderr, "[log] Cannot reopen CLI file; stderr logging remains active\n");
    }

    void print(LogLevel level, const char* fmt, va_list ap) {
        std::lock_guard<std::mutex> lock(m_mutex);

        time_t now = time(nullptr);
        struct tm value = {};
#ifdef _WIN32
        localtime_s(&value, &now);
#else
        localtime_r(&now, &value);
#endif
        char timeBuf[32];
        strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &value);

        const char* tag = "INFO";
        if (level == LOG_LVL_ERROR) tag = "ERR ";
        else if (level == LOG_LVL_DETAIL) tag = "DBG ";

        char message[16384] = {};
        int length = vsnprintf(message, sizeof(message), fmt, ap);
        message[sizeof(message) - 1] = 0;
        std::string line = std::string("[") + timeBuf + " " + tag + "] " + message;
        for (size_t i = 0; i < line.size(); ++i)
            if (line[i] == '\n' || line[i] == '\r') line[i] = ' ';
        if (length < 0 || static_cast<size_t>(length) >= sizeof(message)) line += " [truncated]";
        line += '\n';
        fwrite(line.data(), 1, line.size(), stderr);
        fflush(stderr);
        rotateIfNeeded(line.size());
        if (m_file) {
            const size_t written = fwrite(line.data(), 1, line.size(), m_file);
            m_fileBytes += written;
            if (written != line.size() || fflush(m_file) != 0) {
                fprintf(stderr, "[log] CLI file write failed; stderr logging remains active\n");
                fclose(m_file);
                m_file = nullptr;
            }
        }
    }

    std::atomic<bool> m_verbose;
    mutable std::mutex m_mutex;
    FILE* m_file;
    size_t m_fileBytes;
    std::string m_logFile;
};

#define LOG_ERR(...)    VLan::CliLog::instance().error(__VA_ARGS__)
#define LOG_INFO(...)   VLan::CliLog::instance().info(__VA_ARGS__)
#define LOG_DBG(...)    VLan::CliLog::instance().detail(__VA_ARGS__)

} // namespace VLan
#endif // VLAN_CLI_LOG_H
