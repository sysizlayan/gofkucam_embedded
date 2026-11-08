#pragma once

#include <memory>
#include <vector>

#include "IObserver.hpp"

class ISubject {
public:
    virtual ~ISubject() = default;

    virtual ObservedSignal signal() const = 0;
    virtual const ObservedPayload &payload() const = 0;
    virtual void setPayload(const ObservedPayload &payload) = 0;
    virtual void subscribe(IObserver *observer) = 0;
    virtual void unsubscribe(IObserver *observer) = 0;
    virtual void notify() = 0;
    virtual void notify(const ObservedPayload &payload) = 0;
};

using SubjectPtr = std::unique_ptr<ISubject>;
