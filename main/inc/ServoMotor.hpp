#pragma once

#include "esp_err.h"

struct servo_t;

// Owned exclusively by the gate task. Positions are angles from 0 to 180 degrees.
class ServoMotor {
public:
    ServoMotor() = default;
    ~ServoMotor();
    ServoMotor(const ServoMotor &) = delete;
    ServoMotor &operator=(const ServoMotor &) = delete;

    esp_err_t initialize(float initialPosition);
    esp_err_t setPosition(float position);
    bool ready() const { return handle_ != nullptr; }

private:
    servo_t *handle_ = nullptr;
};
