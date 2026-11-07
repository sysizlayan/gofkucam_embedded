#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

// Start from one owning task after construction. Once started, this object must
// remain alive until device restart (for example, use static storage).
template <typename Event, typename StateType, std::size_t MaxEvents>
class ActiveObject {
    static_assert(std::is_trivially_copyable_v<Event>, "FreeRTOS queues copy events as bytes");
    static_assert(std::is_default_constructible_v<Event>, "Events must be default constructible");
    static_assert(std::is_enum_v<StateType>, "StateType must be an enum");
    static_assert(MaxEvents > 0, "The event queue must have a positive capacity");
    static_assert(MaxEvents <= std::numeric_limits<UBaseType_t>::max(), "Too many events");
    static_assert(MaxEvents <= std::numeric_limits<std::size_t>::max() / sizeof(Event), "Event queue is too large");

public:
    explicit ActiveObject(StateType initialState)
        : queue_(xQueueCreate(static_cast<UBaseType_t>(MaxEvents), sizeof(Event))),
          state_(initialState)
    {
    }

    virtual ~ActiveObject()
    {
        // Only objects whose task was never started may be destroyed.
        configASSERT(task_.load() == nullptr);
        if (queue_ != nullptr) {
            vQueueDelete(queue_);
        }
    }

    ActiveObject(const ActiveObject &) = delete;
    ActiveObject &operator=(const ActiveObject &) = delete;
    ActiveObject(ActiveObject &&) = delete;
    ActiveObject &operator=(ActiveObject &&) = delete;

    // ESP-IDF specifies the task stack size in bytes. Returns false if resources
    // cannot be allocated. Call from the owner, after construction is complete.
    bool start(const char *name = "ActiveObject", std::uint32_t stackSize = 4096,
               UBaseType_t priority = tskIDLE_PRIORITY + 1)
    {
        if (task_.load() != nullptr) {
            return true;
        }
        if (queue_ == nullptr || name == nullptr ||
            stackSize == 0 || priority >= configMAX_PRIORITIES) {
            return false;
        }

        TaskHandle_t task = nullptr;
        if (xTaskCreate(taskEntry, name, stackSize, this, priority, &task) != pdPASS) {
            return false;
        }
        task_.store(task);
        return true;
    }

    // Task context only. False means not started, full, or the wait expired.
    bool postEvent(const Event &event, TickType_t waitTicks = 0)
    {
        return task_.load() != nullptr &&
               xQueueSend(queue_, &event, waitTicks) == pdTRUE;
    }

    // May be observed from other tasks; state changes happen in the worker.
    StateType state() const
    {
        return state_.load();
    }

protected:
    // Block in the owned task until an event or the current state's deadline.
    [[noreturn]] void loop()
    {
        Event event{};
        for (;;) {
            if (xQueueReceive(queue_, &event, event_wait_ticks()) == pdTRUE) {
                handle_event(event);
            } else {
                on_timeout();
            }
        }
    }

    // Template Method: derived classes choose the next state, while this fixed
    // workflow applies the transition. Returning the current state is a no-op.
    // Only the worker should dispatch events; producers use postEvent().
    void handle_event(const Event &event)
    {
        const StateType current = state();
        const StateType next = handle_state_event(current, event);
        if (next == current) {
            return;
        }

        on_state_exit(current);
        state_.store(next);
        on_state_enter(next);
    }

    virtual StateType handle_state_event(StateType current, const Event &event) = 0;

    // A state can set a deadline without a timer task, polling, or a semaphore.
    virtual TickType_t event_wait_ticks() const { return portMAX_DELAY; }
    virtual void on_timeout() {}

    // Hooks run on state changes. The initial state is set by the constructor.
    virtual void on_state_exit(StateType state)
    {
        (void)state;
    }

    virtual void on_state_enter(StateType state)
    {
        (void)state;
    }

private:
    static void taskEntry(void *argument)
    {
        static_cast<ActiveObject *>(argument)->loop();
    }

    QueueHandle_t queue_ = nullptr;
    std::atomic<TaskHandle_t> task_{nullptr};
    std::atomic<StateType> state_;
};
