#pragma once

#include "ActiveObject.hpp"
#include "GateController.hpp"
#include "RecoveryTimer.hpp"
#include "mqtt_client.h"

enum class MqttEvent { Start, NetworkChanged, BrokerChanged, Timeout };

enum class MqttState {
    Idle,
    WaitingForConnection,
    Connected,
    Recovering,
};

class MqttHandler : public ActiveObject<MqttEvent, MqttState, 16> {
public:
    explicit MqttHandler(GateController &gate);
    ~MqttHandler() override;

    // Start the task; broker attempts wait until setNetworkAvailable(true).
    esp_err_t connect();
    void setNetworkAvailable(bool available);

protected:
    MqttState handle_state_event(MqttState state, const MqttEvent &event) override;
    void on_state_enter(MqttState state) override;
    TickType_t event_wait_ticks() const override;
    void on_timeout() override;

private:
    static void mqtt_event_handler(void *handlerArgs, esp_event_base_t base,
                                   std::int32_t eventId, void *eventData);
    void handle_mqtt_event(esp_mqtt_event_handle_t event);
    void beginAttempt();
    esp_err_t createClient();
    void handleGateMessage(esp_mqtt_event_handle_t event);
    void resetGateMessage();

    GateController &gate_;
    esp_mqtt_client_handle_t client_ = nullptr;
    bool clientStarted_ = false;
    bool attemptActive_ = false;
    bool attemptFailed_ = false;
    RecoveryTimer recovery_;
    std::atomic<bool> networkAvailable_{false};
    std::atomic<bool> networkLost_{false};
    std::atomic<bool> brokerConnected_{false};
    // MQTT callback payloads can be fragmented and are not null-terminated.
    char gateMessage_[5]{};
    int gateMessageLength_ = 0;
    int gateMessageReceived_ = 0;
};
