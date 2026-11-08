#include "ServoMotor.hpp"

#include <cmath>

#include "iot_servo.h"
#include "sdkconfig.h"

ServoMotor::~ServoMotor()
{
    if (m_handle != nullptr) { iot_servo_del(m_handle); }
}

esp_err_t ServoMotor::initialize(float initialPosition)
{
    if (!std::isfinite(initialPosition) || initialPosition < 0 || initialPosition > 180 ||
        CONFIG_GOFKUCAM_SERVO_MIN_PULSE_US <= 0 ||
        CONFIG_GOFKUCAM_SERVO_MAX_PULSE_US <= CONFIG_GOFKUCAM_SERVO_MIN_PULSE_US ||
        CONFIG_GOFKUCAM_SERVO_MAX_PULSE_US >= 20000 ||
        CONFIG_GOFKUCAM_SERVO_MIN_POSITION < 0 ||
        CONFIG_GOFKUCAM_SERVO_MAX_POSITION > 180 ||
        CONFIG_GOFKUCAM_SERVO_MIN_POSITION > CONFIG_GOFKUCAM_SERVO_MAX_POSITION) {
        return ESP_ERR_INVALID_ARG;
    }
    if (m_handle == nullptr) {
        servo_config_t config{};
        config.max_angle = 180;
        config.min_width_us = CONFIG_GOFKUCAM_SERVO_MIN_PULSE_US;
        config.max_width_us = CONFIG_GOFKUCAM_SERVO_MAX_PULSE_US;
        config.freq = 50;
        config.speed_mode = LEDC_LOW_SPEED_MODE;
        config.timer_number = LEDC_TIMER_0;
        config.channel = LEDC_CHANNEL_0;
        config.gpio_num = static_cast<gpio_num_t>(CONFIG_GOFKUCAM_SERVO_GPIO);
        const esp_err_t result = iot_servo_new(&config, &m_handle);
        if (result != ESP_OK) { return result; }
    }
    return setPosition(initialPosition);
}

esp_err_t ServoMotor::setPosition(float position)
{
    if (!std::isfinite(position) || position < 0 || position > 180) { return ESP_ERR_INVALID_ARG; }
    if (m_handle == nullptr) { return ESP_ERR_INVALID_STATE; }
    return iot_servo_write_angle(m_handle, position);
}
