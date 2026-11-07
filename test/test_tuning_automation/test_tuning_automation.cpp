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
static void sample(TuningAutomation& a, uint32_t at, double watts = 40, double swr = 3.5) {
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
void test_each_mismatched_transmission_gets_exactly_one_memory_tune() {
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
    sample(a,502); sample(a,503); sample(a,504);
    TEST_ASSERT_TRUE(poll(a,504,MODE_MANUAL,true,false,false)==Action::None);
    TEST_ASSERT_TRUE(poll(a,504)==Action::MemoryTune);
    a.memoryComplete(true,504); a.manualStarted(); a.antennaSelected();
    TEST_ASSERT_TRUE(poll(a,504)==Action::None);
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
    for (int i=0;i<3;++i) sample(a,4,40,1.5);
    TEST_ASSERT_TRUE(poll(a,4)==Action::None); // No extra MEM TUNE on save.
    JsonDocument options; deserializeJson(options,R"({"auto_fallback":false,"swr_threshold":2.0})");
    TEST_ASSERT_TRUE(parseTuningSettings(options.as<JsonVariantConst>(),settings));
    TEST_ASSERT_EQUAL_UINT(1,settings.managed);
    a.configure(settings); TEST_ASSERT_TRUE(a.enabled()); TEST_ASSERT_TRUE(poll(a,4)==Action::None);
}

void test_swr_supervision_continues_after_match_and_frequency_drift_for_entire_tx() {
    for (uint32_t at : {0u,0xfffff000u}) {
        TuningAutomation a; a.configure(managed(true)); start(a,at);
        TEST_ASSERT_TRUE(poll(a,at)==Action::MemoryTune); a.memoryComplete(true,at);
        // Good match persists long past the old 1.5-second post-memory window.
        for (unsigned i=1;i<=300;++i) {
            sample(a,at+i*100,40,1.3);
            TEST_ASSERT_TRUE(poll(a,at+i*100)==Action::None);
        }
        TEST_ASSERT_TRUE(a.state()==TuningState::Matched);
        for (unsigned i=1;i<=3;++i) {
            sample(a,at+30000+i*100,40,2.5);
            TEST_ASSERT_TRUE(poll(a,at+30000+i*100)==(i==3?Action::MemoryTune:Action::None));
        }
        a.memoryComplete(true,at+30300);
        for (unsigned i=1;i<=3;++i) {
            sample(a,at+30300+i*100,40,2.5);
            TEST_ASSERT_TRUE(poll(a,at+30300+i*100)==(i==3?Action::Auto:Action::None));
        }
        a.modeComplete(Action::Auto,true);
        for (unsigned i=4;i<20;++i) {
            sample(a,at+30300+i*100,40,3);
            TEST_ASSERT_TRUE(poll(a,at+30300+i*100,MODE_AUTO)==Action::None);
        }
    }
}
void test_missing_or_invalid_swr_pauses_monitoring_without_disarming_it() {
    for (int scenario=0;scenario<3;++scenario) {
        TuningAutomation a; a.configure(managed(true)); start(a);
        TEST_ASSERT_TRUE(poll(a)==Action::MemoryTune); a.memoryComplete(true,0);
        for (int i=1;i<=3;++i) sample(a,i*30,40,1.2);
        TEST_ASSERT_TRUE(poll(a,100)==Action::None);
        if (scenario==1) { a.invalidateFrame(); sample(a,200,40,NAN); }
        if (scenario==2) { a.setPtt(true,true); for (int i=2;i<=20;++i) sample(a,i*100,0); }
        TEST_ASSERT_TRUE(poll(a,2000)==Action::None);
        TEST_ASSERT_TRUE(a.state()==TuningState::NoFreshData);
        for (int i=1;i<=3;++i) {
            sample(a,2000+i*100,40,2.2);
            TEST_ASSERT_TRUE(poll(a,2000+i*100)==(i==3?Action::MemoryTune:Action::None));
        }
    }
}
void test_enabling_auto_after_memory_arms_only_fresh_swr_without_repeating_memory() {
    TuningAutomation a; auto s=managed(false); a.configure(s); start(a);
    TEST_ASSERT_TRUE(poll(a)==Action::MemoryTune); a.memoryComplete(true,0);
    for (int i=1;i<=30;++i) { sample(a,i*100,40,3); TEST_ASSERT_TRUE(poll(a,i*100)==Action::None); }
    s.autoFallback=true; a.configure(s);
    TEST_ASSERT_TRUE(poll(a,3000)==Action::None); // Stored SWR must not trigger C.
    for (int i=1;i<=3;++i) {
        sample(a,3000+i*100,40,3);
        TEST_ASSERT_TRUE(poll(a,3000+i*100)==(i==3?Action::Auto:Action::None));
    }
    a.modeComplete(Action::Auto,false);
    s.autoFallback=false; a.configure(s); s.autoFallback=true; a.configure(s);
    for (int i=1;i<=10;++i) { sample(a,4000+i*100,40,3); TEST_ASSERT_TRUE(poll(a,4000+i*100)==Action::None); }
}
void test_late_high_swr_still_respects_power_antenna_manual_and_mode_guards() {
    for (int scenario=0;scenario<6;++scenario) {
        TuningAutomation a; a.configure(managed(true)); start(a);
        TEST_ASSERT_TRUE(poll(a)==Action::MemoryTune); a.memoryComplete(true,0);
        for (int i=1;i<=3;++i) sample(a,i*100,40,1.2);
        TEST_ASSERT_TRUE(poll(a,300)==Action::None);
        if (scenario==0) a.manualStarted();
        if (scenario==1) sample(a,400,100,3);
        for (int i=1;i<=3;++i) sample(a,3000+i*100,40,3);
        TEST_ASSERT_TRUE(poll(a,3900)==Action::None); // Stale RF cannot authorize the new cycle.
        for (int i=1;i<=3;++i) sample(a,3900+i*100,40,3);
        const auto result=poll(a,4200,scenario==2?MODE_UNKNOWN:MODE_MANUAL,scenario!=3,scenario==4,scenario!=5);
        TEST_ASSERT_TRUE(result==(scenario==2?Action::Manual:Action::None));
    }
}

void test_new_swr_rises_repeat_complete_cycles_in_one_tx_and_constant_mismatch_does_not_loop() {
    for (bool fallback:{false,true}) {
        TuningAutomation a; a.configure(managed(fallback)); start(a);
        TEST_ASSERT_TRUE(poll(a)==Action::MemoryTune); a.memoryComplete(true,0);
        uint32_t now=0; tuner_mode_t mode=MODE_MANUAL;
        for (int cycle=0;cycle<4;++cycle) {
            // Three good RF measurements arm exactly one new rise.
            for (int i=0;i<3;++i) { now+=100; sample(a,now,40,1.2); TEST_ASSERT_TRUE(poll(a,now,mode)==Action::None); }
            for (int i=0;i<2;++i) { now+=100; sample(a,now,40,3); TEST_ASSERT_TRUE(poll(a,now,mode)==Action::None); }
            now+=100; sample(a,now,40,3);
            TEST_ASSERT_TRUE(poll(a,now,mode)==(mode==MODE_AUTO?Action::Manual:Action::MemoryTune));
            if (mode==MODE_AUTO) {
                a.modeComplete(Action::Manual,true); mode=MODE_MANUAL;
                TEST_ASSERT_TRUE(poll(a,now,mode)==Action::MemoryTune);
            }
            a.memoryComplete(true,now);
            for (int i=1;i<=3;++i) {
                now+=100; sample(a,now,40,3);
                TEST_ASSERT_TRUE(poll(a,now,mode)==(i==3&&fallback?Action::Auto:Action::None));
            }
            if (fallback) { a.modeComplete(Action::Auto,true); mode=MODE_AUTO; }
            for (int i=0;i<50;++i) { now+=100; sample(a,now,40,3); TEST_ASSERT_TRUE(poll(a,now,mode)==Action::None); }
        }
    }
}
void test_idle_zero_batches_end_rf_session_but_missing_packets_never_do() {
    for (uint32_t at:{0u,0xfffff000u}) {
        TuningAutomation a; a.configure(managed()); start(a,at);
        TEST_ASSERT_TRUE(poll(a,at)==Action::MemoryTune); a.memoryComplete(true,at);
        sample(a,at+100,0); TEST_ASSERT_TRUE(poll(a,at+100)==Action::None);
        sample(a,at+1350,0); TEST_ASSERT_TRUE(poll(a,at+1350)==Action::None);
        sample(a,at+2600,0); TEST_ASSERT_TRUE(poll(a,at+2600)==Action::None);
        start(a,at+2700); TEST_ASSERT_TRUE(poll(a,at+2700)==Action::MemoryTune);
        a.memoryComplete(true,at+2700);
        sample(a,at+2800,0);
        TEST_ASSERT_TRUE(poll(a,at+20000)==Action::None); // Time alone is not evidence of RF off.
        start(a,at+20100); TEST_ASSERT_TRUE(poll(a,at+20100)==Action::None);
        sample(a,at+20200,0); sample(a,at+30200,0); // Disconnected interval resets zero evidence.
        start(a,at+30300); TEST_ASSERT_TRUE(poll(a,at+30300)==Action::None);
    }
}
void test_failed_repeat_cycle_requires_another_match_and_rise_before_retrying() {
    for (Action failure : {Action::Manual,Action::MemoryTune,Action::Auto}) {
        TuningAutomation a; a.configure(managed(true)); start(a);
        TEST_ASSERT_TRUE(poll(a)==Action::MemoryTune); a.memoryComplete(true,0);
        uint32_t now=0;
        for (int i=0;i<3;++i) { now+=100; sample(a,now,40,3); }
        TEST_ASSERT_TRUE(poll(a,now)==Action::Auto); a.modeComplete(Action::Auto,true);
        for (int i=0;i<3;++i) { now+=100; sample(a,now,40,1.2); }
        for (int i=0;i<3;++i) { now+=100; sample(a,now,40,3); }
        TEST_ASSERT_TRUE(poll(a,now,MODE_AUTO)==Action::Manual);
        a.modeComplete(Action::Manual,failure!=Action::Manual);
        if (failure!=Action::Manual) {
            TEST_ASSERT_TRUE(poll(a,now)==Action::MemoryTune);
            a.memoryComplete(failure!=Action::MemoryTune,now);
            for (int i=0;i<3;++i) { now+=100; sample(a,now,40,3); }
            TEST_ASSERT_TRUE(poll(a,now)==Action::Auto);
            a.modeComplete(Action::Auto,failure!=Action::Auto);
        }
        const auto mode=failure==Action::Manual||failure==Action::Auto?MODE_UNKNOWN:MODE_AUTO;
        for (int i=0;i<40;++i) {
            now+=100; sample(a,now,40,3); TEST_ASSERT_TRUE(poll(a,now,mode)==Action::None);
        }
        for (int i=0;i<3;++i) { now+=100; sample(a,now,40,1.2); }
        for (int i=0;i<3;++i) { now+=100; sample(a,now,40,3); }
        TEST_ASSERT_TRUE(poll(a,now,mode)==Action::Manual);
        a.modeComplete(Action::Manual,true);
        TEST_ASSERT_TRUE(poll(a,now)==Action::MemoryTune);
    }
}

void test_rf_start_waits_for_one_known_band_but_live_ptt_can_confirm_unknown_ldg_band() {
    for (bool ptt : {false,true}) {
        TuningAutomation a; a.configure(managed()); a.setPtt(ptt,true);
        tuner_meter_t meter{}; meter.forward_power_raw=100; meter.forward_power_watts=40; meter.swr=2.5;
        for (unsigned i=1;i<=3;++i) {
            a.observe(meter,i*100);
            const auto action=poll(a,i*100);
            TEST_ASSERT_TRUE(action==(ptt&&i==3?Action::MemoryTune:Action::None));
        }
        if (ptt) continue;
        meter.band=BAND_20M; a.observe(meter,400); TEST_ASSERT_TRUE(poll(a,400)==Action::None);
        meter.band=BAND_160M;
        for (unsigned i=1;i<=3;++i) {
            a.observe(meter,400+i*100);
            TEST_ASSERT_TRUE(poll(a,400+i*100)==(i==3?Action::MemoryTune:Action::None));
        }
    }
}

void test_matched_transmissions_skip_memory_but_new_mismatch_still_tunes() {
    for (bool tci : {false, true}) for (uint32_t base : {0u, 0xfffff000u}) {
        TuningAutomation a; a.configure(managed(true));
        uint32_t at = base;
        for (int tx = 0; tx < 4; ++tx) {
            a.setPtt(tci, true);
            for (int i = 0; i < 10; ++i) {
                at += 100; sample(a, at, 40, 1.0);
                TEST_ASSERT_TRUE(poll(a, at) == Action::None);
            }
            TEST_ASSERT_TRUE(a.state() == TuningState::Matched);
            a.setPtt(tci, false);
            for (int i = 0; i < 5; ++i) { at += 500; sample(a, at, 0); }
            TEST_ASSERT_TRUE(poll(a, at) == Action::None);
        }
        a.setPtt(tci, true);
        for (int i = 1; i <= 3; ++i) {
            at += 100; sample(a, at, 40, 2.5);
            TEST_ASSERT_TRUE(poll(a, at) == (i == 3 ? Action::MemoryTune : Action::None));
        }
        a.memoryComplete(true, at);
        for (int i = 1; i <= 3; ++i) {
            at += 100; sample(a, at, 40, 2.5);
            TEST_ASSERT_TRUE(poll(a, at) == (i == 3 ? Action::Auto : Action::None));
        }
    }
}
void test_matched_start_still_supervises_late_swr_rises_and_invalid_start_waits() {
    TuningAutomation a; a.configure(managed());
    for (int i = 1; i <= 3; ++i) sample(a, i * 100, 40, NAN);
    TEST_ASSERT_TRUE(poll(a, 300) == Action::None);
    for (int i = 4; i <= 6; ++i) sample(a, i * 100, 40, 1.0);
    TEST_ASSERT_TRUE(poll(a, 600) == Action::None);
    TEST_ASSERT_TRUE(a.state() == TuningState::Matched);
    for (int i = 7; i <= 9; ++i) {
        sample(a, i * 100, 40, 2.5);
        TEST_ASSERT_TRUE(poll(a, i * 100) == (i == 9 ? Action::MemoryTune : Action::None));
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_matched_transmissions_skip_memory_but_new_mismatch_still_tunes);
    RUN_TEST(test_matched_start_still_supervises_late_swr_rises_and_invalid_start_waits);
    RUN_TEST(test_rf_start_waits_for_one_known_band_but_live_ptt_can_confirm_unknown_ldg_band);
    RUN_TEST(test_failed_repeat_cycle_requires_another_match_and_rise_before_retrying);
    RUN_TEST(test_new_swr_rises_repeat_complete_cycles_in_one_tx_and_constant_mismatch_does_not_loop);
    RUN_TEST(test_idle_zero_batches_end_rf_session_but_missing_packets_never_do);
    RUN_TEST(test_swr_supervision_continues_after_match_and_frequency_drift_for_entire_tx);
    RUN_TEST(test_missing_or_invalid_swr_pauses_monitoring_without_disarming_it);
    RUN_TEST(test_enabling_auto_after_memory_arms_only_fresh_swr_without_repeating_memory);
    RUN_TEST(test_late_high_swr_still_respects_power_antenna_manual_and_mode_guards);

    RUN_TEST(test_saving_controlled_options_does_not_restart_memory_or_change_operating_mode);
    RUN_TEST(test_default_native_and_strict_persistent_settings);
    RUN_TEST(test_each_mismatched_transmission_gets_exactly_one_memory_tune);
    RUN_TEST(test_known_tci_tx_preserves_session_through_ssb_silence);
    RUN_TEST(test_waits_for_manual_antenna_fresh_power_and_queue_priority);
    RUN_TEST(test_high_power_consumes_session_and_invalid_frames_cannot_authorize_tuning);
    RUN_TEST(test_auto_requires_enabled_fallback_and_three_post_memory_high_swr_samples);
    RUN_TEST(test_swr_threshold_low_invalid_high_power_stale_and_failure_paths_are_bounded);
    RUN_TEST(test_managed_antenna_change_retunes_once_and_leaving_controlled_does_not_force_auto);
    RUN_TEST(test_tuning_state_roundtrip_and_state_only_mqtt_updates);
    return UNITY_END();
}
