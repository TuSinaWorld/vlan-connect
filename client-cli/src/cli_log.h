#ifndef VLAN_CLI_LOG_H
#define VLAN_CLI_LOG_H

#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <mutex>
#include <string>

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
    bool verbose() const { return m_verbose; }

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
    CliLog() : m_verbose(false) {}

    void print(LogLevel level, const char* fmt, va_list ap) {
        std::lock_guard<std::mutex> lock(m_mutex);

        time_t now = time(nullptr);
        struct tm* t = localtime(&now);
        char timeBuf[32];
        strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", t);

        const char* tag = "INFO";
        if (level == LOG_LVL_ERROR) tag = "ERR ";
        else if (level == LOG_LVL_DETAIL) tag = "DBG ";

        fprintf(stderr, "[%s %s] ", timeBuf, tag);
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
        fflush(stderr);
    }

    bool m_verbose;
    std::mutex m_mutex;
};

#define LOG_ERR(...)    VLan::CliLog::instance().error(__VA_ARGS__)
#define LOG_INFO(...)   VLan::CliLog::instance().info(__VA_ARGS__)
#define LOG_DBG(...)    VLan::CliLog::instance().detail(__VA_ARGS__)

} // namespace VLan
#endif // VLAN_CLI_LOG_H
