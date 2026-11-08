#pragma once

#include "ActiveObject.hpp"
#include "RecoveryTimer.hpp"
#include "Subject.hpp"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"

enum class ConnectionEvent 
{
      Start
    , AddressChanged
    , Disconnected
    , Timeout
};

enum class ConnectionState
{
      Idle
    , WaitingForConnection
    , Connected
    , Recovering
};

class ConnectionHandler : public ActiveObject<ConnectionEvent, ConnectionState, 16> {
public:
    ConnectionHandler();

    // Start asynchronous connection/recovery after netif/event-loop initialization.
    esp_err_t connect();
    // Subscribe/unsubscribe observers for network connectivity signals.
    void subscribe(IObserver *observer);
    void unsubscribe(IObserver *observer);

protected:
    ConnectionState handle_state_event(ConnectionState state, const ConnectionEvent &event) override;
    void on_state_enter(ConnectionState state) override;
    TickType_t event_wait_ticks() const override;
    void on_timeout() override;

private:
    esp_err_t beginAttempt();
    esp_err_t initializeWifi();
    esp_err_t configureStaticIp();
    static void network_event_handler(void *argument, esp_event_base_t base, std::int32_t id, void *data);
    bool hasAddress() const;

    RecoveryTimer m_recovery;
    std::atomic<unsigned> m_addresses{0};
    std::atomic<bool> m_attemptFailed{false};
    std::atomic<bool> m_wifiAssociated{false};
    esp_netif_t *m_wifiNetif = nullptr;
    bool m_ipHandlerRegistered = false;
    bool m_wifiHandlerRegistered = false;
    bool m_wifiInitialized = false;
    bool m_wifiDefaultsRegistered = false;
    bool m_wifiStarted = false;
    Subject m_connectedSubject{ObservedSignal::NetworkConnected};
    Subject m_disconnectedSubject{ObservedSignal::NetworkDisconnected};
    bool m_notifiedConnected = false;
};
