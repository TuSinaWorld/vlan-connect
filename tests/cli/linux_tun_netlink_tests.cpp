#ifndef _WIN32

#include "cli_tun.h"
#include <cassert>
#include <cerrno>
#include <linux/rtnetlink.h>
#include <vector>

using namespace VLan;

class FakeNetlinkTransport : public NetlinkTransport {
public:
    int failOnCall = 0;
    std::vector<uint16_t> messageTypes;

    bool transact(const Buffer& request, int* errorCode) override {
        const struct nlmsghdr* header =
            reinterpret_cast<const struct nlmsghdr*>(request.data());
        messageTypes.push_back(header->nlmsg_type);
        if (failOnCall != 0 &&
            static_cast<int>(messageTypes.size()) == failOnCall) {
            if (errorCode) *errorCode = EIO;
            return false;
        }
        if (errorCode) *errorCode = 0;
        return true;
    }
};

static void testSuccessAndShutdownRollback() {
    FakeNetlinkTransport transport;
    LinuxTunConfiguration state;
    int error = -1;
    assert(configureLinuxTun(&transport, 7, VNET_SUBNET | 2,
                             VNET_MASK, ROOM_MTU_DEFAULT,
                             &state, &error));
    assert(error == 0);
    assert(state.prefix == 24);
    assert(state.addressConfigured && state.routeConfigured);
    assert(transport.messageTypes.size() == 3);
    assert(transport.messageTypes[0] == RTM_NEWADDR);
    assert(transport.messageTypes[1] == RTM_NEWLINK);
    assert(transport.messageTypes[2] == RTM_NEWROUTE);

    rollbackLinuxTun(&transport, &state);
    assert(!state.addressConfigured && !state.routeConfigured);
    assert(transport.messageTypes.size() == 5);
    assert(transport.messageTypes[3] == RTM_DELROUTE);
    assert(transport.messageTypes[4] == RTM_DELADDR);
}

static void testFailureRollback() {
    FakeNetlinkTransport addressFailure;
    addressFailure.failOnCall = 1;
    LinuxTunConfiguration state;
    int error = 0;
    assert(!configureLinuxTun(&addressFailure, 7, VNET_SUBNET | 2,
                              VNET_MASK, ROOM_MTU_DEFAULT,
                              &state, &error));
    assert(error == EIO);
    assert(!state.addressConfigured && !state.routeConfigured);
    assert(addressFailure.messageTypes.size() == 1);
    assert(addressFailure.messageTypes[0] == RTM_NEWADDR);

    FakeNetlinkTransport transport;
    transport.failOnCall = 2;
    assert(!configureLinuxTun(&transport, 7, VNET_SUBNET | 2,
                              VNET_MASK, ROOM_MTU_DEFAULT,
                              &state, &error));
    assert(error == EIO);
    assert(!state.addressConfigured && !state.routeConfigured);
    assert(transport.messageTypes.size() == 3);
    assert(transport.messageTypes[2] == RTM_DELADDR);

    FakeNetlinkTransport routeFailure;
    routeFailure.failOnCall = 3;
    assert(!configureLinuxTun(&routeFailure, 7, VNET_SUBNET | 2,
                              VNET_MASK, ROOM_MTU_DEFAULT,
                              &state, &error));
    assert(error == EIO);
    assert(!state.addressConfigured && !state.routeConfigured);
    assert(routeFailure.messageTypes.size() == 4);
    assert(routeFailure.messageTypes[0] == RTM_NEWADDR);
    assert(routeFailure.messageTypes[1] == RTM_NEWLINK);
    assert(routeFailure.messageTypes[2] == RTM_NEWROUTE);
    assert(routeFailure.messageTypes[3] == RTM_DELADDR);

    FakeNetlinkTransport invalidMask;
    assert(!configureLinuxTun(&invalidMask, 7, VNET_SUBNET | 2,
                              0xff00ff00u, ROOM_MTU_DEFAULT,
                              &state, &error));
    assert(error == EINVAL);
    assert(invalidMask.messageTypes.empty());
}

int main() {
    testSuccessAndShutdownRollback();
    testFailureRollback();
    return 0;
}

#endif
