#include "TimeSync.hpp"

#include <cstdlib>
#include <ctime>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "sdkconfig.h"

namespace {
constexpr const char *TAG = "time_sync";
}

TimeSync::~TimeSync()
{
    configASSERT(!initialized_);
}

esp_err_t TimeSync::initialize()
{
    if (!timezoneConfigured_) {
        if (setenv("TZ", CONFIG_GOFKUCAM_LOG_TIME_ZONE, 1) != 0) { return ESP_ERR_NO_MEM; }
        tzset();
        timezoneConfigured_ = true;
        ESP_LOGI(TAG, "Waiting for NTP; log timestamps are provisional until synchronization");
    }
    if (initialized_) { return ESP_OK; }

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_GOFKUCAM_NTP_SERVER);
    config.start = false;
    config.wait_for_sync = false; // Asynchronous operation; no sync semaphore.
    config.server_from_dhcp = false;
    config.sync_cb = timeSynchronized;
    const esp_err_t result = esp_netif_sntp_init(&config);
    if (result == ESP_OK) { initialized_ = true; }
    return result;
}

esp_err_t TimeSync::setNetworkAvailable(bool available)
{
    if (!available) {
        if (initialized_) {
            esp_netif_sntp_deinit();
            initialized_ = false;
            running_ = false;
        }
        // The RTC/system clock keeps running using the last synchronized time.
        return ESP_OK;
    }
    if (running_) { return ESP_OK; }
    esp_err_t result = initialize();
    if (result == ESP_OK) { result = esp_netif_sntp_start(); }
    if (result == ESP_OK) {
        running_ = true;
        ESP_LOGI(TAG, "Synchronizing time with NTP server %s", CONFIG_GOFKUCAM_NTP_SERVER);
    } else {
        ESP_LOGW(TAG, "Could not start NTP synchronization: %s", esp_err_to_name(result));
    }
    return result;
}

void TimeSync::timeSynchronized(timeval *time)
{
    if (time == nullptr) { return; }
    std::tm local{};
    if (localtime_r(&time->tv_sec, &local) == nullptr) { return; }
    char timestamp[64];
    if (std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S %z (%Z)", &local) == 0) { return; }
    ESP_LOGI(TAG, "NTP time synchronized: %s", timestamp);
}
