#include "GateController.hpp"

#include <algorithm>

#include "esp_log.h"
#include "sdkconfig.h"

namespace {
constexpr const char *TAG = "gate";
constexpr TickType_t STEP_TICKS = pdMS_TO_TICKS(20) > 0 ? pdMS_TO_TICKS(20) : 1;
}

GateController::GateController()
    : ActiveObject(GateState::Idle), m_openingVelocity(CONFIG_GOFKUCAM_GATE_OPENING_VELOCITY_DPS)
{}

esp_err_t GateController::initialize()
{
    if (m_accepting.load()) { return ESP_OK; }
    if (!start("gate_controller")) { return ESP_ERR_NO_MEM; }
    m_accepting.store(true);
    return postEvent(GateEvent::Start) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t GateController::open()
{
    if (!m_accepting.load()) { return ESP_ERR_INVALID_STATE; }
    m_control.fetch_and(~CLOSE_REQUESTED);
    postEvent(GateEvent::Open);
    return ESP_OK;
}

esp_err_t GateController::open(unsigned degreesPerSecond)
{
    if (!m_accepting.load()) { return ESP_ERR_INVALID_STATE; }
    const esp_err_t result = setOpeningVelocity(degreesPerSecond);
    return result == ESP_OK ? open() : result;
}

esp_err_t GateController::setOpeningVelocity(unsigned degreesPerSecond)
{
    if (degreesPerSecond < 1 || degreesPerSecond > 360) { return ESP_ERR_INVALID_ARG; }
    m_openingVelocity.store(degreesPerSecond);
    postEvent(GateEvent::VelocityChanged);
    return ESP_OK;
}

esp_err_t GateController::close()
{
    if (!m_accepting.load()) { return ESP_ERR_INVALID_STATE; }
    unsigned current = m_control.load();
    do {
        if (!(current & MQTT_CONNECTED)) { return ESP_ERR_INVALID_STATE; }
    } while (!m_control.compare_exchange_weak(current, current | CLOSE_REQUESTED));
    postEvent(GateEvent::Close);
    return ESP_OK;
}

void GateController::setMqttConnected(bool connected)
{
    if (connected) {
        m_control.fetch_or(MQTT_CONNECTED); // Connecting alone never closes the gate.
    } else {
        m_control.store(0); // Forget pending close commands and reopen.
    }
    postEvent(GateEvent::MqttChanged);
}

void GateController::update(ObservedSignal signal, const ObservedPayload &payload)
{
    (void)payload;
    switch (signal) {
    case ObservedSignal::MqttConnected:
        setMqttConnected(true);
        break;
    case ObservedSignal::MqttDisconnected:
        setMqttConnected(false);
        break;
    default:
        break;
    }
}

bool GateController::mqttConnected() const { return (m_control.load() & MQTT_CONNECTED) != 0; }
GateState GateController::target() const
{
    return m_control.load() == (MQTT_CONNECTED | CLOSE_REQUESTED) ? GateState::Closed : GateState::Open;
}

esp_err_t GateController::writePosition(float angle)
{
    const esp_err_t result = m_servo.setPosition(angle);
    if (result == ESP_OK) {
        m_position.store(angle);
        m_lastError.store(ESP_OK);
    }
    return result;
}

GateState GateController::servoFailure(esp_err_t error)
{
    m_openingActive = false;
    m_lastError.store(error);
    m_recovery.recover();
    ESP_LOGW(TAG, "Servo command failed: %s", esp_err_to_name(error));
    return GateState::Recovering;
}

GateState GateController::advanceOpening(TickType_t now, bool force)
{
    if (!force && now - m_lastOpeningStep < STEP_TICKS) { return GateState::Opening; }
    const TickType_t elapsed = now - m_openingStarted;
    const float distance = static_cast<float>(static_cast<double>(elapsed) * m_motionVelocity / configTICK_RATE_HZ);
    const float angle = std::min(static_cast<float>(CONFIG_GOFKUCAM_SERVO_MAX_POSITION), m_openingFrom + distance);
    if (angle != m_position.load()) {
        const esp_err_t result = writePosition(angle);
        if (result != ESP_OK) { return servoFailure(result); }
    }
    m_lastOpeningStep = now;
    if (angle >= CONFIG_GOFKUCAM_SERVO_MAX_POSITION) {
        m_openingActive = false;
        m_recovery.connected();
        return GateState::Open;
    }
    return GateState::Opening;
}

GateState GateController::applyRequestedPosition()
{
    esp_err_t result = ESP_OK;
    if (!m_servo.ready()) {
        // Startup has no known closed position: initialize directly to open.
        result = m_servo.initialize(CONFIG_GOFKUCAM_SERVO_MAX_POSITION);
        if (result != ESP_OK) { return servoFailure(result); }
        m_position.store(CONFIG_GOFKUCAM_SERVO_MAX_POSITION);
        m_lastError.store(ESP_OK);
    } else if (m_position.load() < 0 || m_lastError.load() != ESP_OK) {
        // Validate the last successful target before resuming after a driver error.
        const float angle = m_position.load() < 0 ? CONFIG_GOFKUCAM_SERVO_MAX_POSITION : m_position.load();
        result = writePosition(angle);
        if (result != ESP_OK) { return servoFailure(result); }
    }

    if (target() == GateState::Closed) {
        m_openingActive = false;
        if (m_position.load() != CONFIG_GOFKUCAM_SERVO_MIN_POSITION) {
            result = writePosition(CONFIG_GOFKUCAM_SERVO_MIN_POSITION);
            if (result != ESP_OK) { return servoFailure(result); }
        }
        m_recovery.connected();
        return GateState::Closed;
    }
    if (m_position.load() >= CONFIG_GOFKUCAM_SERVO_MAX_POSITION) {
        m_openingActive = false;
        m_recovery.connected();
        return GateState::Open;
    }

    const TickType_t now = xTaskGetTickCount();
    const unsigned velocity = m_openingVelocity.load();
    if (!m_openingActive) {
        m_openingActive = true;
        m_openingFrom = m_position.load();
        m_motionVelocity = velocity;
        m_openingStarted = m_lastOpeningStep = now;
        m_recovery.connected();
        return GateState::Opening;
    }
    if (velocity != m_motionVelocity) {
        // Reach the current point on the old trajectory, then change its slope.
        const GateState progressed = advanceOpening(now, true);
        if (progressed != GateState::Opening) { return progressed; }
        m_openingFrom = m_position.load();
        m_motionVelocity = velocity;
        m_openingStarted = m_lastOpeningStep = now;
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
                    if (m_recovery.remaining() == 0) { return applyRequestedPosition(); }
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
            ESP_LOGI(TAG, "Gate commanded open at %.2f degrees", static_cast<double>(m_position.load()));
            break;
        case GateState::Opening:
            ESP_LOGI(TAG, "Gate opening at %u degrees/s", m_motionVelocity);
            break;
        case GateState::Closed:
            ESP_LOGI(TAG, "Gate commanded closed at %.2f degrees", static_cast<double>(m_position.load()));
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
    if (m_openingActive) {
        const TickType_t elapsed = xTaskGetTickCount() - m_lastOpeningStep;
        return elapsed >= STEP_TICKS ? 0 : STEP_TICKS - elapsed;
    }
    return m_recovery.remaining();
}
void GateController::on_timeout() { handle_event(GateEvent::Timeout); }
