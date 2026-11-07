#pragma once

#include "esp_err.h"

struct timeval;

class TimeSync {
public:
    TimeSync() = default;
    ~TimeSync();
    TimeSync(const TimeSync &) = delete;
    TimeSync &operator=(const TimeSync &) = delete;

    // Configure the clock and SNTP after esp_netif_init(), before Wi-Fi starts.
    esp_err_t initialize();
    // Call from the connection handler's task. No wait for an NTP response.
    esp_err_t setNetworkAvailable(bool available);

private:
    static void timeSynchronized(timeval *time);

    bool timezoneConfigured_ = false;
    bool initialized_ = false;
    bool running_ = false;
};
