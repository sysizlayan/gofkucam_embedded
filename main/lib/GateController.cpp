#include "GateController.hpp"

#include <algorithm>

#include "esp_log.h"
#include "sdkconfig.h"

namespace {
constexpr const char *TAG = "gate";
constexpr TickType_t STEP_TICKS = pdMS_TO_TICKS(20) > 0 ? pdMS_TO_TICKS(20) : 1;
}

GateController::GateController()
    : ActiveObject(GateState::Idle), openingVelocity_(CONFIG_GOFKUCAM_GATE_OPENING_VELOCITY_DPS)
{
}

esp_err_t GateController::initialize()
{
    if (accepting_.load()) { return ESP_OK; }
    if (!start("gate_controller")) { return ESP_ERR_NO_MEM; }
    accepting_.store(true);
    return postEvent(GateEvent::Start) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t GateController::open()
{
    if (!accepting_.load()) { return ESP_ERR_INVALID_STATE; }
    control_.fetch_and(~CLOSE_REQUESTED);
    postEvent(GateEvent::Open);
    return ESP_OK;
}

esp_err_t GateController::open(unsigned degreesPerSecond)
{
    if (!accepting_.load()) { return ESP_ERR_INVALID_STATE; }
    const esp_err_t result = setOpeningVelocity(degreesPerSecond);
    return result == ESP_OK ? open() : result;
}

esp_err_t GateController::setOpeningVelocity(unsigned degreesPerSecond)
{
    if (degreesPerSecond < 1 || degreesPerSecond > 360) { return ESP_ERR_INVALID_ARG; }
    openingVelocity_.store(degreesPerSecond);
    postEvent(GateEvent::VelocityChanged);
    return ESP_OK;
}

esp_err_t GateController::close()
{
    if (!accepting_.load()) { return ESP_ERR_INVALID_STATE; }
    unsigned current = control_.load();
    do {
        if (!(current & MQTT_CONNECTED)) { return ESP_ERR_INVALID_STATE; }
    } while (!control_.compare_exchange_weak(current, current | CLOSE_REQUESTED));
    postEvent(GateEvent::Close);
    return ESP_OK;
}

void GateController::setMqttConnected(bool connected)
{
    if (connected) {
        control_.fetch_or(MQTT_CONNECTED); // Connecting alone never closes the gate.
    } else {
        control_.store(0); // Forget pending close commands and reopen.
    }
    postEvent(GateEvent::MqttChanged);
}

bool GateController::mqttConnected() const { return (control_.load() & MQTT_CONNECTED) != 0; }
GateState GateController::target() const
{
    return control_.load() == (MQTT_CONNECTED | CLOSE_REQUESTED) ? GateState::Closed : GateState::Open;
}

esp_err_t GateController::writePosition(float angle)
{
    const esp_err_t result = servo_.setPosition(angle);
    if (result == ESP_OK) {
        position_.store(angle);
        lastError_.store(ESP_OK);
    }
    return result;
}

GateState GateController::servoFailure(esp_err_t error)
{
    openingActive_ = false;
    lastError_.store(error);
    recovery_.recover();
    ESP_LOGW(TAG, "Servo command failed: %s", esp_err_to_name(error));
    return GateState::Recovering;
}

GateState GateController::advanceOpening(TickType_t now, bool force)
{
    if (!force && now - lastOpeningStep_ < STEP_TICKS) { return GateState::Opening; }
    const TickType_t elapsed = now - openingStarted_;
    const float distance = static_cast<float>(static_cast<double>(elapsed) * motionVelocity_ / configTICK_RATE_HZ);
    const float angle = std::min(static_cast<float>(CONFIG_GOFKUCAM_SERVO_MAX_POSITION), openingFrom_ + distance);
    if (angle != position_.load()) {
        const esp_err_t result = writePosition(angle);
        if (result != ESP_OK) { return servoFailure(result); }
    }
    lastOpeningStep_ = now;
    if (angle >= CONFIG_GOFKUCAM_SERVO_MAX_POSITION) {
        openingActive_ = false;
        recovery_.connected();
        return GateState::Open;
    }
    return GateState::Opening;
}

GateState GateController::applyRequestedPosition()
{
    esp_err_t result = ESP_OK;
    if (!servo_.ready()) {
        // Startup has no known closed position: initialize directly to open.
        result = servo_.initialize(CONFIG_GOFKUCAM_SERVO_MAX_POSITION);
        if (result != ESP_OK) { return servoFailure(result); }
        position_.store(CONFIG_GOFKUCAM_SERVO_MAX_POSITION);
        lastError_.store(ESP_OK);
    } else if (position_.load() < 0 || lastError_.load() != ESP_OK) {
        // Validate the last successful target before resuming after a driver error.
        const float angle = position_.load() < 0 ? CONFIG_GOFKUCAM_SERVO_MAX_POSITION : position_.load();
        result = writePosition(angle);
        if (result != ESP_OK) { return servoFailure(result); }
    }

    if (target() == GateState::Closed) {
        openingActive_ = false;
        if (position_.load() != CONFIG_GOFKUCAM_SERVO_MIN_POSITION) {
            result = writePosition(CONFIG_GOFKUCAM_SERVO_MIN_POSITION);
            if (result != ESP_OK) { return servoFailure(result); }
        }
        recovery_.connected();
        return GateState::Closed;
    }
    if (position_.load() >= CONFIG_GOFKUCAM_SERVO_MAX_POSITION) {
        openingActive_ = false;
        recovery_.connected();
        return GateState::Open;
    }

    const TickType_t now = xTaskGetTickCount();
    const unsigned velocity = openingVelocity_.load();
    if (!openingActive_) {
        openingActive_ = true;
        openingFrom_ = position_.load();
        motionVelocity_ = velocity;
        openingStarted_ = lastOpeningStep_ = now;
        recovery_.connected();
        return GateState::Opening;
    }
    if (velocity != motionVelocity_) {
        // Reach the current point on the old trajectory, then change its slope.
        const GateState progressed = advanceOpening(now, true);
        if (progressed != GateState::Opening) { return progressed; }
        openingFrom_ = position_.load();
        motionVelocity_ = velocity;
        openingStarted_ = lastOpeningStep_ = now;
    }
    return advanceOpening(now);
}

GateState GateController::handle_state_event(GateState state, const GateEvent &event)
{
    switch (state)
    {
        case GateState::Idle:
            switch (event)
            {
                case GateEvent::Start:
                case GateEvent::Open:
                case GateEvent::Close:
                case GateEvent::MqttChanged:
                case GateEvent::VelocityChanged:
                case GateEvent::Timeout:
                    return applyRequestedPosition();
            }
            break;

        case GateState::Open:
            switch (event)
            {
                case GateEvent::Start:
                case GateEvent::Open:
                case GateEvent::Close:
                case GateEvent::MqttChanged:
                case GateEvent::VelocityChanged:
                case GateEvent::Timeout:
                    return applyRequestedPosition();
            }
            break;

        case GateState::Opening:
            switch (event)
            {
                case GateEvent::Start:
                case GateEvent::Open:
                case GateEvent::Close:
                case GateEvent::MqttChanged:
                case GateEvent::VelocityChanged:
                case GateEvent::Timeout:
                    return applyRequestedPosition();
            }
            break;

        case GateState::Closed:
            switch (event)
            {
                case GateEvent::Start:
                case GateEvent::Open:
                case GateEvent::Close:
                case GateEvent::MqttChanged:
                case GateEvent::VelocityChanged:
                case GateEvent::Timeout:
                    return applyRequestedPosition();
            }
            break;

        case GateState::Recovering:
            switch (event)
            {
                case GateEvent::Start:
                case GateEvent::Open:
                case GateEvent::Close:
                case GateEvent::MqttChanged:
                case GateEvent::VelocityChanged:
                case GateEvent::Timeout:
                    if (recovery_.remaining() == 0) { return applyRequestedPosition(); }
                    break;
            }
            break;
    }
    return state;
}

void GateController::on_state_enter(GateState state)
{
    switch (state)
    {
        case GateState::Open:
            ESP_LOGI(TAG, "Gate commanded open at %.2f degrees", static_cast<double>(position_.load()));
            break;
        case GateState::Opening:
            ESP_LOGI(TAG, "Gate opening at %u degrees/s", motionVelocity_);
            break;
        case GateState::Closed:
            ESP_LOGI(TAG, "Gate commanded closed at %.2f degrees", static_cast<double>(position_.load()));
            break;
        case GateState::Recovering:
            ESP_LOGW(TAG, "Retrying servo control after recovery delay");
            break;
        case GateState::Idle:
            break;
    }
}

TickType_t GateController::event_wait_ticks() const
{
    if (openingActive_) {
        const TickType_t elapsed = xTaskGetTickCount() - lastOpeningStep_;
        return elapsed >= STEP_TICKS ? 0 : STEP_TICKS - elapsed;
    }
    return recovery_.remaining();
}
void GateController::on_timeout() { handle_event(GateEvent::Timeout); }
