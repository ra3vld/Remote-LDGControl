#include <unity.h>
#include <map>
#include <string>
#include <vector>
#include "tci_protocol.h"
#include "radio_settings.h"
#include "antenna_automation.h"
#include "mqtt_protocol.h"
#include "tci_expertsdr3_fixture.h"

void setUp() {}
void tearDown() {}
static void initialize(ldg::TciProtocol& protocol, uint32_t now = 100, uint8_t receiver = 0) {
    protocol.connect(receiver, now);
    TEST_ASSERT_TRUE(protocol.feed("PROTOCOL:ExpertSDR3,2.1;device:SunSDR2DX;trx_count:2;channels_count:2;ready;"
        "vfo:0,0,14074000;vfo:0,1,7100000;split_enable:0,false;xit_enable:0,false;xit_offset:0,0;"
        "vfo:1,0,21074000;vfo:1,1,28074000;split_enable:1,false;xit_enable:1,false;xit_offset:1,0;"
        "trx:0,false;trx:1,false;tune:0,false;tune:1,false;modulation:0,DIGU;", now));
}
static ldg::RadioState readyRadio(uint64_t hz = 14074000) {
    ldg::RadioState state;
    state.connection = ldg::RadioConnection::Ready;
    state.frequencyValid = state.rxKnown = state.tuneKnown = true;
    state.rxFrequencyHz = state.txFrequencyHz = hz;
    state.stableMs = 200;
    return state;
}
static tuner_meter_t zeroMeter() { tuner_meter_t value{}; return value; }
static ldg::AntennaSettings settings() {
    ldg::AntennaSettings value; value.enabled = true;
    value.bands[BAND_20M] = ANT_A; value.bands[BAND_40M] = ANT_B;
    return value;
}

void test_initial_state_is_complete_model_independent_and_tracks_all_tx_sources() {
    ldg::TciProtocol protocol;
    initialize(protocol);
    auto radio = protocol.snapshot(300);
    TEST_ASSERT_TRUE(radio.safeRx());
    TEST_ASSERT_EQUAL_UINT64(14074000, radio.txFrequencyHz);
    TEST_ASSERT_EQUAL_UINT(200, radio.stableMs);
    TEST_ASSERT_EQUAL_STRING("SunSDR2DX", radio.device);
    TEST_ASSERT_TRUE(protocol.feed("trx:1,true,micpc;", 310));
    TEST_ASSERT_FALSE(protocol.snapshot(310).safeRx());
    TEST_ASSERT_TRUE(protocol.feed("trx:1,false;tune:1,true;", 320));
    TEST_ASSERT_TRUE(protocol.snapshot(320).tuning);
    TEST_ASSERT_FALSE(protocol.snapshot(320).safeRx());
    TEST_ASSERT_TRUE(protocol.feed("tune:1,false;device:AnotherVendor;protocol:OtherServer,1.9;channel_count:2;", 330));
    TEST_ASSERT_TRUE(protocol.snapshot(330).safeRx());
    TEST_ASSERT_EQUAL_STRING("AnotherVendor", protocol.snapshot(330).device);
    initialize(protocol, 400, 1);
    TEST_ASSERT_EQUAL_UINT64(21074000, protocol.snapshot(600).txFrequencyHz);
}

void test_expertsdr3_bootstrap_ignores_last_tx_and_requires_ready_after_start() {
    ldg::TciProtocol protocol;
    protocol.connect(0, 0);
    for (const char* message : expertSdr3Bootstrap) {
        if (!strcmp(message, "ready;")) {
            TEST_ASSERT_FALSE(protocol.initialized());
            TEST_ASSERT_FALSE(protocol.snapshot(200).safeRx());
        }
        TEST_ASSERT_TRUE(protocol.feed(message, 250));
    }
    const auto state = protocol.snapshot(450);
    TEST_ASSERT_TRUE(state.safeRx());
    TEST_ASSERT_EQUAL_UINT64(7108060, state.txFrequencyHz);
    TEST_ASSERT_EQUAL_STRING("SunSDR2DX", state.device);
    TEST_ASSERT_EQUAL_STRING("ExpertSDR3,2.1", state.protocol);
    TEST_ASSERT_TRUE(protocol.feed("xit_offset:0,0.0;", 500));
    TEST_ASSERT_TRUE(protocol.snapshot(500).safeRx());
    TEST_ASSERT_TRUE(protocol.feed("xit_enable:0,true;xit_offset:0,-60.000;", 510));
    TEST_ASSERT_EQUAL_UINT64(7108000, protocol.snapshot(510).txFrequencyHz);
    TEST_ASSERT_TRUE(protocol.feed("xit_offset:0,+1000000000.0;", 520));
    TEST_ASSERT_EQUAL_INT32(1000000000, protocol.snapshot(520).xitOffset);
    for (const char* invalid : {"0.", ".0", "0.1", "1.00x", "1e2", "nan", "1000000001.0"}) {
        initialize(protocol);
        const std::string command = std::string("xit_offset:0,") + invalid + ";";
        TEST_ASSERT_FALSE(protocol.feed(command.c_str(), 600));
        TEST_ASSERT_FALSE(protocol.snapshot(600).safeRx());
    }
}

void test_radio_config_json_owns_the_snapshot_host_string() {
    ldg::RadioSettings value;
    strcpy(value.host, "expert-sdr.local"); value.enabled = true;
    JsonDocument doc;
    ldg::writeRadioSettingsJson(doc, value);
    strcpy(value.host, "overwritten");
    TEST_ASSERT_EQUAL_STRING("expert-sdr.local", doc["host"].as<const char*>());
}

void test_split_xit_fragmentation_and_obsolete_reported_tx_frequency() {
    ldg::TciProtocol protocol;
    initialize(protocol);
    TEST_ASSERT_TRUE(protocol.feed(" SPLIT_EN", 400));
    TEST_ASSERT_TRUE(protocol.feed("ABLE:0,true;unknown:a,b,c,d,e,f;XIT_ENABLE:0,true;XIT_OFF", 401));
    TEST_ASSERT_TRUE(protocol.feed("SET:0,-100;vfo:0,1,7000050;", 402));
    auto radio = protocol.snapshot(602);
    TEST_ASSERT_TRUE(radio.safeRx());
    TEST_ASSERT_EQUAL_UINT64(6999950, radio.txFrequencyHz);
    TEST_ASSERT_EQUAL_INT(BAND_UNKNOWN, ldg::frequencyBand(radio.txFrequencyHz));
    TEST_ASSERT_TRUE(protocol.feed("vfo:0,1,7100000;tx_frequency:7099950;", 700));
    TEST_ASSERT_EQUAL_UINT64(7099950, protocol.snapshot(700).txFrequencyHz);
    TEST_ASSERT_TRUE(protocol.feed("xit_offset:0,200;", 710));
    TEST_ASSERT_EQUAL_UINT64(7100200, protocol.snapshot(710).txFrequencyHz);
    TEST_ASSERT_TRUE(protocol.feed("tx_frequency:14074000;", 720));
    TEST_ASSERT_FALSE(protocol.snapshot(720).frequencyValid);
    TEST_ASSERT_TRUE(protocol.feed("split_enable:0,false;", 730));
    TEST_ASSERT_EQUAL_UINT64(14074200, protocol.snapshot(730).txFrequencyHz);
}

void test_incomplete_malformed_oversized_and_reconnected_state_never_authorize_switches() {
    ldg::TciProtocol protocol;
    protocol.connect(0, 0);
    TEST_ASSERT_TRUE(protocol.feed("trx_count:2;ready;vfo:0,0,14074000;split_enable:0,false;xit_enable:0,false;trx:0,false;tune:0,false;", 10));
    TEST_ASSERT_FALSE(protocol.snapshot(300).safeRx());
    TEST_ASSERT_TRUE(protocol.feed("trx:1,false;tune:1,false;", 400));
    TEST_ASSERT_TRUE(protocol.snapshot(400).safeRx());
    TEST_ASSERT_FALSE(protocol.feed("trx:0,perhaps;", 410));
    TEST_ASSERT_FALSE(protocol.snapshot(410).safeRx());
    initialize(protocol, 500);
    TEST_ASSERT_FALSE(protocol.feed("vfo:0,0,18446744073709551616;", 510));
    initialize(protocol, 600);
    std::string huge(1025, 'x'); huge += ';';
    TEST_ASSERT_FALSE(protocol.feed(huge.c_str(), 610));
    initialize(protocol, 700);
    TEST_ASSERT_FALSE(protocol.feed("vfo:0,,7100000;", 710));
    protocol.disconnect(true, "lost");
    TEST_ASSERT_FALSE(protocol.snapshot(720).frequencyValid);
    protocol.connect(0, 730);
    TEST_ASSERT_TRUE(protocol.feed("ready;", 740));
    TEST_ASSERT_FALSE(protocol.snapshot(740).safeRx());
}

void test_heartbeat_and_stable_band_are_distinct_and_survive_clock_wrap() {
    ldg::TciProtocol protocol;
    const uint32_t start = 0xffffff00u;
    initialize(protocol, start);
    TEST_ASSERT_TRUE(protocol.snapshot(start + 3000).safeRx());
    TEST_ASSERT_FALSE(protocol.snapshot(start + 3001).safeRx());
    protocol.heartbeat(start + 4000);
    TEST_ASSERT_TRUE(protocol.snapshot(start + 4000).safeRx());
    TEST_ASSERT_EQUAL_UINT(4000, protocol.snapshot(start + 4000).stableMs);
    TEST_ASSERT_TRUE(protocol.feed("vfo:0,0,14075000;", start + 4010));
    TEST_ASSERT_EQUAL_UINT(4010, protocol.snapshot(start + 4010).stableMs);
    TEST_ASSERT_TRUE(protocol.feed("vfo:0,0,7100000;", start + 4020));
    TEST_ASSERT_EQUAL_UINT(0, protocol.snapshot(start + 4020).stableMs);
    TEST_ASSERT_TRUE(protocol.feed("stop;", start + 4030));
    TEST_ASSERT_FALSE(protocol.snapshot(start + 4030).safeRx());
}

void test_antenna_intent_selects_first_band_without_rf_and_waits_for_rx_and_actual_zero() {
    ldg::AntennaAutomation automation;
    automation.configure(settings()); automation.probeComplete(true);
    auto radio = readyRadio(); radio.stableMs = 199;
    automation.observeRadio(radio, 0); automation.observe(zeroMeter(), 100, 0);
    TEST_ASSERT_EQUAL_INT(int(ldg::AntennaAutomation::Action::None), int(automation.poll(100,0,false,ANT_B).action));
    radio.stableMs = 200; radio.transmitting = true;
    automation.observeRadio(radio,0);
    TEST_ASSERT_EQUAL_INT(int(ldg::AntennaAutomation::Action::None), int(automation.poll(100,0,false,ANT_B).action));
    TEST_ASSERT_EQUAL_INT(int(ldg::AntennaAuto::WaitingRx), int(automation.automatic()));
    radio.transmitting = false; automation.observeRadio(radio,0);
    auto power = zeroMeter(); power.forward_power_raw = 1; power.forward_power_watts = 0.01;
    automation.observe(power,101,0);
    TEST_ASSERT_EQUAL_INT(int(ldg::AntennaAutomation::Action::None), int(automation.poll(101,0,false,ANT_B).action));
    automation.observe(zeroMeter(),102,0);
    const auto request = automation.poll(102,0,false,ANT_B);
    TEST_ASSERT_EQUAL_INT(int(ldg::AntennaAutomation::Action::Select),int(request.action));
    TEST_ASSERT_EQUAL_INT(ANT_A,request.antenna);
    automation.selectionComplete(true);
    radio.txFrequencyHz = 14076000; automation.observeRadio(radio,0);
    TEST_ASSERT_EQUAL_INT(int(ldg::AntennaAutomation::Action::None),int(automation.poll(103,0,false,ANT_A).action));
}

void test_radio_manual_lock_survives_reconnection_until_a_different_band() {
    ldg::AntennaAutomation automation;
    automation.configure(settings()); automation.probeComplete(true);
    auto radio = readyRadio(); automation.observeRadio(radio,0); automation.observe(zeroMeter(),100,0);
    automation.manualStarted(); automation.manualAntennaComplete(true);
    automation.observeRadio(ldg::RadioState{},1);
    automation.observeRadio(radio,1);
    TEST_ASSERT_EQUAL_INT(int(ldg::AntennaAutomation::Action::None),int(automation.poll(101,1,false,ANT_B).action));
    radio.txFrequencyHz = 7100000; automation.observeRadio(radio,1);
    TEST_ASSERT_EQUAL_INT(int(ldg::AntennaAutomation::Action::Select),int(automation.poll(102,1,false,ANT_A).action));
}

void test_radio_loss_cancels_intent_and_restores_rf_debounce_without_held_values() {
    ldg::AntennaAutomation automation;
    automation.configure(settings()); automation.probeComplete(true);
    auto radio = readyRadio(); automation.observeRadio(radio,0); automation.observe(zeroMeter(),100,0);
    automation.observeRadio(ldg::RadioState{},0);
    TEST_ASSERT_FALSE(automation.usesRadio());
    TEST_ASSERT_EQUAL_INT(int(ldg::AntennaAutomation::Action::None),int(automation.poll(100,0,false,ANT_B).action));
    auto rf = zeroMeter(); rf.band = BAND_40M; rf.forward_power_raw = 100; rf.forward_power_watts = 10;
    automation.observe(rf,110,0); automation.observe(rf,120,0);
    TEST_ASSERT_EQUAL_INT(int(ldg::AntennaAutomation::Action::None),int(automation.poll(120,0,false,ANT_A).action));
    automation.observe(rf,130,0);
    TEST_ASSERT_EQUAL_INT(int(ldg::AntennaAutomation::Action::Select),int(automation.poll(130,0,false,ANT_A).action));
}

void test_radio_intent_survives_meter_timeout_but_never_invents_zero() {
    ldg::AntennaAutomation automation;
    automation.configure(settings()); automation.probeComplete(true);
    automation.observeRadio(readyRadio(),0);
    TEST_ASSERT_EQUAL_INT(int(ldg::AntennaAutomation::Action::RefreshMeter),int(automation.poll(100,0,false,ANT_B).action));
    TEST_ASSERT_EQUAL_INT(int(ldg::AntennaAutomation::Action::None),int(automation.poll(1700,0,false,ANT_B).action));
    automation.meterRefreshFailed();
    automation.observe(zeroMeter(),1800,0);
    TEST_ASSERT_EQUAL_INT(int(ldg::AntennaAutomation::Action::Select),int(automation.poll(1800,0,false,ANT_B).action));
}

struct Storage {
    std::map<std::string,std::vector<uint8_t>> records;
    bool fail = false;
    size_t putBytes(const char* key, const void* bytes, size_t size) {
        if (fail) return 0;
        const auto* p = static_cast<const uint8_t*>(bytes); records[key] = {p,p+size}; return size;
    }
    size_t getBytesLength(const char* key) { return records[key].size(); }
    size_t getBytes(const char* key, void* out, size_t size) {
        if (records[key].size() != size) return 0;
        memcpy(out,records[key].data(),size); return size;
    }
};
void test_settings_are_separate_atomic_validated_and_backward_compatible() {
    Storage store; store.records["configV1"] = {1,2,3};
    ldg::RadioSettings value, loaded;
    TEST_ASSERT_FALSE(ldg::readRadioSettings(store,loaded));
    TEST_ASSERT_FALSE(loaded.enabled); TEST_ASSERT_EQUAL_UINT(40001,loaded.port);
    strcpy(value.host,"expert-sdr.local"); value.enabled = true;
    TEST_ASSERT_TRUE(ldg::writeRadioSettings(store,value));
    store.fail = true; value.port = 50001;
    TEST_ASSERT_FALSE(ldg::writeRadioSettings(store,value));
    TEST_ASSERT_TRUE(ldg::readRadioSettings(store,loaded));
    TEST_ASSERT_EQUAL_UINT(40001,loaded.port);
    TEST_ASSERT_EQUAL_UINT(3,store.records["configV1"].size());
    store.records["radioV1"][0] = 2;
    TEST_ASSERT_FALSE(ldg::readRadioSettings(store,loaded));
    for (const char* json : {"{}", "{\"enabled\":true,\"host\":\"\",\"port\":40001,\"receiver\":0}",
        "{\"enabled\":true,\"host\":\"ws://host\",\"port\":40001,\"receiver\":0}",
        "{\"enabled\":true,\"host\":\"host\",\"port\":65536,\"receiver\":0}",
        "{\"enabled\":true,\"host\":\"host\",\"port\":40001,\"receiver\":16}"}) {
        JsonDocument doc; deserializeJson(doc,json);
        TEST_ASSERT_FALSE(ldg::parseRadioSettings(doc.as<JsonVariantConst>(),loaded));
    }
}

void test_web_and_remote_radio_snapshot_round_trip_and_payload_bound() {
    tuner_state_t state; state.radio = readyRadio(); state.antennaFromRadio = true;
    state.antennaAuto = ldg::AntennaAuto::WaitingRx;
    strcpy(state.radio.device,"\"SunSDR2DX\""); strcpy(state.radio.protocol,"ExpertSDR3,2.1");
    memset(state.radio.error,'x',sizeof(state.radio.error)-1);
    state.radio.freeHeap = state.radio.largestBlock = state.radio.stackFree = UINT32_MAX;
    state.radio.ageMs = state.radio.stableMs = UINT32_MAX;
    JsonDocument doc; ldg::writeState(doc,state,false);
    TEST_ASSERT_LESS_THAN_UINT(1536,measureJson(doc) + 120); // shared meter scale fields also fit.
    const auto remote = ldg::readWebState(doc.as<JsonVariantConst>());
    TEST_ASSERT_TRUE(remote.antennaFromRadio);
    TEST_ASSERT_EQUAL_UINT64(state.radio.txFrequencyHz,remote.radio.txFrequencyHz);
    TEST_ASSERT_EQUAL_STRING(state.radio.device,remote.radio.device);
    TEST_ASSERT_EQUAL_INT(int(state.antennaAuto),int(remote.antennaAuto));
    ldg::ActiveTunerState active; active.updateRemote(state);
    TEST_ASSERT_FALSE(active.select(tuner_state_t{},3001,true).radio.frequencyValid);
}

void test_frequency_table_inclusive_edges_and_unsupported_bands() {
    TEST_ASSERT_EQUAL_INT(BAND_160M,ldg::frequencyBand(1800000));
    TEST_ASSERT_EQUAL_INT(BAND_160M,ldg::frequencyBand(2000000));
    TEST_ASSERT_EQUAL_INT(BAND_UNKNOWN,ldg::frequencyBand(2000001));
    TEST_ASSERT_EQUAL_INT(BAND_60M,ldg::frequencyBand(5357000));
    TEST_ASSERT_EQUAL_INT(BAND_6M,ldg::frequencyBand(54000000));
    TEST_ASSERT_EQUAL_INT(BAND_UNKNOWN,ldg::frequencyBand(144300000));
    TEST_ASSERT_EQUAL_INT(BAND_UNKNOWN,ldg::frequencyBand(0));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_expertsdr3_bootstrap_ignores_last_tx_and_requires_ready_after_start);
    RUN_TEST(test_radio_config_json_owns_the_snapshot_host_string);
    RUN_TEST(test_initial_state_is_complete_model_independent_and_tracks_all_tx_sources);
    RUN_TEST(test_split_xit_fragmentation_and_obsolete_reported_tx_frequency);
    RUN_TEST(test_incomplete_malformed_oversized_and_reconnected_state_never_authorize_switches);
    RUN_TEST(test_heartbeat_and_stable_band_are_distinct_and_survive_clock_wrap);
    RUN_TEST(test_antenna_intent_selects_first_band_without_rf_and_waits_for_rx_and_actual_zero);
    RUN_TEST(test_radio_manual_lock_survives_reconnection_until_a_different_band);
    RUN_TEST(test_radio_loss_cancels_intent_and_restores_rf_debounce_without_held_values);
    RUN_TEST(test_radio_intent_survives_meter_timeout_but_never_invents_zero);
    RUN_TEST(test_settings_are_separate_atomic_validated_and_backward_compatible);
    RUN_TEST(test_web_and_remote_radio_snapshot_round_trip_and_payload_bound);
    RUN_TEST(test_frequency_table_inclusive_edges_and_unsupported_bands);
    return UNITY_END();
}
