#pragma once

#include "ActiveObject.hpp"
#include "GateController.hpp"
#include "IObserver.hpp"
#include "RecoveryTimer.hpp"
#include "Subject.hpp"
#include "mqtt_client.h"

enum class MqttEvent { Start, NetworkChanged, BrokerChanged, Timeout };

enum class MqttState {
    Idle,
    WaitingForConnection,
    Connected,
    Recovering,
};

class MqttHandler : public ActiveObject<MqttEvent, MqttState, 16>, public IObserver {
public:
    explicit MqttHandler(GateController &gate);
    ~MqttHandler() override;

    // Start the task; broker attempts wait until setNetworkAvailable(true).
    esp_err_t connect();
    void setNetworkAvailable(bool available);
    void update(ObservedSignal signal, const ObservedPayload &payload = {}) override;
    void subscribe(IObserver *observer);
    void unsubscribe(IObserver *observer);

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
    void notifyConnection(bool connected);

    GateController &m_gate;
    esp_mqtt_client_handle_t m_client = nullptr;
    bool m_clientStarted = false;
    bool m_attemptActive = false;
    bool m_attemptFailed = false;
    RecoveryTimer m_recovery;
    std::atomic<bool> m_networkAvailable{false};
    std::atomic<bool> m_networkLost{false};
    std::atomic<bool> m_brokerConnected{false};
    Subject m_connectedSubject{ObservedSignal::MqttConnected};
    Subject m_disconnectedSubject{ObservedSignal::MqttDisconnected};
    bool m_notifiedConnected = false;
    // MQTT callback payloads can be fragmented and are not null-terminated.
    char m_gateMessage[5]{};
    int m_gateMessageLength = 0;
    int m_gateMessageReceived = 0;
};
