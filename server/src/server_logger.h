#ifndef VLAN_SERVER_LOGGER_H
#define VLAN_SERVER_LOGGER_H

#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <cstring>
#include <string>
#include <mutex>

namespace VLan {

enum ServerLogLevel {
    SRV_LOG_NORMAL = 0,
    SRV_LOG_DETAIL = 1
};

class ServerLogger {
public:
    static ServerLogger& instance();

    void init(const std::string& filePath = "",
              size_t maxFileSizeBytes = 10 * 1024 * 1024,
              ServerLogLevel level = SRV_LOG_NORMAL);
    void shutdown();

    void logInfo(const char* fmt, ...);
    void logDetail(const char* fmt, ...);
    void logError(const char* fmt, ...);

    ServerLogLevel level() const { return m_level; }

private:
    ServerLogger();
    ~ServerLogger();
    ServerLogger(const ServerLogger&);

    void writeLog(const char* tag, const char* fmt, va_list args);
    void checkAndRotate();
    std::string currentTimestamp();

    FILE*          m_file;
    std::string    m_filePath;
    size_t         m_maxFileSize;
    ServerLogLevel m_level;
    std::mutex     m_mutex;
};

} // namespace VLan

#define LOG_INFO(fmt, ...)    VLan::ServerLogger::instance().logInfo(fmt, ##__VA_ARGS__)
#define LOG_DETAIL(fmt, ...)  VLan::ServerLogger::instance().logDetail(fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...)   VLan::ServerLogger::instance().logError(fmt, ##__VA_ARGS__)

#endif // VLAN_SERVER_LOGGER_H
