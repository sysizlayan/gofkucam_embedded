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

ConnectionHandler::ConnectionHandler()
    : ActiveObject(ConnectionState::Idle) 
{}

esp_err_t ConnectionHandler::connect()
{
    if (!start("connection"))
    {
        return ESP_ERR_NO_MEM;
    }
    return postEvent(ConnectionEvent::Start) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void ConnectionHandler::subscribe(IObserver *observer)
{
    m_connectedSubject.subscribe(observer);
    m_disconnectedSubject.subscribe(observer);
}

void ConnectionHandler::unsubscribe(IObserver *observer)
{
    m_connectedSubject.unsubscribe(observer);
    m_disconnectedSubject.unsubscribe(observer);
}

bool ConnectionHandler::hasAddress() const
{
    return m_wifiAssociated.load() && (m_addresses.load() & WIFI_V4) != 0;
}

esp_err_t ConnectionHandler::configureStaticIp()
{
    esp_err_t result = esp_netif_dhcpc_stop(m_wifiNetif);
    if (result != ESP_OK && result != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) { return result; }

    esp_netif_ip_info_t ip{};
    result = esp_netif_str_to_ip4(NetworkConfig::ADDRESS, &ip.ip);
    if (result == ESP_OK) { result = esp_netif_str_to_ip4(NetworkConfig::NETMASK, &ip.netmask); }
    if (result == ESP_OK) { result = esp_netif_str_to_ip4(NetworkConfig::GATEWAY, &ip.gw); }
    if (result == ESP_OK) { result = esp_netif_set_ip_info(m_wifiNetif, &ip); }
    if (result != ESP_OK) { return result; }

    // DHCP no longer supplies DNS; use the agreed router address for DNS too.
    esp_netif_dns_info_t dns{};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4 = ip.gw;
    return esp_netif_set_dns_info(m_wifiNetif, ESP_NETIF_DNS_MAIN, &dns);
}

esp_err_t ConnectionHandler::initializeWifi()
{
    if (!m_wifiInitialized)
    {
        wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
        const esp_err_t result = esp_wifi_init(&config);
        if (result != ESP_OK) { return result; }
        m_wifiInitialized = true;
    }
    if (m_wifiNetif == nullptr)
    {
        esp_netif_inherent_config_t config = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
        config.if_desc = "gofkucam_wifi_sta";
        config.route_prio = 128;

        m_wifiNetif = esp_netif_create_wifi(WIFI_IF_STA, &config);
        if (m_wifiNetif == nullptr)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    const esp_err_t networkResult = configureStaticIp();
    if (networkResult != ESP_OK)
    {
        return networkResult;
    }

    if (!m_wifiDefaultsRegistered)
    {
        const esp_err_t result = esp_wifi_set_default_wifi_sta_handlers();
        if (result != ESP_OK) { return result; }
        m_wifiDefaultsRegistered = true;
    }
    // Default interface handlers must run before our connection callback.
    if (!m_wifiHandlerRegistered) {
        const esp_err_t result = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_event_handler, this);
        if (result != ESP_OK) { return result; }
        m_wifiHandlerRegistered = true;
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
    if (result == ESP_OK)
    {
        result = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (result == ESP_OK)
    {
        result = esp_wifi_set_config(WIFI_IF_STA, &config);
        if (result != ESP_OK){return result;}
    }
    if (!m_wifiStarted)
    {
        result = esp_wifi_start();
        if (result != ESP_OK){return result;}
        m_wifiStarted = true;
    }
    return esp_wifi_connect();
}

esp_err_t ConnectionHandler::beginAttempt()
{
    if (!m_ipHandlerRegistered)
    {
        const esp_err_t result = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, network_event_handler, this);
        if (result != ESP_OK) { return result; }
        m_ipHandlerRegistered = true;
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
                    if (m_attemptFailed.load() || m_recovery.remaining() == 0) {
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
                    if (m_recovery.remaining() == 0) { return ConnectionState::WaitingForConnection; }
                    break;
            }
            break;
    }
    return state;
}


void ConnectionHandler::on_state_enter(ConnectionState next)
{
    switch (next)
    {
        case ConnectionState::Idle:
            // Do nothing; wait for connect() to be called.
            break;
        case ConnectionState::WaitingForConnection:
        {
            m_attemptFailed.store(false);
            m_recovery.waitForConnection();
            const esp_err_t result = beginAttempt();
            if (result != ESP_OK)
            {
                ESP_LOGW(TAG, "Connection attempt failed: %s", esp_err_to_name(result));
                m_attemptFailed.store(true);
                m_recovery.failed();
            }
            break;
        }
        case ConnectionState::Connected:
        {
            m_recovery.connected();
            ESP_LOGI(TAG, "Connected to access point \"%s\"", CONFIG_GOFKUCAM_ACCESS_POINT_NAME);
            esp_netif_ip_info_t ip{};
            if (esp_netif_get_ip_info(m_wifiNetif, &ip) == ESP_OK)
            {
                ESP_LOGI(TAG, "- IPv4 address: " IPSTR ",", IP2STR(&ip.ip));
            }
            m_notifiedConnected = true;
            m_connectedSubject.notify();
            break;
        }
        case ConnectionState::Recovering:
        {
            m_notifiedConnected = false;
            m_disconnectedSubject.notify();
            m_wifiAssociated.store(false);
            m_addresses.store(0);
            if (m_wifiStarted)
            {
                esp_wifi_disconnect();
            }
            m_recovery.recover();
            ESP_LOGI(TAG, "Recovering; retry in %u ms", static_cast<unsigned>(m_recovery.remaining() * portTICK_PERIOD_MS));
            break;
        }
    }
}

TickType_t ConnectionHandler::event_wait_ticks() const 
{
    return m_recovery.remaining();
}

void ConnectionHandler::on_timeout()
{
    handle_event(ConnectionEvent::Timeout);
}

void ConnectionHandler::network_event_handler(void *argument, esp_event_base_t base, std::int32_t id, void *data)
{
    auto *self = static_cast<ConnectionHandler *>(argument);
    unsigned address = 0;
    const bool disconnected = (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) ||
                              (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP);
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED)
    {
        self->m_wifiAssociated.store(true);
        // Static IP configuration is complete before the driver can connect.
        self->m_addresses.fetch_or(WIFI_V4);
        self->postEvent(ConnectionEvent::AddressChanged);
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP && data != nullptr &&
        static_cast<ip_event_got_ip_t *>(data)->esp_netif == self->m_wifiNetif)
    {
        address = WIFI_V4;
    }

    if (disconnected)
    {
        self->m_wifiAssociated.store(false);
        self->m_addresses.store(0);
        self->m_attemptFailed.store(true);
        self->postEvent(ConnectionEvent::Disconnected);
    }
    else if (address != 0)
    {
        self->m_addresses.fetch_or(address);
        self->postEvent(ConnectionEvent::AddressChanged);
    }
}
