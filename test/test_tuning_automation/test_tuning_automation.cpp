#include <unity.h>
#include <map>
#include <string>
#include <vector>
#include "tuning_automation.h"
#include "mqtt_protocol.h"
#include "mqtt_state.h"
using namespace ldg;
using Action = TuningAutomation::Action;
void setUp() {}
void tearDown() {}
static TuningSettings managed(bool fallback = false) {
    TuningSettings s; s.managed = 1; s.autoFallback = fallback; return s;
}
static void sample(TuningAutomation& a, uint32_t at, double watts = 40, double swr = 1.5) {
    tuner_meter_t meter{}; meter.forward_power_raw = watts > 0 ? 100 : 0;
    meter.forward_power_watts = watts; meter.swr = swr; meter.band = BAND_40M;
    a.observe(meter, at);
}
static void start(TuningAutomation& a, uint32_t at = 0) {
    for (unsigned i = 0; i < 3; ++i) sample(a, at);
}
static Action poll(TuningAutomation& a, uint32_t at = 0, tuner_mode_t mode = MODE_MANUAL,
                   bool ready = true, bool manual = false, bool tune = true) {
    return a.poll(at, mode, ready, manual, tune);
}
struct Storage {
    std::map<std::string,std::vector<uint8_t>> blobs;
    bool fail = false;
    size_t putBytes(const char* key,const void* data,size_t n) {
        if (fail) return 0;
        blobs[key] = std::vector<uint8_t>((const uint8_t*)data,(const uint8_t*)data+n); return n;
    }
    size_t getBytesLength(const char* key) { return blobs[key].size(); }
    size_t getBytes(const char* key,void* data,size_t n) {
        if (getBytesLength(key)!=n) return 0;
        memcpy(data,blobs[key].data(),n); return n;
    }
};
void test_default_native_and_strict_persistent_settings() {
    TuningAutomation a; start(a); TEST_ASSERT_TRUE(poll(a,0,MODE_UNKNOWN)==Action::None);
    Storage storage; storage.blobs["configV1"]={42}; storage.blobs["antBandV1"]={24};
    TuningSettings value; TEST_ASSERT_FALSE(readTuningSettings(storage,value));
    TEST_ASSERT_EQUAL_UINT(20,value.swrTenths); TEST_ASSERT_EQUAL_UINT(0,value.managed);
    value=managed(true); value.swrTenths=25;
    TEST_ASSERT_TRUE(writeTuningSettings(storage,value));
    TuningSettings loaded; TEST_ASSERT_TRUE(readTuningSettings(storage,loaded));
    TEST_ASSERT_TRUE(sameTuningSettings(value,loaded));
    storage.fail=true; value.swrTenths=30; TEST_ASSERT_FALSE(writeTuningSettings(storage,value));
    TEST_ASSERT_TRUE(readTuningSettings(storage,loaded)); TEST_ASSERT_EQUAL_UINT(25,loaded.swrTenths);
    TEST_ASSERT_EQUAL_UINT(42,storage.blobs["configV1"][0]); TEST_ASSERT_EQUAL_UINT(24,storage.blobs["antBandV1"][0]);
    storage.blobs["tuneCtlV1"][0]=2; TEST_ASSERT_FALSE(readTuningSettings(storage,loaded));
    TEST_ASSERT_EQUAL_UINT(25,loaded.swrTenths);
    for (const char* json : {"{}", "[]", "{\"mode\":\"bad\",\"auto_fallback\":false,\"swr_threshold\":2}",
        "{\"auto_fallback\":1,\"swr_threshold\":2}",
        "{\"auto_fallback\":true,\"swr_threshold\":\"2\"}",
        "{\"auto_fallback\":true,\"swr_threshold\":1}",
        "{\"auto_fallback\":true,\"swr_threshold\":10.1}",
        "{\"auto_fallback\":true,\"swr_threshold\":2.05}"}) {
        JsonDocument doc; deserializeJson(doc,json);
        TEST_ASSERT_FALSE(parseTuningSettings(doc.as<JsonVariantConst>(),loaded));
        TEST_ASSERT_EQUAL_UINT(25,loaded.swrTenths);
    }
    JsonDocument doc; value=managed(true); writeTuningSettingsJson(doc,value);
    TEST_ASSERT_TRUE(doc["mode"].isNull());
    TEST_ASSERT_TRUE(parseTuningSettings(doc.as<JsonVariantConst>(),loaded));
    TEST_ASSERT_TRUE(sameTuningSettings(value,loaded));
}
void test_each_new_transmission_gets_exactly_one_memory_tune() {
    for (uint32_t at : {0u,0xfffffff0u}) {
        TuningAutomation a; a.configure(managed());
        sample(a,at,0); TEST_ASSERT_TRUE(poll(a,at,MODE_UNKNOWN)==Action::Manual);
        a.modeComplete(Action::Manual,true);
        for (int tx=0; tx<3; ++tx) {
            start(a,at); TEST_ASSERT_TRUE(poll(a,at)==Action::MemoryTune);
            a.memoryComplete(true,at);
            for (int i=0;i<10;++i) { sample(a,at); TEST_ASSERT_TRUE(poll(a,at)==Action::None); }
            // Short gaps and missing data do not create another transmission.
            sample(a,at,0); sample(a,at+1000,0); start(a,at+1001);
            TEST_ASSERT_TRUE(poll(a,at+1001)==Action::None);
            at+=1002;
            for (int i=0;i<=4;++i) sample(a,at+400*i,0);
            at+=1601;
            TEST_ASSERT_TRUE(poll(a,at)==Action::None);
        }
    }
}
void test_known_tci_tx_preserves_session_through_ssb_silence() {
    TuningAutomation a; a.configure(managed()); a.setPtt(true,true); start(a);
    TEST_ASSERT_TRUE(poll(a)==Action::MemoryTune); a.memoryComplete(true,0);
    for (int i=1;i<30;++i) { sample(a,200*i,0); TEST_ASSERT_TRUE(poll(a,200*i)==Action::None); }
    start(a,6000); TEST_ASSERT_TRUE(poll(a,6000)==Action::None);
    a.setPtt(true,false); sample(a,6001,0); TEST_ASSERT_TRUE(poll(a,6001)==Action::None);
    start(a,6002); TEST_ASSERT_TRUE(poll(a,6002)==Action::None); // RX RF noise is not a start.
    a.setPtt(true,true); start(a,6003); TEST_ASSERT_TRUE(poll(a,6003)==Action::MemoryTune);
}
void test_waits_for_manual_antenna_fresh_power_and_queue_priority() {
    TuningAutomation a; a.configure(managed());
    TEST_ASSERT_TRUE(poll(a,0,MODE_UNKNOWN)==Action::None);
    start(a); TEST_ASSERT_TRUE(poll(a,0,MODE_UNKNOWN,true,true)==Action::None);
    TEST_ASSERT_TRUE(poll(a,0,MODE_UNKNOWN)==Action::Manual); a.modeComplete(Action::Manual,true);
    TEST_ASSERT_TRUE(poll(a,0,MODE_MANUAL,false)==Action::None);
    TEST_ASSERT_TRUE(poll(a,501)==Action::None);
    sample(a,502); TEST_ASSERT_TRUE(poll(a,502,MODE_MANUAL,true,false,false)==Action::None);
    TEST_ASSERT_TRUE(poll(a,502)==Action::MemoryTune);
    a.memoryComplete(true,502); a.manualStarted(); a.antennaSelected();
    TEST_ASSERT_TRUE(poll(a,502)==Action::None);
    TEST_ASSERT_TRUE(a.state()==TuningState::ManualOverride);
}
void test_high_power_consumes_session_and_invalid_frames_cannot_authorize_tuning() {
    TuningAutomation a; a.configure(managed()); start(a); sample(a,1,100);
    TEST_ASSERT_TRUE(poll(a,1)==Action::None); sample(a,2,40);
    TEST_ASSERT_TRUE(poll(a,2)==Action::None); TEST_ASSERT_TRUE(a.state()==TuningState::HighPower);
    a.antennaSelected(); TEST_ASSERT_TRUE(poll(a,2)==Action::None);
    TuningAutomation prefix; prefix.configure(managed());
    sample(prefix,0,100); sample(prefix,1,40); sample(prefix,2,40);
    TEST_ASSERT_TRUE(poll(prefix,2)==Action::None);
    TEST_ASSERT_TRUE(prefix.state()==TuningState::HighPower);
    TuningAutomation b; b.configure(managed()); start(b); b.invalidateFrame();
    TEST_ASSERT_TRUE(poll(b)==Action::None); sample(b,1,NAN);
    TEST_ASSERT_TRUE(poll(b,1)==Action::None);
}
void test_auto_requires_enabled_fallback_and_three_post_memory_high_swr_samples() {
    for (bool fallback : {false,true}) {
        TuningAutomation a; a.configure(managed(fallback));
        start(a); TEST_ASSERT_TRUE(poll(a)==Action::MemoryTune); a.memoryComplete(true,0);
        TEST_ASSERT_TRUE(poll(a)==Action::None); // Pre-T SWR is never used.
        for (int i=1;i<=3;++i) { sample(a,i,40,2); TEST_ASSERT_TRUE(poll(a,i)==(i==3&&fallback?Action::Auto:Action::None)); }
        if (fallback) {
            a.modeComplete(Action::Auto,true); TEST_ASSERT_TRUE(a.state()==TuningState::AutoActive);
            sample(a,4,40,4); TEST_ASSERT_TRUE(poll(a,4,MODE_AUTO)==Action::None);
            for (int i=0;i<=4;++i) sample(a,100+400*i,0);
            TEST_ASSERT_TRUE(poll(a,1700,MODE_AUTO)==Action::Manual);
            a.modeComplete(Action::Manual,true); TEST_ASSERT_TRUE(a.state()==TuningState::WaitingRf);
            start(a,1701); TEST_ASSERT_TRUE(poll(a,1701)==Action::MemoryTune);
        }
    }
}
void test_swr_threshold_low_invalid_high_power_stale_and_failure_paths_are_bounded() {
    for (int scenario=0;scenario<5;++scenario) {
        TuningAutomation a; auto s=managed(true); s.swrTenths=30; a.configure(s); start(a);
        TEST_ASSERT_TRUE(poll(a)==Action::MemoryTune); a.memoryComplete(scenario!=4,0);
        for (int i=1;i<=3;++i) sample(a,i,scenario==2?100:40,scenario==0?2.5:scenario==1?NAN:4);
        if (scenario==4) { TEST_ASSERT_TRUE(poll(a,3)==Action::Auto); a.modeComplete(Action::Auto,false); }
        else TEST_ASSERT_TRUE(poll(a,scenario==3?1600:3)==Action::None);
        TEST_ASSERT_TRUE(poll(a,1700)==Action::None); // No retry / stale-SWR fallback.
    }
    TuningAutomation b; b.configure(managed(true)); start(b);
    TEST_ASSERT_TRUE(poll(b,0,MODE_UNKNOWN)==Action::Manual); b.modeComplete(Action::Manual,false);
    for (int i=0;i<10;++i) { sample(b,i); TEST_ASSERT_TRUE(poll(b,i,MODE_UNKNOWN)==Action::None); }
}
void test_managed_antenna_change_retunes_once_and_leaving_controlled_does_not_force_auto() {
    TuningAutomation a; a.configure(managed()); start(a); TEST_ASSERT_TRUE(poll(a)==Action::MemoryTune);
    a.memoryComplete(true,0); a.antennaSelected(); TEST_ASSERT_TRUE(poll(a)==Action::MemoryTune);
    a.memoryComplete(true,0); TEST_ASSERT_TRUE(poll(a)==Action::None);
    a.configure(TuningSettings{}); TEST_ASSERT_TRUE(poll(a)==Action::None);
    sample(a,1,0); TEST_ASSERT_TRUE(poll(a,1)==Action::None);
    TEST_ASSERT_TRUE(poll(a,1)==Action::None);
}
void test_tuning_state_roundtrip_and_state_only_mqtt_updates() {
    tuner_state_t state; state.tuningManaged=true; state.tuningState=TuningState::AutoActive;
    JsonDocument doc; writeState(doc,state,false); auto copy=readWebState(doc.as<JsonVariantConst>());
    TEST_ASSERT_TRUE(copy.tuningManaged); TEST_ASSERT_TRUE(copy.tuningState==TuningState::AutoActive);
    copy.tuningState=TuningState::ReturningManual; TEST_ASSERT_TRUE(significantChange(state,copy));
    doc["tuning_state"]="bad"; copy=readWebState(doc.as<JsonVariantConst>()); TEST_ASSERT_TRUE(copy.tuningState==TuningState::Native);
}
void test_saving_controlled_options_does_not_restart_memory_or_change_operating_mode() {
    TuningAutomation a; auto settings = managed(true); a.configure(settings); start(a);
    TEST_ASSERT_TRUE(poll(a)==Action::MemoryTune); a.memoryComplete(true,0);
    for (int i=1;i<=3;++i) sample(a,i,40,2.5);
    settings.swrTenths = 30; a.configure(settings);
    TEST_ASSERT_TRUE(poll(a,3)==Action::None); // Old threshold counters cannot enable AUTO.
    TEST_ASSERT_TRUE(a.enabled());
    start(a,4); TEST_ASSERT_TRUE(poll(a,4)==Action::None); // No extra MEM TUNE on save.
    JsonDocument options; deserializeJson(options,R"({"auto_fallback":false,"swr_threshold":2.0})");
    TEST_ASSERT_TRUE(parseTuningSettings(options.as<JsonVariantConst>(),settings));
    TEST_ASSERT_EQUAL_UINT(1,settings.managed);
    a.configure(settings); TEST_ASSERT_TRUE(a.enabled()); TEST_ASSERT_TRUE(poll(a,4)==Action::None);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_saving_controlled_options_does_not_restart_memory_or_change_operating_mode);
    RUN_TEST(test_default_native_and_strict_persistent_settings);
    RUN_TEST(test_each_new_transmission_gets_exactly_one_memory_tune);
    RUN_TEST(test_known_tci_tx_preserves_session_through_ssb_silence);
    RUN_TEST(test_waits_for_manual_antenna_fresh_power_and_queue_priority);
    RUN_TEST(test_high_power_consumes_session_and_invalid_frames_cannot_authorize_tuning);
    RUN_TEST(test_auto_requires_enabled_fallback_and_three_post_memory_high_swr_samples);
    RUN_TEST(test_swr_threshold_low_invalid_high_power_stale_and_failure_paths_are_bounded);
    RUN_TEST(test_managed_antenna_change_retunes_once_and_leaving_controlled_does_not_force_auto);
    RUN_TEST(test_tuning_state_roundtrip_and_state_only_mqtt_updates);
    return UNITY_END();
}
