#ifndef VLAN_RELAY_PROBE_WATCHDOG_H
#define VLAN_RELAY_PROBE_WATCHDOG_H

#include <cstdint>

namespace VLan {

class RelayProbeWatchdog {
public:
    RelayProbeWatchdog() : m_waiting(false), m_reported(false), m_since(0) {}

    // Report once for an uninterrupted outage, including transport rebuilds.
    // A fresh peer reply, removal, or room teardown resets the outage.
    bool observe(uint32_t now, bool hasFreshReply) {
        if (hasFreshReply) {
            m_waiting = false;
            m_reported = false;
            return false;
        }
        if (!m_waiting) {
            m_waiting = true;
            m_since = now;
        }
        if (!m_reported && now - m_since >= 10000u) {
            m_reported = true;
            return true;
        }
        return false;
    }

private:
    bool m_waiting;
    bool m_reported;
    uint32_t m_since;
};

} // namespace VLan

#endif // VLAN_RELAY_PROBE_WATCHDOG_H
