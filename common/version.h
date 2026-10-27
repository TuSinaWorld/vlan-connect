#ifndef VLAN_VERSION_H
#define VLAN_VERSION_H

// Canonical software release version; shared by all executables and Win32 resources.
#define VLAN_RELEASE_VERSION "0.3.4"
#define VLAN_RELEASE_VERSION_COMMA 0,3,4,0
#define VLAN_RELEASE_VERSION_WINDOWS "0.3.4.0"
#ifndef VLAN_BUILD_COMMIT
#define VLAN_BUILD_COMMIT "unknown"
#endif

#ifndef RC_INVOKED
#include "protocol.h"
#include <sstream>
#include <string>

namespace VLan {
inline std::string releaseBuildDescription(const char* component) {
    std::ostringstream out;
    out << component << " " << VLAN_RELEASE_VERSION
        << " protocol=" << PROTOCOL_VERSION << " build=" << VLAN_BUILD_COMMIT;
    return out.str();
}
} // namespace VLan
#endif

#endif
