#include <unity.h>
#include <map>
#include <string>
#include <vector>
#include "antenna_automation.h"
#include "config_store.h"

using namespace ldg;
void setUp() {}
void tearDown() {}
static AntennaSettings settings() {
    AntennaSettings s;
    s.enabled = 1; s.bands[BAND_40M] = ANT_A; s.bands[BAND_20M] = ANT_B;
    return s;
}
static void packet(AntennaAutomation& a, tuner_band_t band, double watts, uint32_t now = 0, uint32_t revision = 0) {
    tuner_meter_t meter{};
    meter.band = band; meter.forward_power_watts = watts;
    meter.forward_power_raw = watts == 0 ? 0 : 100;
    a.observe(meter, now, revision);
}
static void band(AntennaAutomation& a, tuner_band_t b, double watts = 40, uint32_t now = 0) {
    for (int i = 0; i < 3; ++i) packet(a, b, watts, now);
}
static AntennaAutomation ready() {
    AntennaAutomation a;
    a.configure(settings()); a.probeComplete(true);
    band(a, BAND_40M);
    return a;
}
void test_startup_requires_actual_fresh_zero_and_only_runs_once() {
    AntennaAutomation a;
    TEST_ASSERT_TRUE(a.poll(0, 0, false, ANT_UNKNOWN).action == AntennaAutomation::Action::None);
    packet(a, BAND_40M, 40);
    TEST_ASSERT_TRUE(a.poll(0, 0, false, ANT_UNKNOWN).action == AntennaAutomation::Action::None);
    TEST_ASSERT_TRUE(a.init() == AntennaInit::WaitingRfOff);
    packet(a, BAND_UNKNOWN, NAN);
    TEST_ASSERT_TRUE(a.poll(0, 0, false, ANT_UNKNOWN).action == AntennaAutomation::Action::None);
    packet(a, BAND_UNKNOWN, 0);
    TEST_ASSERT_TRUE(a.poll(501, 0, false, ANT_UNKNOWN).action == AntennaAutomation::Action::None);
    packet(a, BAND_UNKNOWN, 0, 502);
    TEST_ASSERT_TRUE(a.poll(502, 0, true, ANT_UNKNOWN).action == AntennaAutomation::Action::None);
    TEST_ASSERT_TRUE(a.poll(502, 0, false, ANT_UNKNOWN).action == AntennaAutomation::Action::Probe);
    a.probeComplete(true);
    for (int i = 0; i < 10; ++i)
        TEST_ASSERT_TRUE(a.poll(502, 0, false, ANT_A).action == AntennaAutomation::Action::None);
}
void test_first_band_is_baseline_and_manual_choice_survives_rf_pauses() {
    auto a = ready();
    TEST_ASSERT_EQUAL_INT(BAND_40M, a.currentBand());
    TEST_ASSERT_TRUE(a.poll(0, 0, false, ANT_B).action == AntennaAutomation::Action::None);
    band(a, BAND_20M);
    const auto request = a.poll(0, 0, false, ANT_A);
    TEST_ASSERT_TRUE(request.action == AntennaAutomation::Action::Select);
    TEST_ASSERT_EQUAL_INT(ANT_B, request.antenna);
    a.selectionComplete(true);
    a.manualStarted(); a.manualAntennaComplete(true);
    packet(a, BAND_UNKNOWN, 0, 10000);
    band(a, BAND_20M, 40, 10001);
    TEST_ASSERT_TRUE(a.poll(10001, 1, false, ANT_A).action == AntennaAutomation::Action::None);
    band(a, BAND_40M, 40, 10002);
    a.poll(10002, 0, false, ANT_A); // Already selected.
    band(a, BAND_20M, 40, 10003);
    TEST_ASSERT_TRUE(a.poll(10003, 0, false, ANT_A).action == AntennaAutomation::Action::Select);
}
void test_100_watts_or_a_high_sample_consumes_transition_without_delayed_retry() {
    for (double watts : {99.9, 100.0, 150.0}) {
        auto a = ready();
        band(a, BAND_20M, watts);
        const auto action = a.poll(0, 0, false, ANT_A).action;
        TEST_ASSERT_TRUE(action == (watts < 100 ? AntennaAutomation::Action::Select : AntennaAutomation::Action::None));
        band(a, BAND_20M, 40);
        TEST_ASSERT_TRUE(a.poll(0, 0, false, ANT_A).action == AntennaAutomation::Action::None);
    }
    auto a = ready();
    packet(a, BAND_20M, 150); packet(a, BAND_20M, 40); packet(a, BAND_20M, 40);
    TEST_ASSERT_TRUE(a.automatic() == AntennaAuto::HighPower);
    TEST_ASSERT_TRUE(a.poll(0, 0, false, ANT_A).action == AntennaAutomation::Action::None);
}
void test_execution_rechecks_power_age_and_manual_priority() {
    for (int scenario = 0; scenario < 4; ++scenario) {
        auto a = ready(); band(a, BAND_20M);
        if (scenario == 0) packet(a, BAND_20M, 100);
        TEST_ASSERT_TRUE(a.poll(scenario == 1 ? 501 : 0, scenario == 2 ? 1 : 0,
            scenario == 3, ANT_A).action == AntennaAutomation::Action::None);
        band(a, BAND_20M);
        TEST_ASSERT_TRUE(a.poll(0, 0, false, ANT_A).action == AntennaAutomation::Action::None);
    }
}
void test_manual_command_during_confirmation_does_not_disable_debounce() {
    auto a = ready();
    packet(a, BAND_20M, 40); a.manualStarted();
    TEST_ASSERT_EQUAL_INT(BAND_40M, a.currentBand());
    packet(a, BAND_20M, 40); packet(a, BAND_20M, 40);
    TEST_ASSERT_EQUAL_INT(BAND_20M, a.currentBand());
    TEST_ASSERT_TRUE(a.automatic() == AntennaAuto::Manual);
    TEST_ASSERT_TRUE(a.poll(0, 1, false, ANT_A).action == AntennaAutomation::Action::None);
    a = ready();
    packet(a, BAND_20M, 40); a.manualStarted();
    packet(a, BAND_UNKNOWN, 0, 10000);
    band(a, BAND_20M, 40, 10001);
    TEST_ASSERT_TRUE(a.automatic() == AntennaAuto::Manual);
    TEST_ASSERT_TRUE(a.poll(10001, 1, false, ANT_A).action == AntennaAutomation::Action::None);
}
void test_unknown_invalid_and_stale_frames_break_confirmation_across_wrap() {
    auto a = ready();
    packet(a, BAND_20M, 40); packet(a, BAND_20M, 40);
    a.invalidateFrame(); packet(a, BAND_20M, 40);
    TEST_ASSERT_EQUAL_INT(BAND_40M, a.currentBand());
    packet(a, BAND_UNKNOWN, 40); packet(a, BAND_20M, 40); packet(a, BAND_20M, 40);
    TEST_ASSERT_EQUAL_INT(BAND_40M, a.currentBand());
    packet(a, BAND_20M, 40, 1000);
    TEST_ASSERT_EQUAL_INT(BAND_40M, a.currentBand());
    a = ready();
    packet(a, BAND_20M, 40, 0xfffffffeu);
    packet(a, BAND_20M, 40, 0xffffffffu);
    packet(a, BAND_20M, 40, 0);
    TEST_ASSERT_TRUE(a.poll(1, 0, false, ANT_A).action == AntennaAutomation::Action::Select);
}
void test_invalid_power_is_skipped_and_settings_changes_do_not_apply_to_current_band() {
    auto a = ready();
    packet(a, BAND_20M, NAN); packet(a, BAND_20M, 40); packet(a, BAND_20M, 40);
    TEST_ASSERT_TRUE(a.automatic() == AntennaAuto::InvalidPower);
    TEST_ASSERT_TRUE(a.poll(0, 0, false, ANT_A).action == AntennaAutomation::Action::None);
    auto s = settings(); s.bands[BAND_20M] = ANT_A;
    a.configure(s); band(a, BAND_20M);
    TEST_ASSERT_TRUE(a.poll(0, 0, false, ANT_B).action == AntennaAutomation::Action::None);
    band(a, BAND_40M); a.poll(0, 0, false, ANT_A);
    band(a, BAND_20M);
    TEST_ASSERT_EQUAL_INT(ANT_A, a.poll(0, 0, false, ANT_B).antenna);
}
void test_error_has_no_retry_and_manual_selection_unlocks_future_transitions() {
    AntennaAutomation a; a.configure(settings()); a.probeComplete(false);
    packet(a, BAND_UNKNOWN, 0);
    TEST_ASSERT_TRUE(a.poll(0, 0, false, ANT_UNKNOWN).action == AntennaAutomation::Action::None);
    a.manualAntennaComplete(false);
    TEST_ASSERT_TRUE(a.init() == AntennaInit::Error);
    a.manualAntennaComplete(true); band(a, BAND_40M); band(a, BAND_20M);
    TEST_ASSERT_TRUE(a.poll(0, 0, false, ANT_A).action == AntennaAutomation::Action::Select);
    a.selectionComplete(false); band(a, BAND_20M);
    TEST_ASSERT_TRUE(a.poll(0, 0, false, ANT_UNKNOWN).action == AntennaAutomation::Action::None);
}
struct Storage {
    std::map<std::string, std::vector<uint8_t>> data;
    bool fail = false;
    size_t putBytes(const char* key, const void* value, size_t size) {
        if (fail) return 0;
        const auto bytes = static_cast<const uint8_t*>(value);
        data[key] = std::vector<uint8_t>(bytes, bytes + size); return size;
    }
    size_t getBytesLength(const char* key) { return data[key].size(); }
    size_t getBytes(const char* key, void* value, size_t size) {
        if (data[key].size() != size) return 0;
        memcpy(value, data[key].data(), size); return size;
    }
};
void test_table_persistence_preserves_network_blob_and_rejects_corruption_and_failed_writes() {
    Storage store;
    DeviceConfig cfg{};
    strcpy(cfg.webUsername, "admin"); strcpy(cfg.webPassword, "saved-password");
    strcpy(cfg.wifiSSID, "existing-wifi"); cfg.meterPsuVoltage = 13.8; cfg.mqttPort = 1883;
    TEST_ASSERT_TRUE(writeConfig(store, cfg));
    const auto original = store.data["configV1"];
    TEST_ASSERT_TRUE(writeAntennaSettings(store, settings()));
    AntennaSettings loaded;
    TEST_ASSERT_TRUE(readAntennaSettings(store, loaded));
    TEST_ASSERT_TRUE(sameAntennaSettings(settings(), loaded));
    TEST_ASSERT_TRUE(original == store.data["configV1"]);
    store.fail = true;
    AntennaSettings disabled;
    TEST_ASSERT_FALSE(writeAntennaSettings(store, disabled));
    TEST_ASSERT_TRUE(readAntennaSettings(store, loaded));
    TEST_ASSERT_TRUE(loaded.enabled);
    store.data["antBandsV1"][0] = 2;
    TEST_ASSERT_FALSE(readAntennaSettings(store, loaded));
    DeviceConfig restored;
    TEST_ASSERT_TRUE(readConfig(store, restored));
    TEST_ASSERT_EQUAL_STRING(cfg.wifiSSID, restored.wifiSSID);
    TEST_ASSERT_EQUAL_STRING(cfg.webPassword, restored.webPassword);
}
void test_table_json_requires_complete_valid_table_and_defaults_to_no_switch() {
    AntennaSettings value;
    TEST_ASSERT_FALSE(value.enabled);
    JsonDocument doc;
    writeAntennaSettingsJson(doc, settings());
    TEST_ASSERT_TRUE(parseAntennaSettings(doc.as<JsonVariantConst>(), value));
    TEST_ASSERT_TRUE(sameAntennaSettings(settings(), value));
    for (const char* json : {"{}", "{\"enabled\":1,\"bands\":[]}", "{\"enabled\":true,\"bands\":[1]}"}) {
        deserializeJson(doc, json);
        TEST_ASSERT_FALSE(parseAntennaSettings(doc.as<JsonVariantConst>(), value));
    }
    doc.clear(); writeAntennaSettingsJson(doc, settings());
    doc["bands"][2] = -1;
    TEST_ASSERT_FALSE(parseAntennaSettings(doc.as<JsonVariantConst>(), value));
    doc["bands"][2] = 3;
    TEST_ASSERT_FALSE(parseAntennaSettings(doc.as<JsonVariantConst>(), value));
    TEST_ASSERT_TRUE(sameAntennaSettings(settings(), value));
}
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_startup_requires_actual_fresh_zero_and_only_runs_once);
    RUN_TEST(test_first_band_is_baseline_and_manual_choice_survives_rf_pauses);
    RUN_TEST(test_100_watts_or_a_high_sample_consumes_transition_without_delayed_retry);
    RUN_TEST(test_execution_rechecks_power_age_and_manual_priority);
    RUN_TEST(test_manual_command_during_confirmation_does_not_disable_debounce);
    RUN_TEST(test_unknown_invalid_and_stale_frames_break_confirmation_across_wrap);
    RUN_TEST(test_invalid_power_is_skipped_and_settings_changes_do_not_apply_to_current_band);
    RUN_TEST(test_error_has_no_retry_and_manual_selection_unlocks_future_transitions);
    RUN_TEST(test_table_persistence_preserves_network_blob_and_rejects_corruption_and_failed_writes);
    RUN_TEST(test_table_json_requires_complete_valid_table_and_defaults_to_no_switch);
    return UNITY_END();
}
