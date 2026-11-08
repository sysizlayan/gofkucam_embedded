#include "MqttHandler.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"

static constexpr const char *TAG = "mqtt_example";

MqttHandler::MqttHandler(GateController &gate) : ActiveObject(MqttState::Idle), m_gate(gate)
{
}

MqttHandler::~MqttHandler()
{
    // MQTT retains this instance as its callback context until device restart.
    configASSERT(m_client == nullptr);
}

esp_err_t MqttHandler::connect()
{
    if (!start("mqtt_handler")) { return ESP_ERR_NO_MEM; }
    return postEvent(MqttEvent::Start) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void MqttHandler::subscribe(IObserver *observer)
{
    m_connectedSubject.subscribe(observer);
    m_disconnectedSubject.subscribe(observer);
}

void MqttHandler::unsubscribe(IObserver *observer)
{
    m_connectedSubject.unsubscribe(observer);
    m_disconnectedSubject.unsubscribe(observer);
}

void MqttHandler::notifyConnection(bool connected)
{
    if (m_notifiedConnected != connected) {
        m_notifiedConnected = connected;
        if (connected) {
            m_connectedSubject.notify();
        } else {
            m_disconnectedSubject.notify();
        }
    }
}

void MqttHandler::setNetworkAvailable(bool available)
{
    if (!available)
    { 
        m_notifiedConnected = false;
        m_disconnectedSubject.notify();
    }
    const bool previous = m_networkAvailable.exchange(available);
    if (previous && !available) { m_networkLost.store(true); }
    if (previous != available) {
        postEvent(MqttEvent::NetworkChanged);
    }
}

void MqttHandler::update(ObservedSignal signal, const ObservedPayload &payload)
{
    (void)payload;
    switch (signal) {
    case ObservedSignal::NetworkConnected:
        setNetworkAvailable(true);
        break;
    case ObservedSignal::NetworkDisconnected:
        setNetworkAvailable(false);
        break;
    default:
        break;
    }
}

esp_err_t MqttHandler::createClient()
{
    if (m_client != nullptr) { return ESP_OK; }
    esp_mqtt_client_config_t config{};
    config.broker.address.hostname = CONFIG_GOFKUCAM_MQTT_BROKER_ADDRESS;
    config.broker.address.port = CONFIG_GOFKUCAM_MQTT_BROKER_PORT;
    config.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
    config.network.disable_auto_reconnect = true;
    config.network.timeout_ms = 5000;
    config.session.keepalive = 30;
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&config);
    if (client == nullptr) { return ESP_ERR_NO_MEM; }
    const esp_err_t result = esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqtt_event_handler, this);
    if (result != ESP_OK) {
        esp_mqtt_client_destroy(client);
        return result;
    }
    m_client = client;
    return ESP_OK;
}

void MqttHandler::beginAttempt()
{
    m_attemptActive = m_networkAvailable.load();
    m_attemptFailed = false;
    if (!m_attemptActive) { return; }
    m_recovery.waitForConnection();
    esp_err_t result = createClient();
    if (result == ESP_OK) {
        result = m_clientStarted ? esp_mqtt_client_reconnect(m_client) : esp_mqtt_client_start(m_client);
        if (result == ESP_OK) { m_clientStarted = true; }
    }
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Broker attempt failed: %s", esp_err_to_name(result));
        m_attemptFailed = true;
        m_recovery.failed();
    }
}

void MqttHandler::mqtt_event_handler(void *handlerArgs, esp_event_base_t base,
                                     std::int32_t eventId, void *eventData)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32, base, eventId);
    if (handlerArgs != nullptr && eventData != nullptr) {
        auto *self = static_cast<MqttHandler *>(handlerArgs);
        self->handle_mqtt_event(static_cast<esp_mqtt_event_handle_t>(eventData));
    }
}

void MqttHandler::handle_mqtt_event(esp_mqtt_event_handle_t event)
{
    switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        resetGateMessage();
        m_brokerConnected.store(m_networkAvailable.load());
        postEvent(MqttEvent::BrokerChanged);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        resetGateMessage();
        m_notifiedConnected = false;
        m_disconnectedSubject.notify();
        m_brokerConnected.store(false);
        postEvent(MqttEvent::BrokerChanged);
        break;
    case MQTT_EVENT_SUBSCRIBED:
        if (event->data != nullptr && event->data_len > 0) {
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d, return code=0x%02x ",
                     event->msg_id, static_cast<std::uint8_t>(*event->data));
            if (static_cast<std::uint8_t>(*event->data) >= 0x80) {
                ESP_LOGW(TAG, "Broker rejected the CatStatus subscription");
                m_notifiedConnected = false;
                m_disconnectedSubject.notify();
                m_brokerConnected.store(false);
                postEvent(MqttEvent::BrokerChanged);
            }
        } else {
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        }
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        handleGateMessage(event);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        resetGateMessage();
        m_notifiedConnected = false;
        m_disconnectedSubject.notify();
        m_brokerConnected.store(false);
        postEvent(MqttEvent::BrokerChanged);
        if (event->error_handle == nullptr) {
            break;
        }
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGI(TAG, "Last captured errno : %d (%s)", event->error_handle->esp_transport_sock_errno,
                     std::strerror(event->error_handle->esp_transport_sock_errno));
        } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGI(TAG, "Connection refused error: 0x%x", event->error_handle->connect_return_code);
        } else {
            ESP_LOGW(TAG, "Unknown error type: 0x%x", event->error_handle->error_type);
        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

void MqttHandler::resetGateMessage()
{
    m_gateMessageLength = 0;
    m_gateMessageReceived = 0;
}

void MqttHandler::handleGateMessage(esp_mqtt_event_handle_t event)
{
    constexpr char TOPIC[] = "CatStatus";
    constexpr int TOPIC_LENGTH = sizeof(TOPIC) - 1;
    if (event->current_data_offset == 0) {
        resetGateMessage();
        if (event->topic == nullptr || event->topic_len != TOPIC_LENGTH ||
            std::memcmp(event->topic, TOPIC, sizeof(TOPIC) - 1) != 0 ||
            (event->total_data_len != 4 && event->total_data_len != 5)) { return; }
        m_gateMessageLength = event->total_data_len;
    }
    if (m_gateMessageLength == 0) { return; }
    if (event->total_data_len != m_gateMessageLength || event->current_data_offset != m_gateMessageReceived ||
        event->data_len <= 0 || event->data == nullptr || event->data_len > m_gateMessageLength - m_gateMessageReceived ||
        (event->topic != nullptr && (event->topic_len != TOPIC_LENGTH ||
                                   std::memcmp(event->topic, TOPIC, sizeof(TOPIC) - 1) != 0))) {
        resetGateMessage();
        return;
    }
    std::memcpy(m_gateMessage + m_gateMessageReceived, event->data, static_cast<std::size_t>(event->data_len));
    m_gateMessageReceived += event->data_len;
    if (m_gateMessageReceived != m_gateMessageLength) { return; }

    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (m_gateMessageLength == 4 && std::memcmp(m_gateMessage, "open", 4) == 0) {
        result = m_gate.open();
        ESP_LOGI(TAG, "CatStatus command received: open");
    } else if (m_gateMessageLength == 5 && std::memcmp(m_gateMessage, "close", 5) == 0) {
        result = m_gate.close();
        ESP_LOGI(TAG, "CatStatus command received: close");
    }
    if (result != ESP_OK) { ESP_LOGW(TAG, "Gate command not accepted: %s", esp_err_to_name(result)); }
    resetGateMessage();
}

MqttState MqttHandler::handle_state_event(MqttState state, const MqttEvent &event)
{
    switch (state)
    {
        case MqttState::Idle:
            switch (event)
            {
                case MqttEvent::Start:
                    return MqttState::WaitingForConnection;
                case MqttEvent::NetworkChanged:
                case MqttEvent::BrokerChanged:
                case MqttEvent::Timeout:
                    break;
            }
            break;

        case MqttState::WaitingForConnection:
            switch (event)
            {
                case MqttEvent::Start:
                case MqttEvent::NetworkChanged:
                case MqttEvent::BrokerChanged:
                case MqttEvent::Timeout:
                    if (m_networkLost.exchange(false)) {
                        m_brokerConnected.store(false);
                        if (m_attemptActive) { return MqttState::Recovering; }
                    }
                    if (!m_networkAvailable.load()) {
                        m_brokerConnected.store(false);
                        return m_attemptActive ? MqttState::Recovering : state;
                    }
                    if (m_brokerConnected.load()) { return MqttState::Connected; }
                    if (event == MqttEvent::NetworkChanged && !m_attemptActive) { beginAttempt(); }
                    if (m_attemptFailed || (event == MqttEvent::BrokerChanged && m_attemptActive) ||
                        (m_attemptActive && m_recovery.remaining() == 0)) {
                        return MqttState::Recovering;
                    }
                    break;
            }
            break;

        case MqttState::Connected:
            switch (event)
            {
                case MqttEvent::Start:
                case MqttEvent::NetworkChanged:
                case MqttEvent::BrokerChanged:
                case MqttEvent::Timeout:
                    if (m_networkLost.exchange(false) || !m_networkAvailable.load()) {
                        m_brokerConnected.store(false);
                        return MqttState::Recovering;
                    }
                    if (!m_brokerConnected.load()) { return MqttState::Recovering; }
                    break;
            }
            break;

        case MqttState::Recovering:
            switch (event)
            {
                case MqttEvent::Start:
                case MqttEvent::NetworkChanged:
                case MqttEvent::BrokerChanged:
                case MqttEvent::Timeout:
                    if (m_networkLost.exchange(false)) { m_brokerConnected.store(false); }
                    if (!m_networkAvailable.load()) {
                        m_brokerConnected.store(false);
                        break;
                    }
                    if (m_brokerConnected.load()) { return MqttState::Connected; }
                    if (event == MqttEvent::NetworkChanged || m_recovery.remaining() == 0) {
                        return MqttState::WaitingForConnection;
                    }
                    break;
            }
            break;
    }
    return state;
}

void MqttHandler::on_state_enter(MqttState next)
{
    switch (next)
    {
        case MqttState::Idle:
            m_notifiedConnected = false;
            m_disconnectedSubject.notify();
            break;
        case MqttState::WaitingForConnection:
            m_notifiedConnected = false;
            m_disconnectedSubject.notify();
            beginAttempt();
            break;
        case MqttState::Recovering:
            m_notifiedConnected = false;
            m_disconnectedSubject.notify();
            m_attemptActive = false;
            // This runs in our worker, never in the SDK's MQTT callback task.
            if (m_clientStarted) { esp_mqtt_client_disconnect(m_client); }
            m_recovery.recover();
            ESP_LOGI(TAG, "Recovering; waiting for network or retry in %u ms",
                    static_cast<unsigned>(m_recovery.remaining() * portTICK_PERIOD_MS));
            break;
        case MqttState::Connected:
        {
            ESP_LOGI(TAG, "Connected to MQTT broker %s:%u", CONFIG_GOFKUCAM_MQTT_BROKER_ADDRESS,
                    static_cast<unsigned>(CONFIG_GOFKUCAM_MQTT_BROKER_PORT));
            m_attemptActive = false;
            m_recovery.connected();
            const int message = esp_mqtt_client_subscribe(m_client, "CatStatus", 1);
            if (message < 0)
            {
                ESP_LOGW(TAG, "Could not subscribe to CatStatus");
                m_notifiedConnected = false;
                m_disconnectedSubject.notify();
                m_brokerConnected.store(false);
                postEvent(MqttEvent::BrokerChanged);
            }
            else
            {
                ESP_LOGI(TAG, "Subscribed to CatStatus, msg_id=%d", message);
                m_notifiedConnected = true;
                m_connectedSubject.notify();
            }
            break;
        }
    }
}

TickType_t MqttHandler::event_wait_ticks() const
{
    return m_networkAvailable.load() ? m_recovery.remaining() : portMAX_DELAY;
}

void MqttHandler::on_timeout()
{
    handle_event(MqttEvent::Timeout);
}
