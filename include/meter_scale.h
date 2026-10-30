#pragma once

#include <ArduinoJson.h>
#include <atomic>
#include <stdint.h>

namespace ldg {

// One runtime scale for the local display and every browser. Each actual
// change flips between 1000 W and 100 W, so one atomic revision encodes both
// the value and its ordering without a torn value/revision snapshot.
class MeterScale {
public:
    struct Snapshot {
        uint16_t watts;
        uint32_t revision;
    };

    Snapshot snapshot() const {
        const uint32_t revision = m_revision.load();
        return {uint16_t((revision & 1) ? 100 : 1000), revision};
    }

    bool set(uint16_t watts) {
        if (watts != 100 && watts != 1000) return false;
        uint32_t revision = m_revision.load();
        const uint32_t parity = watts == 100 ? 1 : 0;
        while ((revision & 1) != parity &&
               !m_revision.compare_exchange_weak(revision, revision + 1)) {}
        return true;
    }

    void toggle() { m_revision.fetch_add(1); }

private:
    std::atomic<uint32_t> m_revision{0};
};

inline void writeMeterScale(JsonDocument& doc, const MeterScale::Snapshot& scale) {
    doc["meter_scale"] = scale.watts;
    doc["meter_scale_revision"] = scale.revision;
}

// An explicit set is idempotent. A toggle is evaluated on the board, so a
// newly opened or temporarily stale browser never guesses the current scale.
inline bool applyMeterScaleRequest(MeterScale& scale, JsonVariantConst request) {
    if (!request.is<JsonObjectConst>()) return false;
    const auto watts = request["meter_scale"];
    const auto toggle = request["toggle"];
    if (!toggle.isUnbound()) {
        if (!watts.isUnbound() || !toggle.is<bool>() || !toggle.as<bool>()) return false;
        scale.toggle();
        return true;
    }
    return watts.is<uint16_t>() && scale.set(watts.as<uint16_t>());
}

} // namespace ldg
