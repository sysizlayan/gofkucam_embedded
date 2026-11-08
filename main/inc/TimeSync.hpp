#pragma once

#include "IObserver.hpp"
#include "esp_err.h"

struct timeval;

class TimeSync : public IObserver {
public:
    TimeSync() = default;
    ~TimeSync() override;
    TimeSync(const TimeSync &) = delete;
    TimeSync &operator=(const TimeSync &) = delete;

    // Configure the clock and SNTP after esp_netif_init(), before Wi-Fi starts.
    esp_err_t initialize();
    // Call from the connection handler's task. No wait for an NTP response.
    esp_err_t setNetworkAvailable(bool available);
    void update(ObservedSignal signal, const ObservedPayload &payload = {}) override;

private:
    static void timeSynchronized(timeval *time);

    bool m_timezoneConfigured = false;
    bool m_initialized = false;
    bool m_running = false;
};
