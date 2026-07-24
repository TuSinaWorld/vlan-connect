#ifdef _WIN32

#include "../../client/src/core/tun_adapter.h"
#include "../fakes/fake_wintun_api.h"
#include <QByteArray>
#include <QCoreApplication>
#include <cassert>
#include <vector>
#include <windows.h>

using namespace VLan;
using namespace VLanTest;

static void waitForRelease(const FakeWintunApi& api) {
    for (int i = 0; i < 100 && api.releaseCount() == 0; ++i)
        Sleep(10);
}

static void testReceiveReleaseAndRepeatedShutdown() {
    FakeWintunApi api;
    TunAdapter adapter(&api, nullptr);
    assert(adapter.initialize(QStringLiteral("VLanTest")));
    assert(adapter.startSession());

    std::vector<uint8_t> packet(64, 0x5a);
    api.injectPacket(packet);
    waitForRelease(api);
    assert(api.releaseCount() == 1);

    QByteArray outbound(32, '\x11');
    assert(adapter.writePacket(outbound));
    assert(api.sendCount() == 1);

    adapter.shutdown();
    assert(api.unsafeReleaseCount() == 0);
    assert(api.endSessionCount() == 1);
    assert(api.closeAdapterCount() == 1);
    assert(api.unloadCount() == 1);
    assert(!adapter.writePacket(outbound));

    adapter.shutdown();
    assert(api.endSessionCount() == 1);
    assert(api.closeAdapterCount() == 1);
    assert(api.unloadCount() == 1);
}

static void testPartialSessionStartup() {
    FakeWintunApi api;
    api.setReadEventAvailable(false);
    TunAdapter adapter(&api, nullptr);
    assert(adapter.initialize(QStringLiteral("VLanTestPartial")));
    assert(!adapter.startSession());
    assert(api.endSessionCount() == 1);

    adapter.shutdown();
    assert(api.endSessionCount() == 1);
    assert(api.closeAdapterCount() == 1);
    assert(api.unloadCount() == 1);
    assert(api.unsafeReleaseCount() == 0);
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    testReceiveReleaseAndRepeatedShutdown();
    testPartialSessionStartup();
    return 0;
}

#else

int main() {
    return 0;
}

#endif
