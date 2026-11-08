#pragma once

#include <algorithm>
#include <vector>

#include "ISubject.hpp"

class Subject : public ISubject {
public:
    explicit Subject(ObservedSignal signal, const ObservedPayload &payload = {});
    ~Subject() override = default;

    Subject(const Subject &) = delete;
    Subject &operator=(const Subject &) = delete;
    Subject(Subject &&) = delete;
    Subject &operator=(Subject &&) = delete;

    ObservedSignal signal() const override { return m_signal; }
    const ObservedPayload &payload() const override { return m_payload; }
    void setPayload(const ObservedPayload &payload) override { m_payload = payload; }

    void subscribe(IObserver *observer) override;
    void unsubscribe(IObserver *observer) override;
    void notify() override;
    void notify(const ObservedPayload &payload) override;

private:
    ObservedSignal m_signal;
    ObservedPayload m_payload;
    std::vector<IObserver *> m_observers;
};
