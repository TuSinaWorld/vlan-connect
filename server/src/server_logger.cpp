#include "server_logger.h"
#include <cstdlib>
#include <vector>

namespace VLan {

ServerLogger& ServerLogger::instance() {
    static ServerLogger s;
    return s;
}

ServerLogger::ServerLogger()
    : m_file(nullptr), m_maxFileSize(10 * 1024 * 1024),
      m_level(SRV_LOG_NORMAL) {}

ServerLogger::~ServerLogger() { shutdown(); }

void ServerLogger::init(const std::string& filePath, size_t maxFileSizeBytes,
                        ServerLogLevel level) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_filePath    = filePath;
    m_maxFileSize = maxFileSizeBytes;
    m_level       = level;

    if (!m_filePath.empty()) {
        m_file = fopen(m_filePath.c_str(), "a");
        if (!m_file)
            fprintf(stderr, "[logger] Cannot open log file: %s\n", m_filePath.c_str());
    }
}

void ServerLogger::shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file) { fclose(m_file); m_file = nullptr; }
}

std::string ServerLogger::currentTimestamp() {
    time_t now = time(nullptr);
    struct tm t;
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
    return buf;
}

void ServerLogger::checkAndRotate() {
    if (!m_file || m_maxFileSize == 0) return;

    long pos = ftell(m_file);
    if (pos < 0) return;
    if (static_cast<size_t>(pos) < m_maxFileSize) return;

    fclose(m_file);

    FILE* src = fopen(m_filePath.c_str(), "rb");
    if (!src) {
        m_file = fopen(m_filePath.c_str(), "a");
        return;
    }

    fseek(src, 0, SEEK_END);
    long fileSize = ftell(src);
    long keepFrom = fileSize / 2;

    // Align to next newline
    fseek(src, keepFrom, SEEK_SET);
    int ch;
    while ((ch = fgetc(src)) != EOF && ch != '\n') {}
    keepFrom = ftell(src);

    long keepSize = fileSize - keepFrom;
    std::vector<char> buf(keepSize);
    fseek(src, keepFrom, SEEK_SET);
    size_t readN = fread(buf.data(), 1, keepSize, src);
    fclose(src);

    m_file = fopen(m_filePath.c_str(), "w");
    if (m_file) {
        std::string ts = currentTimestamp();
        fprintf(m_file, "--- Log truncated at %s, previous entries removed ---\n", ts.c_str());
        if (readN > 0) fwrite(buf.data(), 1, readN, m_file);
        fflush(m_file);
    }
}

void ServerLogger::writeLog(const char* tag, const char* fmt, va_list args) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string ts = currentTimestamp();

    char msgBuf[4096];
    vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);

    printf("[%s] [%s] %s\n", ts.c_str(), tag, msgBuf);
    fflush(stdout);

    if (m_file) {
        fprintf(m_file, "[%s] [%s] %s\n", ts.c_str(), tag, msgBuf);
        fflush(m_file);
        checkAndRotate();
    }
}

void ServerLogger::logInfo(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    writeLog("INFO", fmt, args);
    va_end(args);
}

void ServerLogger::logDetail(const char* fmt, ...) {
    if (m_level < SRV_LOG_DETAIL) return;
    va_list args;
    va_start(args, fmt);
    writeLog("DEBUG", fmt, args);
    va_end(args);
}

void ServerLogger::logError(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    writeLog("ERROR", fmt, args);
    va_end(args);
}

} // namespace VLan
