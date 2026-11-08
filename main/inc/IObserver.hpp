#pragma once

#include <variant>

enum class ObservedSignal {
      GateOpened
    , GateClosed
    , GateOpening
    , GateClosing
    , MqttConnected
    , MqttDisconnected
    , NetworkConnected
    , NetworkDisconnected
};

using ObservedPayload = std::variant<std::monostate, int, float, unsigned>;

class IObserver {
public:
    virtual ~IObserver() = default;

    virtual void update(ObservedSignal signal, const ObservedPayload &payload = {}) = 0;
};
