#pragma once

#include "ActiveObject.hpp"
#include "RecoveryTimer.hpp"
#include "ServoMotor.hpp"

enum class GateEvent { Start, Open, Close, MqttChanged, VelocityChanged, Timeout };
enum class GateState { Idle, Open, Opening, Closed, Recovering };

class GateController : public ActiveObject<GateEvent, GateState, 16> {
public:
    GateController();
    esp_err_t initialize();
    esp_err_t open();
    // Runtime velocity is shared by HTTP and MQTT opening commands (1..360 deg/s).
    esp_err_t open(unsigned degreesPerSecond);
    esp_err_t close();
    esp_err_t setOpeningVelocity(unsigned degreesPerSecond);
    unsigned openingVelocity() const { return openingVelocity_.load(); }
    void setMqttConnected(bool connected);

    bool mqttConnected() const;
    GateState target() const;
    float commandedPosition() const { return position_.load(); }
    esp_err_t lastError() const { return lastError_.load(); }

protected:
    GateState handle_state_event(GateState state, const GateEvent &event) override;
    void on_state_enter(GateState state) override;
    TickType_t event_wait_ticks() const override;
    void on_timeout() override;

private:
    GateState applyRequestedPosition();
    GateState advanceOpening(TickType_t now, bool force = false);
    GateState servoFailure(esp_err_t error);
    esp_err_t writePosition(float position);
    // The atomic mailbox is authoritative; queued events wake the worker.
    // A disconnect clears a pending close even when the queue is already full.
    static constexpr unsigned MQTT_CONNECTED = 1;
    static constexpr unsigned CLOSE_REQUESTED = 2;
    std::atomic<unsigned> control_{0};
    std::atomic<bool> accepting_{false};
    std::atomic<float> position_{-1};
    std::atomic<unsigned> openingVelocity_;
    std::atomic<esp_err_t> lastError_{ESP_OK};
    ServoMotor servo_;
    RecoveryTimer recovery_;
    bool openingActive_ = false;
    float openingFrom_ = 0;
    unsigned motionVelocity_ = 0;
    TickType_t openingStarted_ = 0;
    TickType_t lastOpeningStep_ = 0;
};
