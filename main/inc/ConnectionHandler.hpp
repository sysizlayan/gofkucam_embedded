#pragma once

#include "ActiveObject.hpp"
#include "RecoveryTimer.hpp"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"

enum class ConnectionEvent { Start, AddressChanged, Disconnected, Timeout };

enum class ConnectionState {
    Idle,
    WaitingForConnection,
    Connected,
    Recovering,
};

class ConnectionHandler : public ActiveObject<ConnectionEvent, ConnectionState, 16> {
public:
    ConnectionHandler();

    // Start asynchronous connection/recovery after netif/event-loop initialization.
    esp_err_t connect();
    using ConnectionListener = void (*)(void *context, bool connected);
    // Set before connect(); notifications execute in this handler's task.
    void setConnectionListener(ConnectionListener listener, void *context);

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
    void notifyConnection(bool connected);
    bool hasAddress() const;

    RecoveryTimer recovery_;
    std::atomic<unsigned> addresses_{0};
    std::atomic<bool> attemptFailed_{false};
    std::atomic<bool> wifiAssociated_{false};
    esp_netif_t *wifiNetif_ = nullptr;
    bool ipHandlerRegistered_ = false;
    bool wifiHandlerRegistered_ = false;
    bool wifiInitialized_ = false;
    bool wifiDefaultsRegistered_ = false;
    bool wifiStarted_ = false;
    ConnectionListener listener_ = nullptr;
    void *listenerContext_ = nullptr;
    bool notifiedConnected_ = false;
};
