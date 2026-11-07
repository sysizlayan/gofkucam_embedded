#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Used only by the owning active-object task. Tick subtraction handles wraparound.
class RecoveryTimer {
public:
    void waitForConnection() { arm(pdMS_TO_TICKS(30000)); }
    void failed() { arm(0); }
    void recover()
    {
        arm(pdMS_TO_TICKS(backoffMs_));
        backoffMs_ = backoffMs_ >= 15000 ? 30000 : backoffMs_ * 2;
    }
    void connected()
    {
        backoffMs_ = 1000;
        duration_ = portMAX_DELAY;
    }
    TickType_t remaining() const
    {
        if (duration_ == portMAX_DELAY) {
            return portMAX_DELAY;
        }
        const TickType_t elapsed = xTaskGetTickCount() - started_;
        return elapsed >= duration_ ? 0 : duration_ - elapsed;
    }

private:
    void arm(TickType_t duration)
    {
        started_ = xTaskGetTickCount();
        duration_ = duration;
    }
    TickType_t started_ = 0;
    TickType_t duration_ = portMAX_DELAY;
    unsigned backoffMs_ = 1000;
};
