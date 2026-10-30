#include <unity.h>
#include "meter_scale.h"

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
        "{\"meter_scale\":0}", "{\"meter_scale\":600}",
        "{\"meter_scale\":65536}", "{\"toggle\":false}",
        "{\"toggle\":1}", "{\"toggle\":\"true\"}",
        "{\"toggle\":null}", "{\"toggle\":true,\"meter_scale\":100}",
        "{\"toggle\":true,\"meter_scale\":null}"
    };
    ldg::MeterScale scale;
    scale.toggle();
    for (const char* json : invalid) {
        TEST_ASSERT_FALSE_MESSAGE(request(scale, json), json);
        TEST_ASSERT_EQUAL_UINT16(100, scale.snapshot().watts);
        TEST_ASSERT_EQUAL_UINT32(1, scale.snapshot().revision);
    }
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
    return UNITY_END();
}
