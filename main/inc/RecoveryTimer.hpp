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
        arm(pdMS_TO_TICKS(m_backoffMs));
        m_backoffMs = m_backoffMs >= 15000 ? 30000 : m_backoffMs * 2;
    }
    void connected()
    {
        m_backoffMs = 1000;
        m_duration = portMAX_DELAY;
    }
    TickType_t remaining() const
    {
        if (m_duration == portMAX_DELAY) {
            return portMAX_DELAY;
        }
        const TickType_t elapsed = xTaskGetTickCount() - m_started;
        return elapsed >= m_duration ? 0 : m_duration - elapsed;
    }

private:
    void arm(TickType_t duration)
    {
        m_started = xTaskGetTickCount();
        m_duration = duration;
    }
    TickType_t m_started = 0;
    TickType_t m_duration = portMAX_DELAY;
    unsigned m_backoffMs = 1000;
};
