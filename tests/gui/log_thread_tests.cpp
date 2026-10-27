#include <QCoreApplication>
#include "../../client/src/ui/log_manager.h"
#include <atomic>
#include <cassert>
#include <thread>
#include <vector>

using namespace VLan;

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    LogManager& log = LogManager::instance();
    log.clearMaskedKeywords();
    log.addMaskedKeyword("private-test-marker");
    std::atomic<int> finished(0);
    std::vector<std::thread> writers;
    for (int i = 0; i < 4; ++i) writers.emplace_back([&]() {
        for (int n = 0; n < 1000; ++n) log.logDetail("worker private-test-marker");
        ++finished;
    });
    int masks = 0;
    while (finished < 4) {
        log.addMaskedKeyword(QString("temporary-%1").arg(masks++ % 32));
        const QList<LogEntry> entries = log.allEntries();
        assert(entries.size() <= 2000);
        for (const LogEntry& entry : entries) assert(!entry.message.contains("private-test-marker"));
        std::this_thread::yield();
    }
    for (std::thread& writer : writers) writer.join();
    const QList<LogEntry> snapshot = log.allEntries();
    assert(snapshot.size() == 2000);
    log.logDetail("a later append cannot invalidate a returned snapshot");
    assert(snapshot.size() == 2000 && snapshot.back().message == "worker [*]");
    log.clearMaskedKeywords();
    return 0;
}
