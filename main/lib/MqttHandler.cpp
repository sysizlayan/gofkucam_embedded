#include "MqttHandler.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"

static constexpr const char *TAG = "mqtt_example";

MqttHandler::MqttHandler(GateController &gate) : ActiveObject(MqttState::Idle), gate_(gate)
{
}

MqttHandler::~MqttHandler()
{
    // MQTT retains this instance as its callback context until device restart.
    configASSERT(client_ == nullptr);
}

esp_err_t MqttHandler::connect()
{
    if (!start("mqtt_handler")) { return ESP_ERR_NO_MEM; }
    return postEvent(MqttEvent::Start) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void MqttHandler::setNetworkAvailable(bool available)
{
    if (!available) { gate_.setMqttConnected(false); }
    const bool previous = networkAvailable_.exchange(available);
    if (previous && !available) { networkLost_.store(true); }
    if (previous != available) {
        postEvent(MqttEvent::NetworkChanged);
    }
}

esp_err_t MqttHandler::createClient()
{
    if (client_ != nullptr) { return ESP_OK; }
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
    client_ = client;
    return ESP_OK;
}

void MqttHandler::beginAttempt()
{
    attemptActive_ = networkAvailable_.load();
    attemptFailed_ = false;
    if (!attemptActive_) { return; }
    recovery_.waitForConnection();
    esp_err_t result = createClient();
    if (result == ESP_OK) {
        result = clientStarted_ ? esp_mqtt_client_reconnect(client_) : esp_mqtt_client_start(client_);
        if (result == ESP_OK) { clientStarted_ = true; }
    }
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Broker attempt failed: %s", esp_err_to_name(result));
        attemptFailed_ = true;
        recovery_.failed();
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
        brokerConnected_.store(networkAvailable_.load());
        postEvent(MqttEvent::BrokerChanged);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        resetGateMessage();
        gate_.setMqttConnected(false);
        brokerConnected_.store(false);
        postEvent(MqttEvent::BrokerChanged);
        break;
    case MQTT_EVENT_SUBSCRIBED:
        if (event->data != nullptr && event->data_len > 0) {
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d, return code=0x%02x ",
                     event->msg_id, static_cast<std::uint8_t>(*event->data));
            if (static_cast<std::uint8_t>(*event->data) >= 0x80) {
                ESP_LOGW(TAG, "Broker rejected the CatStatus subscription");
                gate_.setMqttConnected(false);
                brokerConnected_.store(false);
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
        gate_.setMqttConnected(false);
        brokerConnected_.store(false);
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
    gateMessageLength_ = 0;
    gateMessageReceived_ = 0;
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
        gateMessageLength_ = event->total_data_len;
    }
    if (gateMessageLength_ == 0) { return; }
    if (event->total_data_len != gateMessageLength_ || event->current_data_offset != gateMessageReceived_ ||
        event->data_len <= 0 || event->data == nullptr || event->data_len > gateMessageLength_ - gateMessageReceived_ ||
        (event->topic != nullptr && (event->topic_len != TOPIC_LENGTH ||
                                   std::memcmp(event->topic, TOPIC, sizeof(TOPIC) - 1) != 0))) {
        resetGateMessage();
        return;
    }
    std::memcpy(gateMessage_ + gateMessageReceived_, event->data, static_cast<std::size_t>(event->data_len));
    gateMessageReceived_ += event->data_len;
    if (gateMessageReceived_ != gateMessageLength_) { return; }

    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (gateMessageLength_ == 4 && std::memcmp(gateMessage_, "open", 4) == 0) {
        result = gate_.open();
        ESP_LOGI(TAG, "CatStatus command received: open");
    } else if (gateMessageLength_ == 5 && std::memcmp(gateMessage_, "close", 5) == 0) {
        result = gate_.close();
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
                    if (networkLost_.exchange(false)) {
                        brokerConnected_.store(false);
                        if (attemptActive_) { return MqttState::Recovering; }
                    }
                    if (!networkAvailable_.load()) {
                        brokerConnected_.store(false);
                        return attemptActive_ ? MqttState::Recovering : state;
                    }
                    if (brokerConnected_.load()) { return MqttState::Connected; }
                    if (event == MqttEvent::NetworkChanged && !attemptActive_) { beginAttempt(); }
                    if (attemptFailed_ || (event == MqttEvent::BrokerChanged && attemptActive_) ||
                        (attemptActive_ && recovery_.remaining() == 0)) {
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
                    if (networkLost_.exchange(false) || !networkAvailable_.load()) {
                        brokerConnected_.store(false);
                        return MqttState::Recovering;
                    }
                    if (!brokerConnected_.load()) { return MqttState::Recovering; }
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
                    if (networkLost_.exchange(false)) { brokerConnected_.store(false); }
                    if (!networkAvailable_.load()) {
                        brokerConnected_.store(false);
                        break;
                    }
                    if (brokerConnected_.load()) { return MqttState::Connected; }
                    if (event == MqttEvent::NetworkChanged || recovery_.remaining() == 0) {
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
    if (next == MqttState::WaitingForConnection) {
        beginAttempt();
    } else if (next == MqttState::Recovering) {
        gate_.setMqttConnected(false);
        attemptActive_ = false;
        // This runs in our worker, never in the SDK's MQTT callback task.
        if (clientStarted_) { esp_mqtt_client_disconnect(client_); }
        recovery_.recover();
        ESP_LOGI(TAG, "Recovering; waiting for network or retry in %u ms",
                 static_cast<unsigned>(recovery_.remaining() * portTICK_PERIOD_MS));
    } else if (next == MqttState::Connected) {
        ESP_LOGI(TAG, "Connected to MQTT broker %s:%u", CONFIG_GOFKUCAM_MQTT_BROKER_ADDRESS,
                 static_cast<unsigned>(CONFIG_GOFKUCAM_MQTT_BROKER_PORT));
        attemptActive_ = false;
        recovery_.connected();
        gate_.setMqttConnected(true);
        const int message = esp_mqtt_client_subscribe(client_, "CatStatus", 1);
        if (message < 0) {
            ESP_LOGW(TAG, "Could not subscribe to CatStatus");
            gate_.setMqttConnected(false);
            brokerConnected_.store(false);
            postEvent(MqttEvent::BrokerChanged);
        } else {
            ESP_LOGI(TAG, "Subscribed to CatStatus, msg_id=%d", message);
        }
    }
}

TickType_t MqttHandler::event_wait_ticks() const
{
    return networkAvailable_.load() ? recovery_.remaining() : portMAX_DELAY;
}

void MqttHandler::on_timeout() { handle_event(MqttEvent::Timeout); }
