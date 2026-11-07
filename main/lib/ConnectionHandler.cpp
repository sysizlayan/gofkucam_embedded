#include "ConnectionHandler.hpp"
#include "NetworkConfig.hpp"

#include <cstring>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "sdkconfig.h"

#if !CONFIG_LWIP_IPV4
#error "The static Wi-Fi address requires IPv4 support"
#endif

namespace {
constexpr const char *TAG = "connection";
constexpr unsigned WIFI_V4 = 1;
}

ConnectionHandler::ConnectionHandler() : ActiveObject(ConnectionState::Idle) {}

esp_err_t ConnectionHandler::connect()
{
    if (!start("connection")) {
        return ESP_ERR_NO_MEM;
    }
    return postEvent(ConnectionEvent::Start) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void ConnectionHandler::setConnectionListener(ConnectionListener listener, void *context)
{
    listener_ = listener;
    listenerContext_ = context;
}

bool ConnectionHandler::hasAddress() const
{
    return wifiAssociated_.load() && (addresses_.load() & WIFI_V4) != 0;
}

esp_err_t ConnectionHandler::configureStaticIp()
{
    esp_err_t result = esp_netif_dhcpc_stop(wifiNetif_);
    if (result != ESP_OK && result != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) { return result; }

    esp_netif_ip_info_t ip{};
    result = esp_netif_str_to_ip4(NetworkConfig::ADDRESS, &ip.ip);
    if (result == ESP_OK) { result = esp_netif_str_to_ip4(NetworkConfig::NETMASK, &ip.netmask); }
    if (result == ESP_OK) { result = esp_netif_str_to_ip4(NetworkConfig::GATEWAY, &ip.gw); }
    if (result == ESP_OK) { result = esp_netif_set_ip_info(wifiNetif_, &ip); }
    if (result != ESP_OK) { return result; }

    // DHCP no longer supplies DNS; use the agreed router address for DNS too.
    esp_netif_dns_info_t dns{};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4 = ip.gw;
    return esp_netif_set_dns_info(wifiNetif_, ESP_NETIF_DNS_MAIN, &dns);
}

esp_err_t ConnectionHandler::initializeWifi()
{
    if (!wifiInitialized_) {
        wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
        const esp_err_t result = esp_wifi_init(&config);
        if (result != ESP_OK) { return result; }
        wifiInitialized_ = true;
    }
    if (wifiNetif_ == nullptr) {
        esp_netif_inherent_config_t config = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
        config.if_desc = "gofkucam_wifi_sta";
        config.route_prio = 128;
        wifiNetif_ = esp_netif_create_wifi(WIFI_IF_STA, &config);
        if (wifiNetif_ == nullptr) { return ESP_ERR_NO_MEM; }
    }
    const esp_err_t networkResult = configureStaticIp();
    if (networkResult != ESP_OK) { return networkResult; }
    if (!wifiDefaultsRegistered_) {
        const esp_err_t result = esp_wifi_set_default_wifi_sta_handlers();
        if (result != ESP_OK) { return result; }
        wifiDefaultsRegistered_ = true;
    }
    // Default interface handlers must run before our connection callback.
    if (!wifiHandlerRegistered_) {
        const esp_err_t result = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_event_handler, this);
        if (result != ESP_OK) { return result; }
        wifiHandlerRegistered_ = true;
    }
    wifi_config_t config{};
    static_assert(sizeof(CONFIG_GOFKUCAM_ACCESS_POINT_NAME) - 1 <= sizeof(config.sta.ssid));
    static_assert(sizeof(CONFIG_GOFKUCAM_ACCESS_POINT_PASSWORD) - 1 <= sizeof(config.sta.password));
    std::memcpy(config.sta.ssid, CONFIG_GOFKUCAM_ACCESS_POINT_NAME, sizeof(CONFIG_GOFKUCAM_ACCESS_POINT_NAME) - 1);
    std::memcpy(config.sta.password, CONFIG_GOFKUCAM_ACCESS_POINT_PASSWORD, sizeof(CONFIG_GOFKUCAM_ACCESS_POINT_PASSWORD) - 1);
    config.sta.scan_method = WIFI_FAST_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config.sta.threshold.rssi = -127;
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    esp_err_t result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (result == ESP_OK) { result = esp_wifi_set_mode(WIFI_MODE_STA); }
    if (result == ESP_OK) { result = esp_wifi_set_config(WIFI_IF_STA, &config); }
    if (result != ESP_OK) { return result; }
    if (!wifiStarted_) {
        result = esp_wifi_start();
        if (result != ESP_OK) { return result; }
        wifiStarted_ = true;
    }
    return esp_wifi_connect();
}

esp_err_t ConnectionHandler::beginAttempt()
{
    if (!ipHandlerRegistered_) {
        const esp_err_t result = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, network_event_handler, this);
        if (result != ESP_OK) { return result; }
        ipHandlerRegistered_ = true;
    }
    return initializeWifi();
}

ConnectionState ConnectionHandler::handle_state_event(ConnectionState state, const ConnectionEvent &event)
{
    // Every queued notification reconciles the latest status, including when
    // a full queue prevented a more recent notification from being enqueued.
    switch (state)
    {
        case ConnectionState::Idle:
            switch (event)
            {
                case ConnectionEvent::Start:
                    return ConnectionState::WaitingForConnection;
                case ConnectionEvent::AddressChanged:
                case ConnectionEvent::Disconnected:
                case ConnectionEvent::Timeout:
                    break;
            }
            break;

        case ConnectionState::WaitingForConnection:
            switch (event)
            {
                case ConnectionEvent::Start:
                case ConnectionEvent::AddressChanged:
                case ConnectionEvent::Disconnected:
                case ConnectionEvent::Timeout:
                    if (hasAddress()) { return ConnectionState::Connected; }
                    if (attemptFailed_.load() || recovery_.remaining() == 0) {
                        return ConnectionState::Recovering;
                    }
                    break;
            }
            break;

        case ConnectionState::Connected:
            switch (event)
            {
                case ConnectionEvent::Start:
                case ConnectionEvent::AddressChanged:
                case ConnectionEvent::Disconnected:
                case ConnectionEvent::Timeout:
                    if (!hasAddress()) { return ConnectionState::Recovering; }
                    break;
            }
            break;

        case ConnectionState::Recovering:
            switch (event)
            {
                case ConnectionEvent::Start:
                case ConnectionEvent::AddressChanged:
                case ConnectionEvent::Disconnected:
                case ConnectionEvent::Timeout:
                    if (hasAddress()) { return ConnectionState::Connected; }
                    if (recovery_.remaining() == 0) { return ConnectionState::WaitingForConnection; }
                    break;
            }
            break;
    }
    return state;
}

void ConnectionHandler::notifyConnection(bool connected)
{
    if (notifiedConnected_ != connected) {
        notifiedConnected_ = connected;
        if (listener_ != nullptr) { listener_(listenerContext_, connected); }
    }
}

void ConnectionHandler::on_state_enter(ConnectionState next)
{
    if (next == ConnectionState::WaitingForConnection) {
        attemptFailed_.store(false);
        recovery_.waitForConnection();
        const esp_err_t result = beginAttempt();
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Connection attempt failed: %s", esp_err_to_name(result));
            attemptFailed_.store(true);
            recovery_.failed();
        }
    } else if (next == ConnectionState::Recovering) {
        notifyConnection(false);
        wifiAssociated_.store(false);
        addresses_.store(0);
        if (wifiStarted_) { esp_wifi_disconnect(); }
        recovery_.recover();
        ESP_LOGI(TAG, "Recovering; retry in %u ms", static_cast<unsigned>(recovery_.remaining() * portTICK_PERIOD_MS));
    } else if (next == ConnectionState::Connected) {
        recovery_.connected();
        ESP_LOGI(TAG, "Connected to access point \"%s\"", CONFIG_GOFKUCAM_ACCESS_POINT_NAME);
        esp_netif_ip_info_t ip{};
        if (esp_netif_get_ip_info(wifiNetif_, &ip) == ESP_OK) {
            ESP_LOGI(TAG, "- IPv4 address: " IPSTR ",", IP2STR(&ip.ip));
        }
        notifyConnection(true);
    }
}

TickType_t ConnectionHandler::event_wait_ticks() const { return recovery_.remaining(); }
void ConnectionHandler::on_timeout() { handle_event(ConnectionEvent::Timeout); }

void ConnectionHandler::network_event_handler(void *argument, esp_event_base_t base, std::int32_t id, void *data)
{
    auto *self = static_cast<ConnectionHandler *>(argument);
    unsigned address = 0;
    const bool disconnected = (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) ||
                              (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP);
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        self->wifiAssociated_.store(true);
        // Static IP configuration is complete before the driver can connect.
        self->addresses_.fetch_or(WIFI_V4);
        self->postEvent(ConnectionEvent::AddressChanged);
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP && data != nullptr &&
        static_cast<ip_event_got_ip_t *>(data)->esp_netif == self->wifiNetif_) { address = WIFI_V4; }
    if (disconnected) {
        self->wifiAssociated_.store(false);
        self->addresses_.store(0);
        self->attemptFailed_.store(true);
        self->postEvent(ConnectionEvent::Disconnected);
    } else if (address != 0) {
        self->addresses_.fetch_or(address);
        self->postEvent(ConnectionEvent::AddressChanged);
    }
}
