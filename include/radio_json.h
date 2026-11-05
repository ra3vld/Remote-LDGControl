#pragma once
#include <ArduinoJson.h>
#include "radio_band.h"

namespace ldg {
inline void writeRadioJson(JsonObject object, const RadioState& radio) {
    object["provider"] = radioProviderName(radio.provider);
    object["state"] = radioConnectionName(radio.connection);
    object["receiver"] = radio.receiver;
    object["device"] = radio.device; object["protocol"] = radio.protocol;
    if (radio.rxFrequencyHz) object["rx_frequency_hz"] = radio.rxFrequencyHz;
    else object["rx_frequency_hz"] = nullptr;
    if (radio.frequencyValid) object["tx_frequency_hz"] = radio.txFrequencyHz;
    else object["tx_frequency_hz"] = nullptr;
    object["band"] = radioBandName(radio.frequencyValid ? radio.txFrequencyHz : 0);
    object["rx_tx"] = radio.transmitting ? "tx" : radio.rxKnown ? "rx" : "unknown";
    if (radio.tuneKnown || radio.tuning) object["tuning"] = radio.tuning;
    else object["tuning"] = nullptr;
    if (radio.splitKnown) object["split"] = radio.split;
    else object["split"] = nullptr;
    if (radio.xitKnown) object["xit"] = radio.xit;
    else object["xit"] = nullptr;
    object["xit_offset_hz"] = radio.xitOffset;
    object["modulation"] = radio.modulation;
    object["age_ms"] = radio.ageMs; object["stable_ms"] = radio.stableMs;
    object["error"] = radio.error;
    object["free_heap"] = radio.freeHeap; object["largest_block"] = radio.largestBlock;
    object["stack_free"] = radio.stackFree;
}
inline RadioState readRadioJson(JsonVariantConst object) {
    RadioState radio;
    const char* provider=object["provider"] | "none";
    radio.provider=!strcmp(provider,"tci") ? RadioProvider::Tci : !strcmp(provider,"omnirig") ? RadioProvider::OmniRig : RadioProvider::None;
    const char* connection = object["state"] | "disabled";
    for (unsigned i = 0; i <= unsigned(RadioConnection::Ready); ++i)
        if (!strcmp(connection, radioConnectionName(static_cast<RadioConnection>(i)))) radio.connection = static_cast<RadioConnection>(i);
    radio.receiver = object["receiver"] | 0;
    const char* trx = object["rx_tx"] | "unknown";
    radio.rxKnown = !strcmp(trx,"rx") || !strcmp(trx,"tx");
    radio.transmitting = !strcmp(trx,"tx");
    radio.tuneKnown = object["tuning"].is<bool>(); radio.tuning = object["tuning"] | false;
    radio.splitKnown = object["split"].is<bool>(); radio.split = object["split"] | false;
    radio.xitKnown = object["xit"].is<bool>(); radio.xit = object["xit"] | false;
    radio.xitOffset = object["xit_offset_hz"] | 0;
    radio.rxFrequencyHz = object["rx_frequency_hz"] | uint64_t(0);
    radio.txFrequencyHz = object["tx_frequency_hz"] | uint64_t(0);
    radio.frequencyValid = radio.txFrequencyHz > 0 && radio.txFrequencyHz <= 1000000000000ULL;
    radio.ageMs = object["age_ms"] | uint32_t(0);
    radio.stableMs = object["stable_ms"] | uint32_t(0);
    radio.freeHeap = object["free_heap"] | uint32_t(0);
    radio.largestBlock = object["largest_block"] | uint32_t(0);
    radio.stackFree = object["stack_free"] | uint32_t(0);
    #define LDG_RADIO_COPY(field) do { const char* text = object[#field] | ""; strncpy(radio.field,text,sizeof(radio.field)-1); } while (0)
    LDG_RADIO_COPY(device); LDG_RADIO_COPY(protocol); LDG_RADIO_COPY(modulation); LDG_RADIO_COPY(error);
    #undef LDG_RADIO_COPY
    return radio;
}
}
