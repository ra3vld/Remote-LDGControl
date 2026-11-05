#include <unity.h>
#include "meter_scale.h"
#include <map>
#include <vector>

void setUp() {}
void tearDown() {}

static bool request(ldg::MeterScale& scale, const char* json) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) return false;
    return ldg::applyMeterScaleRequest(scale, doc.as<JsonVariantConst>());
}

void test_browser_set_and_display_touch_share_the_same_scale() {
    ldg::MeterScale scale;
    TEST_ASSERT_EQUAL_UINT16(1000, scale.snapshot().watts);
    TEST_ASSERT_TRUE(request(scale, "{\"meter_scale\":100}"));
    // The display reads this same model on its next LVGL iteration.
    TEST_ASSERT_EQUAL_UINT16(100, scale.snapshot().watts);
    scale.toggle(); // Display tap.

    JsonDocument event;
    ldg::writeMeterScale(event, scale.snapshot());
    TEST_ASSERT_EQUAL_UINT16(1000, event["meter_scale"].as<uint16_t>());
    TEST_ASSERT_EQUAL_UINT32(2, event["meter_scale_revision"].as<uint32_t>());
}

void test_stale_browser_toggles_the_board_value_and_two_clients_do_not_lose_updates() {
    ldg::MeterScale scale;
    scale.toggle(); // Display selects 100 W before the browser has any snapshot.
    TEST_ASSERT_TRUE(request(scale, "{\"toggle\":true}"));
    TEST_ASSERT_EQUAL_UINT16(1000, scale.snapshot().watts);
    TEST_ASSERT_TRUE(request(scale, "{\"toggle\":true}")); // Second browser.
    TEST_ASSERT_EQUAL_UINT16(100, scale.snapshot().watts);
    TEST_ASSERT_EQUAL_UINT32(3, scale.snapshot().revision);
}

void test_explicit_sets_are_idempotent_and_changes_are_ordered() {
    ldg::MeterScale scale;
    TEST_ASSERT_TRUE(request(scale, "{\"meter_scale\":1000}"));
    TEST_ASSERT_EQUAL_UINT32(0, scale.snapshot().revision);
    TEST_ASSERT_TRUE(request(scale, "{\"meter_scale\":100}"));
    const auto first = scale.snapshot();
    TEST_ASSERT_TRUE(request(scale, "{\"meter_scale\":100}"));
    TEST_ASSERT_EQUAL_UINT32(first.revision, scale.snapshot().revision);
    TEST_ASSERT_TRUE(request(scale, "{\"meter_scale\":1000}"));
    TEST_ASSERT_GREATER_THAN_UINT32(first.revision, scale.snapshot().revision);
}

void test_invalid_requests_never_change_the_display_scale() {
    const char* invalid[] = {
        "{}", "null", "[]", "100", "{\"meter_scale\":null}",
        "{\"meter_scale\":\"100\"}", "{\"meter_scale\":true}",
        "{\"meter_scale\":100.5}", "{\"meter_scale\":-100}",
        "{\"meter_scale\":0}", "{\"meter_scale\":1001}",
        "{\"meter_scale\":65536}", "{\"toggle\":false}",
        "{\"toggle\":1}", "{\"toggle\":\"true\"}",
        "{\"toggle\":null}", "{\"toggle\":true,\"meter_scale\":100}",
        "{\"toggle\":true,\"meter_scale\":null}",
        "{\"meter_logarithmic\":1}", "{\"meter_logarithmic\":null}",
        "{\"meter_scale\":600,\"meter_logarithmic\":\"true\"}",
        "{\"toggle\":true,\"meter_logarithmic\":true}"
    };
    ldg::MeterScale scale;
    scale.toggle();
    for (const char* json : invalid) {
        TEST_ASSERT_FALSE_MESSAGE(request(scale, json), json);
        TEST_ASSERT_EQUAL_UINT16(100, scale.snapshot().watts);
        TEST_ASSERT_EQUAL_UINT32(1, scale.snapshot().revision);
    }
}

void test_custom_range_and_log_mapping_are_shared_idempotent_and_keep_zero_valid() {
    ldg::MeterScale scale;
    TEST_ASSERT_TRUE(request(scale, "{\"meter_scale\":600,\"meter_logarithmic\":true}"));
    TEST_ASSERT_EQUAL_UINT16(600, scale.snapshot().watts);
    TEST_ASSERT_TRUE(scale.snapshot().logarithmic);
    TEST_ASSERT_EQUAL_UINT32(1, scale.snapshot().revision);
    TEST_ASSERT_TRUE(request(scale, "{\"meter_scale\":600,\"meter_logarithmic\":true}"));
    TEST_ASSERT_EQUAL_UINT32(1, scale.snapshot().revision);
    scale.toggle();
    TEST_ASSERT_TRUE(scale.snapshot().logarithmic);
    TEST_ASSERT_TRUE(request(scale, "{\"meter_logarithmic\":false}"));
    TEST_ASSERT_FALSE(scale.snapshot().logarithmic);
    for (const uint16_t maximum : {1, 10, 100, 600, 1000}) for (const bool log : {false,true}) {
        TEST_ASSERT_TRUE(scale.set(maximum, log));
        TEST_ASSERT_EQUAL_FLOAT(0, ldg::meterFraction(0, maximum, log));
        TEST_ASSERT_EQUAL_FLOAT(0, ldg::meterFraction(NAN, maximum, log));
        TEST_ASSERT_EQUAL_FLOAT(1, ldg::meterFraction(maximum * 2, maximum, log));
        for (int tick = 0; tick <= 8; ++tick) {
            const float fraction = float(tick) / 8;
            TEST_ASSERT_FLOAT_WITHIN(0.00001f, fraction,
                ldg::meterFraction(ldg::meterPower(fraction, maximum, log), maximum, log));
        }
    }
    TEST_ASSERT_GREATER_THAN_FLOAT(0.5f, ldg::meterFraction(10,100,true));
}
struct MeterStorage {
    std::map<std::string,std::vector<uint8_t>> data;
    bool fail = false;
    size_t putBytes(const char* key, const void* value, size_t size) {
        if (fail) return 0;
        const auto* bytes=static_cast<const uint8_t*>(value);
        data[key]=std::vector<uint8_t>(bytes,bytes+size); return size;
    }
    size_t getBytesLength(const char* key) { return data[key].size(); }
    size_t getBytes(const char* key, void* value, size_t size) {
        if (data[key].size()!=size) return 0;
        memcpy(value,data[key].data(),size); return size;
    }
};
void test_meter_settings_persist_separately_and_corruption_does_not_replace_defaults() {
    MeterStorage storage;
    storage.data["configV1"]={1,2,3};
    ldg::MeterSettings s; s.watts=600; s.logarithmic=true;
    TEST_ASSERT_TRUE(ldg::writeMeterSettings(storage,s));
    ldg::MeterSettings loaded;
    TEST_ASSERT_TRUE(ldg::readMeterSettings(storage,loaded));
    TEST_ASSERT_EQUAL_UINT16(600,loaded.watts); TEST_ASSERT_TRUE(loaded.logarithmic);
    TEST_ASSERT_EQUAL_UINT(3,storage.data["configV1"].size());
    storage.fail=true; s.watts=50;
    TEST_ASSERT_FALSE(ldg::writeMeterSettings(storage,s));
    TEST_ASSERT_TRUE(ldg::readMeterSettings(storage,loaded));
    TEST_ASSERT_EQUAL_UINT16(600,loaded.watts);
    storage.data["meterScaleV1"][6]=2;
    TEST_ASSERT_FALSE(ldg::readMeterSettings(storage,loaded));
    TEST_ASSERT_EQUAL_UINT16(600,loaded.watts);
}

void test_web_snapshot_remains_consistent_when_the_board_changes_again() {
    ldg::MeterScale scale;
    scale.toggle();
    const auto snapshot = scale.snapshot();
    scale.toggle();
    JsonDocument doc;
    ldg::writeMeterScale(doc, snapshot);
    std::string wire;
    serializeJson(doc, wire);
    JsonDocument received;
    TEST_ASSERT_FALSE(deserializeJson(received, wire));
    TEST_ASSERT_EQUAL_UINT16(100, received["meter_scale"].as<uint16_t>());
    TEST_ASSERT_EQUAL_UINT32(1, received["meter_scale_revision"].as<uint32_t>());
    ldg::writeMeterScale(doc, scale.snapshot());
    TEST_ASSERT_EQUAL_UINT16(1000, doc["meter_scale"].as<uint16_t>());
    TEST_ASSERT_EQUAL_UINT32(2, doc["meter_scale_revision"].as<uint32_t>());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_browser_set_and_display_touch_share_the_same_scale);
    RUN_TEST(test_stale_browser_toggles_the_board_value_and_two_clients_do_not_lose_updates);
    RUN_TEST(test_explicit_sets_are_idempotent_and_changes_are_ordered);
    RUN_TEST(test_invalid_requests_never_change_the_display_scale);
    RUN_TEST(test_web_snapshot_remains_consistent_when_the_board_changes_again);
    RUN_TEST(test_custom_range_and_log_mapping_are_shared_idempotent_and_keep_zero_valid);
    RUN_TEST(test_meter_settings_persist_separately_and_corruption_does_not_replace_defaults);
    return UNITY_END();
}
