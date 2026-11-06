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
    a.poll(0, 0, false, ANT_A);
    return a;
}
void test_first_band_uses_table_with_normal_power_and_manual_guards() {
    for (auto b : {BAND_40M, BAND_20M}) for (auto selected : {ANT_A, ANT_B}) {
        AntennaAutomation a; a.configure(settings()); a.probeComplete(true);
        band(a, b);
        const auto request = a.poll(0, 0, false, selected);
        const auto target = b == BAND_40M ? ANT_A : ANT_B;
        TEST_ASSERT_TRUE(request.action == (selected == target ? AntennaAutomation::Action::None : AntennaAutomation::Action::Select));
        if (selected != target) TEST_ASSERT_EQUAL_INT(target, request.antenna);
        else TEST_ASSERT_TRUE(a.automatic() == AntennaAuto::AlreadySelected);
        TEST_ASSERT_TRUE(a.poll(0, 0, false, selected).action == AntennaAutomation::Action::None);
    }
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        AntennaAutomation a; auto s = settings();
        if (scenario == 1) s.bands[BAND_40M] = ANT_UNKNOWN;
        if (scenario == 2) s.enabled = false;
        a.configure(s); a.probeComplete(true);
        if (scenario == 3) { a.manualStarted(); a.manualAntennaComplete(true); }
        band(a, BAND_40M, scenario == 0 ? 100 : 40);
        TEST_ASSERT_TRUE(a.poll(0, 0, false, ANT_B).action == AntennaAutomation::Action::None);
        band(a, BAND_40M);
        TEST_ASSERT_TRUE(a.poll(0, 0, false, ANT_B).action == AntennaAutomation::Action::None);
    }
    // Selecting the tuner mode before RF is not a manual antenna choice.
    AntennaAutomation a; a.configure(settings()); a.manualStarted(); a.probeComplete(true);
    band(a, BAND_40M);
    TEST_ASSERT_TRUE(a.poll(0, 0, false, ANT_B).action == AntennaAutomation::Action::Select);
}
void test_first_band_waits_for_startup_check_without_losing_its_selection() {
    AntennaAutomation a; a.configure(settings());
    band(a, BAND_20M);
    TEST_ASSERT_TRUE(a.poll(0, 0, false, ANT_UNKNOWN).action == AntennaAutomation::Action::None);
    packet(a, BAND_UNKNOWN, 0, 1);
    TEST_ASSERT_TRUE(a.poll(1, 0, false, ANT_UNKNOWN).action == AntennaAutomation::Action::Probe);
    a.probeComplete(true);
    packet(a, BAND_UNKNOWN, 0, 2);
    const auto request = a.poll(2, 0, false, ANT_A);
    TEST_ASSERT_TRUE(request.action == AntennaAutomation::Action::Select);
    TEST_ASSERT_EQUAL_INT(ANT_B, request.antenna);
    AntennaAutomation blocked; blocked.configure(settings()); band(blocked, BAND_20M);
    packet(blocked, BAND_20M, 100, 1);
    blocked.probeComplete(true); packet(blocked, BAND_UNKNOWN, 0, 2);
    TEST_ASSERT_TRUE(blocked.poll(2, 0, false, ANT_A).action == AntennaAutomation::Action::None);
    TEST_ASSERT_TRUE(blocked.automatic() == AntennaAuto::HighPower);
}
void test_busy_selection_survives_missing_frames_and_has_bounded_recovery_across_wrap() {
    auto a = ready();
    const uint32_t started = 0xfffff000u;
    band(a, BAND_20M, 40, started);
    TEST_ASSERT_TRUE(a.poll(started, 0, false, ANT_A).action == AntennaAutomation::Action::Select);
    TEST_ASSERT_TRUE(a.selectionDeferred(BAND_20M, started));
    a.invalidateFrame();
    TEST_ASSERT_TRUE(a.poll(started + AntennaAutomation::TunerWaitMs, 0, false, ANT_A, true).action == AntennaAutomation::Action::None);
    TEST_ASSERT_TRUE(a.waitingTuner()); // A silent/unsuccessful old tune cannot consume the choice.
    packet(a, BAND_20M, 40, started + AntennaAutomation::TunerWaitMs);
    auto request = a.poll(started + AntennaAutomation::TunerWaitMs, 0, false, ANT_A, true);
    TEST_ASSERT_TRUE(request.action == AntennaAutomation::Action::Select);
    TEST_ASSERT_EQUAL_UINT(1, request.recovery);
    TEST_ASSERT_TRUE(a.selectionDeferred(BAND_20M, started + AntennaAutomation::TunerWaitMs));
    packet(a, BAND_20M, 40, started + 100000);
    TEST_ASSERT_TRUE(a.poll(started + 100000, 0, false, ANT_A, true).action == AntennaAutomation::Action::None);
    packet(a, BAND_UNKNOWN, 0, started + 100001);
    request = a.poll(started + 100001, 0, false, ANT_A, true);
    TEST_ASSERT_TRUE(request.action == AntennaAutomation::Action::Select);
    TEST_ASSERT_EQUAL_UINT(2, request.recovery);
    TEST_ASSERT_FALSE(a.selectionDeferred(BAND_20M, started + 100001));
    a.selectionComplete(false);
    TEST_ASSERT_TRUE(a.poll(started + 100001, 0, false, ANT_A).action == AntennaAutomation::Action::None);
}
void test_busy_selection_is_cancelled_by_manual_power_table_or_new_band() {
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        auto a = ready(); band(a, BAND_20M);
        a.poll(0, 0, false, ANT_A);
        TEST_ASSERT_TRUE(a.selectionDeferred(BAND_20M, 0));
        if (scenario == 1) a.manualStarted();
        if (scenario == 2) { packet(a, BAND_20M, 100, 1); packet(a, BAND_20M, 40, 2); }
        if (scenario == 3) { auto s = settings(); s.enabled = false; a.configure(s); }
        if (scenario == 4) band(a, BAND_40M, 40, 2);
        packet(a, BAND_UNKNOWN, 0, 3);
        const auto request = a.poll(3, scenario == 0 ? 1 : 0, scenario == 5, ANT_B, true);
        TEST_ASSERT_TRUE(request.action == (scenario == 4 ? AntennaAutomation::Action::Select : AntennaAutomation::Action::None));
        if (scenario == 4) { TEST_ASSERT_EQUAL_INT(ANT_A, request.antenna); TEST_ASSERT_EQUAL_UINT(0, request.recovery); }
        TEST_ASSERT_FALSE(a.waitingTuner());
    }
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
void test_manual_choice_survives_rf_pauses_after_first_band_selection() {
    auto a = ready();
    TEST_ASSERT_EQUAL_INT(BAND_40M, a.currentBand());
    TEST_ASSERT_TRUE(a.poll(0, 0, false, ANT_B).action == AntennaAutomation::Action::None);
    band(a, BAND_20M);
    const auto request = a.poll(0, 0, false, ANT_A);
    TEST_ASSERT_TRUE(request.action == AntennaAutomation::Action::Select);
    TEST_ASSERT_EQUAL_INT(ANT_B, request.antenna);
    a.selectionComplete(true);
    a.manualStarted(); a.manualAntennaComplete(true);
    TEST_ASSERT_TRUE(a.automatic() == AntennaAuto::ManualSelection);
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
    for (int scenario = 0; scenario < 3; ++scenario) {
        auto a = ready(); band(a, BAND_20M);
        if (scenario == 0) packet(a, BAND_20M, 100);
        TEST_ASSERT_TRUE(a.poll(0, scenario == 1 ? 1 : 0,
            scenario == 2, ANT_A).action == AntennaAutomation::Action::None);
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
void test_unknown_mode_waits_for_fresh_rf_off_and_manual_or_high_power_cancels_it() {
    auto a = ready(); band(a, BAND_20M);
    TEST_ASSERT_TRUE(a.poll(0, 0, false, ANT_A, true).action == AntennaAutomation::Action::None);
    TEST_ASSERT_TRUE(a.automatic() == AntennaAuto::WaitingRfOff);
    packet(a, BAND_20M, 0, 20);
    TEST_ASSERT_TRUE(a.poll(20, 0, false, ANT_A, true).action == AntennaAutomation::Action::Select);
    a.selectionComplete(true);
    TEST_ASSERT_TRUE(a.poll(20, 0, false, ANT_A, true).action == AntennaAutomation::Action::None);
    a = ready(); band(a, BAND_20M); a.poll(0, 0, false, ANT_A, true);
    a.manualStarted(); packet(a, BAND_20M, 0, 20);
    TEST_ASSERT_TRUE(a.poll(20, 1, false, ANT_A, true).action == AntennaAutomation::Action::None);
    a = ready(); band(a, BAND_20M); a.poll(0, 0, false, ANT_A, true);
    packet(a, BAND_20M, 100, 20);
    TEST_ASSERT_TRUE(a.poll(20, 0, false, ANT_A, true).action == AntennaAutomation::Action::None);
    TEST_ASSERT_TRUE(a.automatic() == AntennaAuto::HighPower);
    packet(a, BAND_20M, 0, 30);
    TEST_ASSERT_TRUE(a.poll(30, 0, false, ANT_A, true).action == AntennaAutomation::Action::None);
    a = ready(); band(a, BAND_20M); a.poll(0, 0, false, ANT_A, true);
    TEST_ASSERT_TRUE(a.poll(501, 0, false, ANT_A, true).action == AntennaAutomation::Action::RefreshMeter);
    TEST_ASSERT_TRUE(a.automatic() == AntennaAuto::WaitingMeter);
    packet(a, BAND_20M, 0, 502);
    TEST_ASSERT_TRUE(a.poll(502, 0, false, ANT_A, true).action == AntennaAutomation::Action::Select);
}
void test_stale_power_requests_one_refresh_and_waits_boundedly_without_undoing_manual_choice() {
    for (int scenario = 0; scenario < 5; ++scenario) {
        auto a = ready(); band(a, BAND_20M);
        TEST_ASSERT_TRUE(a.poll(501, 0, false, ANT_A).action == AntennaAutomation::Action::RefreshMeter);
        TEST_ASSERT_TRUE(a.automatic() == AntennaAuto::WaitingMeter);
        TEST_ASSERT_TRUE(a.poll(2000, 0, false, ANT_A).action == AntennaAutomation::Action::None);
        if (scenario == 0) {
            packet(a, BAND_20M, 40, 2001);
            TEST_ASSERT_TRUE(a.poll(2001, 0, false, ANT_A).action == AntennaAutomation::Action::Select);
        } else {
            if (scenario == 1) packet(a, BAND_20M, 100, 2001);
            if (scenario == 2) a.manualStarted();
            if (scenario == 3) a.meterRefreshFailed();
            TEST_ASSERT_TRUE(a.poll(2001, 0, false, ANT_A).action == AntennaAutomation::Action::None);
            TEST_ASSERT_TRUE(a.automatic() == (scenario == 1 ? AntennaAuto::HighPower :
                scenario == 2 ? AntennaAuto::Manual : AntennaAuto::InvalidPower));
            packet(a, BAND_20M, 40, 2002);
            TEST_ASSERT_TRUE(a.poll(2002, 0, false, ANT_A).action == AntennaAutomation::Action::None);
        }
    }
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
void test_controlled_data_wait_keeps_unstarted_selection_and_rechecks_cancellation_guards() {
    for (uint32_t at : {0u,0xfffff000u}) for (int scenario=0;scenario<6;++scenario) {
        auto a=ready(); band(a,BAND_20M,40,at);
        TEST_ASSERT_TRUE(a.poll(at+501,0,false,ANT_A,false,true).action==AntennaAutomation::Action::RefreshMeter);
        TEST_ASSERT_TRUE(a.poll(at+2500,0,false,ANT_A,false,true).action==AntennaAutomation::Action::None);
        TEST_ASSERT_TRUE(a.selectionPending());
        TEST_ASSERT_TRUE(a.automatic()==AntennaAuto::InvalidPower);
        if (scenario==1) a.manualStarted();
        if (scenario==2) packet(a,BAND_20M,100,at+2600);
        if (scenario==3) band(a,BAND_40M,40,at+2600);
        if (scenario==4) { auto s=settings(); s.enabled=0; a.configure(s); }
        if (scenario==5) a.meterRefreshFailed();
        packet(a,BAND_20M,40,at+2700);
        const auto request=a.poll(at+2700,0,false,ANT_A,false,true);
        TEST_ASSERT_TRUE(request.action==(scenario==0?AntennaAutomation::Action::Select:AntennaAutomation::Action::None));
        TEST_ASSERT_TRUE(a.poll(at+2701,0,false,ANT_A,false,true).action==AntennaAutomation::Action::None);
    }
}

void test_disabled_band_table_does_not_block_controlled_tuning_on_radio_band_state() {
    AntennaAutomation a; a.probeComplete(true);
    RadioState radio; radio.connection=RadioConnection::Ready;
    radio.rxKnown=true; radio.transmitting=true;
    // TCI can confirm TX before supplying frequency; a disabled antenna table
    // must not make that independent controlled-tuning path wait forever.
    a.observeRadio(radio,0);
    packet(a,BAND_UNKNOWN,40,100);
    TEST_ASSERT_TRUE(a.readyForTuning(100));
    TEST_ASSERT_FALSE(a.readyForTuning(601)); // Disabling the table never permits stale RF.
    a.probeComplete(false);
    packet(a,BAND_UNKNOWN,40,602);
    TEST_ASSERT_FALSE(a.readyForTuning(602));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_disabled_band_table_does_not_block_controlled_tuning_on_radio_band_state);
    RUN_TEST(test_controlled_data_wait_keeps_unstarted_selection_and_rechecks_cancellation_guards);
    RUN_TEST(test_startup_requires_actual_fresh_zero_and_only_runs_once);
    RUN_TEST(test_first_band_uses_table_with_normal_power_and_manual_guards);
    RUN_TEST(test_first_band_waits_for_startup_check_without_losing_its_selection);
    RUN_TEST(test_busy_selection_survives_missing_frames_and_has_bounded_recovery_across_wrap);
    RUN_TEST(test_busy_selection_is_cancelled_by_manual_power_table_or_new_band);
    RUN_TEST(test_manual_choice_survives_rf_pauses_after_first_band_selection);
    RUN_TEST(test_100_watts_or_a_high_sample_consumes_transition_without_delayed_retry);
    RUN_TEST(test_execution_rechecks_power_age_and_manual_priority);
    RUN_TEST(test_manual_command_during_confirmation_does_not_disable_debounce);
    RUN_TEST(test_unknown_invalid_and_stale_frames_break_confirmation_across_wrap);
    RUN_TEST(test_invalid_power_is_skipped_and_settings_changes_do_not_apply_to_current_band);
    RUN_TEST(test_error_has_no_retry_and_manual_selection_unlocks_future_transitions);
    RUN_TEST(test_unknown_mode_waits_for_fresh_rf_off_and_manual_or_high_power_cancels_it);
    RUN_TEST(test_stale_power_requests_one_refresh_and_waits_boundedly_without_undoing_manual_choice);
    RUN_TEST(test_table_persistence_preserves_network_blob_and_rejects_corruption_and_failed_writes);
    RUN_TEST(test_table_json_requires_complete_valid_table_and_defaults_to_no_switch);
    return UNITY_END();
}
