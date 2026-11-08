#pragma once
#include <atomic>
#include <stdint.h>

namespace ldg {
struct MqttCryptoStats {
    uint32_t checkpoints = 0, yields = 0, maxGapUs = 0;
    bool active = false;
};

// One MQTT worker owns the clock. Other tasks only observe atomic counters.
class MqttCryptoBudget {
public:
    static constexpr uint32_t SliceUs = 2000;
    void begin(uintptr_t task, uint32_t now) {
        m_since = now;
        m_task.store(task);
    }
    void end() { m_task.store(0); }
    MqttCryptoStats stats() const {
        MqttCryptoStats value;
        value.checkpoints = m_checkpoints.load();
        value.yields = m_yields.load();
        value.maxGapUs = m_maxGapUs.load();
        value.active = m_task.load() != 0;
        return value;
    }
    template<class Clock, class Pause>
    void released(uintptr_t task, Clock clock, Pause pause) {
        if (!task || m_task.load() != task) return;
        const uint32_t elapsed = uint32_t(clock() - m_since);
        m_checkpoints.fetch_add(1);
        if (elapsed < SliceUs) return;
        if (elapsed > m_maxGapUs.load()) m_maxGapUs.store(elapsed);
        m_yields.fetch_add(1);
        pause();
        m_since = clock();
    }
private:
    std::atomic<uintptr_t> m_task{0};
    std::atomic<uint32_t> m_checkpoints{0}, m_yields{0}, m_maxGapUs{0};
    uint32_t m_since = 0;
};

#if defined(LDG_MQTT_CRYPTO_BUDGET) && LDG_MQTT_CRYPTO_BUDGET
void beginMqttCryptoBudget();
void endMqttCryptoBudget();
MqttCryptoStats mqttCryptoStats();
#else
inline void beginMqttCryptoBudget() {}
inline void endMqttCryptoBudget() {}
inline MqttCryptoStats mqttCryptoStats() { return {}; }
#endif

class MqttCryptoScope {
public:
    explicit MqttCryptoScope(bool tls) : m_active(tls) {
        if (m_active) beginMqttCryptoBudget();
    }
    ~MqttCryptoScope() { if (m_active) endMqttCryptoBudget(); }
    MqttCryptoScope(const MqttCryptoScope&) = delete;
    MqttCryptoScope& operator=(const MqttCryptoScope&) = delete;
private:
    bool m_active;
};
}
