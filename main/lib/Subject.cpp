#include "Subject.hpp"

Subject::Subject(ObservedSignal signal, const ObservedPayload &payload)
    : m_signal(signal), m_payload(payload)
{
}

void Subject::subscribe(IObserver *observer)
{
    if (observer == nullptr) { return; }
    auto it = std::find(m_observers.begin(), m_observers.end(), observer);
    if (it == m_observers.end()) {
        m_observers.push_back(observer);
    }
}

void Subject::unsubscribe(IObserver *observer)
{
    auto it = std::find(m_observers.begin(), m_observers.end(), observer);
    if (it != m_observers.end()) {
        m_observers.erase(it);
    }
}

void Subject::notify()
{
    for (auto *observer : m_observers) {
        observer->update(m_signal, m_payload);
    }
}

void Subject::notify(const ObservedPayload &payload)
{
    m_payload = payload;
    for (auto *observer : m_observers) {
        observer->update(m_signal, m_payload);
    }
}
