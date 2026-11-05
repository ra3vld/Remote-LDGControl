#include <unity.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include "display_ui.h"
#include "config_manager.h"
#include "mqtt_protocol.h"
#include <WiFi.h>
#include <Wire.h>
#include "../../src/display_ui.cpp"
uint32_t testMillis=0;
extern "C" uint32_t display_test_millis(void) { return testMillis; }
FakeSerial Serial;
FakeWiFi WiFi;
FakeWire Wire;
DisplayConfigManager configManager;
TFT_eSPI* TFT_eSPI::instance=nullptr;
const char* TunerProtocol::bandToString(tuner_band_t band) { return ldg::bandName(band); }
static DisplayUI* ui=nullptr;
static ldg::MeterScale scale;
static uint8_t lastCommand=0;
static bool command(uint8_t cmd) { lastCommand=cmd; return true; }
static void render() { testMillis+=40; ui->loop(); lv_refr_now(nullptr); }
static lv_obj_t* label(lv_obj_t* parent,const char* text) {
    if(lv_obj_check_type(parent,&lv_label_class) && !strcmp(lv_label_get_text(parent),text)) return parent;
    for(uint32_t i=0;i<lv_obj_get_child_cnt(parent);++i) {
        if(auto* found=label(lv_obj_get_child(parent,i),text)) return found;
    }
    return nullptr;
}
void setUp() {
    testMillis=0; lastCommand=0; scale.set(1000);
    strcpy(configManager.config.ant1Name,"Dipole");
    strcpy(configManager.config.ant2Name,"Vertical");
    ui=new DisplayUI;
    TEST_ASSERT_TRUE(ui->begin(command,&scale));
    ui->testRenderer(true,false); // Pixel tests use exact endpoints; motion has separate tests.
    render();
    TFT_eSPI::instance->flushes.clear();
}
void tearDown() {
    lv_mem_monitor_t memory;
    lv_mem_monitor(&memory);
    TEST_ASSERT_LESS_THAN_UINT(LV_MEM_SIZE - 4096, memory.max_used);
    // Keep one LVGL allocator lifetime for the suite. lv_deinit() recreates the
    // pool without resetting its high-water counters in LVGL 8; repeated init
    // then reports phantom usage from discarded screens. Remove the real
    // display/objects between tests and retain the same allocator instead.
    lv_disp_remove(lv_disp_get_default());
    delete ui; ui=nullptr;
}

void test_scale_label_is_visible_and_matches_the_dial() {
    tuner_state_t state; state.meterValid=true; state.meter.forward_power_watts=25;
    state.meter.reflected_power_watts=1; state.meter.swr=1.5; ui->updateState(state);
    scale.set(100); render();
    auto* low=label(lv_scr_act(),"100W"); TEST_ASSERT_NOT_NULL(low);
    lv_area_t area; lv_obj_get_coords(low,&area);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0,area.y1);
    TEST_ASSERT_LESS_THAN_INT(320,area.y2);
    const auto lowPixels=TFT_eSPI::instance->pixels;
    ldg::MeterGeometry geometry; geometry.resize(300,280);
    const auto tip100=geometry.forwardNeedle(0.25f);
    TEST_ASSERT_EQUAL_HEX16(lv_color_hex(0x00ff00).full,lowPixels[tip100.y*480+tip100.x]);
    scale.set(1000); render();
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(),"1000W"));
    TEST_ASSERT_TRUE(lowPixels!=TFT_eSPI::instance->pixels);
    const auto tip1000=geometry.forwardNeedle(0.025f);
    TEST_ASSERT_EQUAL_HEX16(lv_color_hex(0x00ff00).full,TFT_eSPI::instance->pixels[tip1000.y*480+tip1000.x]);
    scale.set(100); render();
    TEST_ASSERT_EQUAL_MEMORY(lowPixels.data(),TFT_eSPI::instance->pixels.data(),lowPixels.size()*sizeof(uint16_t));
}
void test_digital_power_is_visible_for_both_scales_and_distinguishes_zero_from_missing() {
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(), "FWD --"));
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(), "REF --"));
    tuner_state_t state;
    state.meterValid = true;
    state.meter.forward_power_watts = 40;
    state.meter.reflected_power_watts = 1.25;
    state.meter.swr = 1.4;
    for (uint16_t watts : {100, 1000}) {
        scale.set(watts); ui->updateState(state); render();
        auto* forward = label(lv_scr_act(), "FWD 40.0W");
        auto* reflected = label(lv_scr_act(), "REF 1.2W");
        TEST_ASSERT_NOT_NULL(forward);
        TEST_ASSERT_NOT_NULL(reflected);
        lv_area_t f, r; lv_obj_get_coords(forward, &f); lv_obj_get_coords(reflected, &r);
        TEST_ASSERT_GREATER_THAN_INT(0, r.x1);
        TEST_ASSERT_LESS_THAN_INT(f.x1, r.x2);
        TEST_ASSERT_LESS_THAN_INT(300, f.x2);
        TEST_ASSERT_LESS_THAN_INT(280, f.y2);
        TEST_ASSERT_LESS_THAN_INT(280, r.y2);
        bool painted = false;
        for (int y = f.y1; y <= f.y2; ++y) for (int x = f.x1; x <= f.x2; ++x)
            painted |= TFT_eSPI::instance->pixels[y * 480 + x] == lv_color_hex(0x22c55e).full;
        TEST_ASSERT_TRUE(painted);
    }
    state.meterValid = false;
    ui->updateState(state); render();
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(), "FWD --"));
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(), "REF --"));
    state.meterValid = true;
    state.meter.forward_power_watts = state.meter.reflected_power_watts = 0;
    ui->updateState(state); render();
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(), "FWD 0.0W"));
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(), "REF 0.0W"));
    state.meter.forward_power_watts = NAN;
    state.meter.reflected_power_watts = -1;
    ui->updateState(state); render();
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(), "FWD --"));
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(), "REF --"));
}
void test_lcd_keeps_last_band_without_treating_expired_power_as_live() {
    tuner_state_t state;
    state.meterValid = true;
    state.meter.band = state.lastBand = BAND_30M;
    state.meter.forward_power_watts = 40;
    ui->updateState(state); render();
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(), "Band: 30m"));
    state.meterValid = false;
    ui->updateState(state); render();
    auto* held = label(lv_scr_act(), "Band: 30m (last)");
    TEST_ASSERT_NOT_NULL(held);
    lv_area_t area; lv_obj_get_coords(held, &area);
    TEST_ASSERT_LESS_THAN_INT(240, area.x2);
    TEST_ASSERT_LESS_THAN_INT(320, area.y2);
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(), "FWD --"));
    state.meterValid = true;
    state.meter.band = BAND_UNKNOWN;
    ui->updateState(state); render();
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(), "Band: 30m (last)"));
    state.meter.band = state.lastBand = BAND_40M;
    ui->updateState(state); render();
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(), "Band: 40m"));
    ui->updateState(tuner_state_t{}); render();
    TEST_ASSERT_NULL(label(lv_scr_act(), "Band: 40m"));
}
void test_full_and_incremental_meter_draws_produce_the_same_pixels() {
    tuner_state_t state; state.meterValid=true; state.meter.forward_power_watts=25;
    state.meter.reflected_power_watts=1; state.meter.swr=1.5;
    ui->updateState(state); render();
    const auto incremental=TFT_eSPI::instance->pixels;
    lv_obj_invalidate(lv_scr_act()); render();
    TEST_ASSERT_EQUAL_MEMORY(incremental.data(),TFT_eSPI::instance->pixels.data(),incremental.size()*sizeof(uint16_t));
}
void test_meter_update_does_not_redraw_the_controls() {
    tuner_state_t state; state.meterValid=true; state.meter.forward_power_watts=20;
    state.meter.reflected_power_watts=1; state.meter.swr=1.5;
    ui->updateState(state); render();
    TEST_ASSERT_GREATER_THAN_UINT(0,TFT_eSPI::instance->flushes.size());
    unsigned pixels=0;
    for(const auto& flush:TFT_eSPI::instance->flushes) {
        TEST_ASSERT_LESS_OR_EQUAL_INT(310,flush.x+flush.w);
        pixels+=flush.w*flush.h;
    }
    std::printf("Meter update: %u pixels transferred vs 84000 for a complete dial\n",pixels);
    TEST_ASSERT_LESS_THAN_UINT(42000,pixels);
}
void test_partial_repaints_match_full_draw_for_both_scales_and_invalid_readings() {
    tuner_state_t state; state.meterValid=true;
    for(uint16_t watts:{100,1000}) {
        scale.set(watts); render();
        for(float power:{0.f,25.f,75.f,100.f,500.f,1000.f,1.f,0.f}) {
            state.meter.forward_power_watts=power;
            state.meter.reflected_power_watts=power/20;
            state.meter.swr=power>0?1.5:NAN;
            ui->updateState(state); render();
            const auto incremental=TFT_eSPI::instance->pixels;
            lv_obj_invalidate(lv_scr_act()); render();
            TEST_ASSERT_EQUAL_MEMORY(incremental.data(),TFT_eSPI::instance->pixels.data(),incremental.size()*sizeof(uint16_t));
        }
    }
}

void test_custom_log_scale_matches_needle_power_and_partial_repaints() {
    tuner_state_t state; state.meterValid=true;
    state.meter.forward_power_watts=60; state.meter.reflected_power_watts=6; state.meter.swr=1.5;
    scale.set(600,true); ui->updateState(state); render();
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(),"600W LOG"));
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(),"FWD 60.0W"));
    ldg::MeterGeometry geometry; geometry.resize(300,280,true);
    const auto tip=geometry.forwardNeedle(ldg::meterFraction(60,600,true));
    TEST_ASSERT_EQUAL_HEX16(lv_color_hex(0x00ff00).full,TFT_eSPI::instance->pixels[tip.y*480+tip.x]);
    for (const uint16_t maximum : {1,50,600,1000}) for (const bool log : {false,true}) {
        scale.set(maximum,log); render();
        for (const float ratio : {0.f,0.01f,0.1f,0.8f,1.f,0.f}) {
            state.meter.forward_power_watts=ratio*maximum;
            state.meter.reflected_power_watts=ratio*maximum/10;
            ui->updateState(state); render();
            const auto incremental=TFT_eSPI::instance->pixels;
            lv_obj_invalidate(lv_scr_act()); render();
            TEST_ASSERT_EQUAL_MEMORY(incremental.data(),TFT_eSPI::instance->pixels.data(),incremental.size()*sizeof(uint16_t));
        }
    }
}
void test_fast_telemetry_bursts_render_only_the_latest_sample_without_full_screen_refresh() {
    tuner_state_t state; state.meterValid=true; state.meter.swr=1.2;
    state.meter.band=BAND_40M;
    ui->updateState(state); render();
    TFT_eSPI::instance->flushes.clear();
    for (int packet=1;packet<=100;++packet) {
        state.meter.forward_power_watts=packet%2 ? 900 : 30;
        state.meter.reflected_power_watts=packet%2 ? 90 : 3;
        ui->updateState(state);
    }
    render();
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(),"FWD 30.0W"));
    unsigned pixels=0;
    for (const auto& flush:TFT_eSPI::instance->flushes) {
        TEST_ASSERT_LESS_OR_EQUAL_INT(310,flush.x+flush.w);
        pixels+=flush.w*flush.h;
    }
    std::printf("100 queued RF samples: %u transferred pixels; full screen is 153600\n",pixels);
    TEST_ASSERT_LESS_THAN_UINT(42000,pixels);
    const auto incremental=TFT_eSPI::instance->pixels;
    lv_obj_invalidate(lv_scr_act()); render();
    TEST_ASSERT_EQUAL_MEMORY(incremental.data(),TFT_eSPI::instance->pixels.data(),incremental.size()*sizeof(uint16_t));
}

void test_compressed_face_is_pixel_identical_to_vectors_for_ranges_log_and_widget_moves() {
    auto* meter=lv_obj_get_child(lv_scr_act(),0);
    tuner_state_t state;state.meterValid=true;state.meter.swr=1.8;
    for (const bool log : {false,true}) for (const uint16_t watts : {1,100,600,1000}) {
        scale.set(watts,log);state.meter.forward_power_watts=watts*0.37;
        state.meter.reflected_power_watts=watts*0.04;ui->updateState(state);
        ui->testRenderer(false,false);render();
        const auto reference=TFT_eSPI::instance->pixels;
        ui->testRenderer(true,false);render();
        TEST_ASSERT_EQUAL_MEMORY(reference.data(),TFT_eSPI::instance->pixels.data(),reference.size()*sizeof(uint16_t));
        JsonDocument d;ui->writeDiagnostics(d.to<JsonObject>());
        TEST_ASSERT_GREATER_THAN_UINT(0,d["face_bytes"].as<unsigned>());
        TEST_ASSERT_LESS_OR_EQUAL_UINT(ldg::MeterFaceCache::MaxBytes,d["face_bytes"].as<unsigned>());
    }
    lv_obj_set_pos(meter,7,9);ui->testRenderer(false,false);render();
    const auto moved=TFT_eSPI::instance->pixels;
    ui->testRenderer(true,false);render();
    TEST_ASSERT_EQUAL_MEMORY(moved.data(),TFT_eSPI::instance->pixels.data(),moved.size()*sizeof(uint16_t));
}
void test_warm_needle_render_benchmark_and_cache_reuse() {
    tuner_state_t state;state.meterValid=true;state.meter.swr=1.2;state.meter.band=BAND_40M;
    const auto benchmark=[&](bool cached) {
        ui->testRenderer(cached,false);ui->testDelta(cached);ui->testLegacyNeedles(!cached);render();
        TFT_eSPI::instance->flushes.clear();
        auto start=std::chrono::steady_clock::now();
        for(int frame=0;frame<100;++frame) {
            state.meter.forward_power_watts=frame%2 ? 550 : 450;
            state.meter.reflected_power_watts=frame%2 ? 25 : 15;
            ui->updateState(state);render();
        }
        const double us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count()/100;
        unsigned pixels=0;for(const auto& flush:TFT_eSPI::instance->flushes)pixels+=flush.w*flush.h;
        std::printf("%s: %u pixels/frame, %zu SPI windows/frame\n",cached?"Fast renderer":"Legacy renderer",pixels/100,TFT_eSPI::instance->flushes.size()/100);
        return us;
    };
    const double vectorUs=benchmark(false), cachedUs=benchmark(true);
    JsonDocument d;ui->writeDiagnostics(d.to<JsonObject>());
    const auto builds=d["face_builds"].as<unsigned>(), vectors=d["vector_draws"].as<unsigned>();
    state.meter.forward_power_watts=100;ui->updateState(state);render();
    ui->writeDiagnostics(d.as<JsonObject>());
    TEST_ASSERT_EQUAL_UINT(builds,d["face_builds"].as<unsigned>());
    TEST_ASSERT_EQUAL_UINT(vectors,d["vector_draws"].as<unsigned>());
    std::printf("Needle CPU benchmark: vector %.1f us/frame, cached %.1f us/frame, %.2fx; face %u bytes\n",
        vectorUs,cachedUs,vectorUs/cachedUs,d["face_bytes"].as<unsigned>());
}
void test_animated_needle_settles_to_latest_data_without_changing_digital_measurement() {
    ui->testRenderer(true,true);
    tuner_state_t state;state.meterValid=true;state.meter.forward_power_watts=1000;state.meter.swr=1;
    ui->updateState(state);render();
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(),"FWD 1000.0W"));
    auto first=TFT_eSPI::instance->pixels;
    render();TEST_ASSERT_TRUE(first!=TFT_eSPI::instance->pixels);
    state.meter.forward_power_watts=40;ui->updateState(state);
    for(int i=0;i<20;++i)render();
    const auto settled=TFT_eSPI::instance->pixels;
    ui->testRenderer(true,false);ui->updateState(state);render();
    TEST_ASSERT_EQUAL_MEMORY(settled.data(),TFT_eSPI::instance->pixels.data(),settled.size()*sizeof(uint16_t));
}
void test_dial_moves_with_its_widget_instead_of_using_screen_coordinates() {
    auto* meter=lv_obj_get_child(lv_scr_act(),0);
    lv_obj_set_pos(meter,7,9); render();
    const int x=270+7,y=260+9;
    TEST_ASSERT_EQUAL_HEX16(lv_color_hex(0x00aa00).full,TFT_eSPI::instance->pixels[y*480+x]);
}
void test_lcd_pivots_and_needle_ends_use_the_reduced_dial_radius() {
    tuner_state_t state;
    state.meterValid = true;
    state.meter.forward_power_watts = state.meter.reflected_power_watts = 1000;
    ui->updateState(state); render();
    const auto& pixels = TFT_eSPI::instance->pixels;
    TEST_ASSERT_EQUAL_HEX16(lv_color_hex(0x00aa00).full, pixels[260*480+270]);
    TEST_ASSERT_EQUAL_HEX16(lv_color_hex(0xaa0000).full, pixels[260*480+30]);
    TEST_ASSERT_EQUAL_HEX16(lv_color_hex(0x00ff00).full, pixels[20*480+270]);
    TEST_ASSERT_EQUAL_HEX16(lv_color_hex(0xff0000).full, pixels[20*480+30]);
    state.meter.forward_power_watts = state.meter.reflected_power_watts = 0;
    ui->updateState(state); render();
    TEST_ASSERT_EQUAL_HEX16(lv_color_hex(0x00aa00).full, pixels[260*480+270]);
    TEST_ASSERT_EQUAL_HEX16(lv_color_hex(0xaa0000).full, pixels[260*480+30]);
}
void test_mode_button_cycles_manual_auto_controlled() {
    ui->updateStatus(MODE_AUTO,ANT_A); render();
    auto* controlled=label(lv_scr_act(),"CONTROLLED"); TEST_ASSERT_NOT_NULL(controlled);
    TEST_ASSERT_LESS_OR_EQUAL_INT(lv_obj_get_content_width(lv_obj_get_parent(controlled)),lv_obj_get_width(controlled));
    lv_event_send(lv_obj_get_parent(controlled),LV_EVENT_CLICKED,nullptr);
    TEST_ASSERT_EQUAL_UINT8(TUNER_CMD_CONTROLLED_MODE,lastCommand);
    ui->updateStatus(MODE_CONTROLLED,ANT_A,ANT_A,MODE_MANUAL); render();
    auto* manual=label(lv_scr_act(),"SET MANUAL"); TEST_ASSERT_NOT_NULL(manual);
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(),"Mode: CONTROLLED"));
    tuner_state_t temporary; temporary.tuningManaged = true; temporary.mode = MODE_AUTO;
    ui->updateState(temporary); render();
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(),"Mode: CONTROLLED"));
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(),"SET MANUAL"));
    lv_event_send(lv_obj_get_parent(manual),LV_EVENT_CLICKED,nullptr);
    TEST_ASSERT_EQUAL_UINT8(TUNER_CMD_MANUAL_MODE,lastCommand);
    ui->updateStatus(MODE_MANUAL,ANT_A); render();
    auto* automatic=label(lv_scr_act(),"SET AUTO"); TEST_ASSERT_NOT_NULL(automatic);
    lv_event_send(lv_obj_get_parent(automatic),LV_EVENT_CLICKED,nullptr);
    TEST_ASSERT_EQUAL_UINT8('C',lastCommand);
}
void test_bypass_button_exits_via_auto_and_waits_for_confirmed_state() {
    ui->updateStatus(MODE_MANUAL,ANT_A); render();
    auto* enable=label(lv_scr_act(),"BYPASS ON"); TEST_ASSERT_NOT_NULL(enable);
    lv_event_send(lv_obj_get_parent(enable),LV_EVENT_CLICKED,nullptr);
    TEST_ASSERT_EQUAL_UINT8('P',lastCommand);
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(),"BYPASS ON"));
    ui->updateStatus(MODE_BYPASS,ANT_A); render();
    auto* disable=label(lv_scr_act(),"BYPASS OFF"); TEST_ASSERT_NOT_NULL(disable);
    lv_event_send(lv_obj_get_parent(disable),LV_EVENT_CLICKED,nullptr);
    TEST_ASSERT_EQUAL_UINT8('C',lastCommand);
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(),"BYPASS OFF"));
    ui->updateStatus(MODE_AUTO,ANT_A); render();
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(),"BYPASS ON"));
}
void test_feedback_remains_pending_until_completion_and_then_expires() {
    ldg::CommandFeedback feedback;
    const auto request=feedback.start('F');
    ui->updateCommandFeedback(feedback.snapshot()); render();
    testMillis+=20000; render();
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(),"FULL TUNE: waiting..."));
    feedback.finish(request,false);
    ui->updateCommandFeedback(feedback.snapshot()); render();
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(),"FULL TUNE: not confirmed"));
    testMillis+=4000; render();
    TEST_ASSERT_NULL(label(lv_scr_act(),"FULL TUNE: not confirmed"));
    const auto second=feedback.start('C'); feedback.finish(second,true);
    ui->updateCommandFeedback(feedback.snapshot()); render();
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(),"AUTO: OK"));
}
void test_changed_antenna_names_refresh_without_recreating_the_screen() {
    auto* screen=lv_scr_act();
    strcpy(configManager.config.ant1Name,"New name");
    testMillis+=1000; render();
    TEST_ASSERT_EQUAL_PTR(screen,lv_scr_act());
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(),"New name"));
    if(const char* path=std::getenv("LDG_DISPLAY_SNAPSHOT")) {
        tuner_state_t state;
        state.meterValid = true;
        state.meter.forward_power_watts = 40;
        state.meter.reflected_power_watts = 1.2;
        state.meter.swr = 1.5;
        state.lastBand = BAND_30M;
        ui->updateState(state); render();
        if(auto* file=std::fopen(path,"wb")) {
            std::fprintf(file,"P6\n480 320\n255\n");
            for(uint16_t pixel:TFT_eSPI::instance->pixels) {
                const unsigned char rgb[]={uint8_t(((pixel>>11)&31)*255/31),uint8_t(((pixel>>5)&63)*255/63),uint8_t((pixel&31)*255/31)};
                std::fwrite(rgb,1,3,file);
            }
            std::fclose(file);
        }
    }
}
void test_antenna_initialization_status_is_visible_and_changes_without_recreating_screen() {
    tuner_state_t state;
    auto* screen = lv_scr_act();
    for (auto item : {std::make_pair(ldg::AntennaInit::Waiting, "ANT: waiting for live power"),
                      std::make_pair(ldg::AntennaInit::WaitingRfOff, "ANT: waiting for RF off"),
                      std::make_pair(ldg::AntennaInit::Checking, "ANT: checking"),
                      std::make_pair(ldg::AntennaInit::Restoring, "ANT: restoring original"),
                      std::make_pair(ldg::AntennaInit::Ready, "ANT: Ready"),
                      std::make_pair(ldg::AntennaInit::Error, "ANT: check failed - select manually")}) {
        state.antennaInit = item.first;
        ui->updateState(state); render();
        auto* status = label(screen, item.second);
        TEST_ASSERT_NOT_NULL(status);
        lv_area_t area; lv_obj_get_coords(status, &area);
        TEST_ASSERT_LESS_THAN_INT(320, area.y2);
        TEST_ASSERT_LESS_THAN_INT(300, area.x2);
        TEST_ASSERT_TRUE(screen == lv_scr_act());
    }
}

void test_automatic_antenna_reasons_are_visible_on_lcd() {
    tuner_state_t state;
    state.antennaInit = ldg::AntennaInit::Ready;
    state.antenna = state.lastAntenna = ANT_A;
    auto* screen = lv_scr_act();
    for (auto item : {std::make_pair(ldg::AntennaAuto::HighPower, "ANT: Ready; blocked: power >=100W"),
                      std::make_pair(ldg::AntennaAuto::InvalidPower, "ANT: Ready; blocked: no fresh power"),
                      std::make_pair(ldg::AntennaAuto::Unassigned, "ANT: Ready; table: do not switch"),
                      std::make_pair(ldg::AntennaAuto::ManualSelection, "ANT: Ready; user selected manually"),
                      std::make_pair(ldg::AntennaAuto::Manual, "ANT: Ready; cancelled by user"),
                      std::make_pair(ldg::AntennaAuto::Retrying, "ANT: Ready; retrying selection"),
                      std::make_pair(ldg::AntennaAuto::WaitingMeter, "ANT: Ready; waiting for fresh power"),
                      std::make_pair(ldg::AntennaAuto::Tuning, "ANT: Ready; selected; memory tuning"),
                      std::make_pair(ldg::AntennaAuto::TuneFailed, "ANT: Ready; selected; MEM TUNE failed"),
                      std::make_pair(ldg::AntennaAuto::TuneNoRf, "ANT: Ready; selected; no RF for tuning"),
                      std::make_pair(ldg::AntennaAuto::TuneHighPower, "ANT: Ready; selected; tune blocked >=100W"),
                      std::make_pair(ldg::AntennaAuto::TuneInvalidPower, "ANT: Ready; selected; no fresh tune power"),
                      std::make_pair(ldg::AntennaAuto::TuneBandChanged, "ANT: Ready; selected; tune band changed"),
                      std::make_pair(ldg::AntennaAuto::TuneManual, "ANT: Ready; selected; tune cancelled by user"),
                      std::make_pair(ldg::AntennaAuto::WaitingTuner, "ANT: Ready; waiting tuner; choice saved")}) {
        state.antennaAuto = item.first;
        ui->updateState(state); render();
        TEST_ASSERT_NOT_NULL(label(screen, item.second));
        TEST_ASSERT_TRUE(screen == lv_scr_act());
    }
}

void test_last_confirmed_antenna_is_visible_as_unconfirmed_after_a_failed_command() {
    tuner_state_t state;
    state.antennaInit = ldg::AntennaInit::Ready;
    state.antenna = state.lastAntenna = ANT_B;
    ui->updateState(state); render();
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(), "ANT: Ready"));
    state.antenna = ANT_UNKNOWN;
    ui->updateState(state); render();
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(), "ANT: last 2 (unconfirmed)"));
    state.antenna = ANT_A;
    ui->updateState(state); render();
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(), "ANT: Ready"));
    TEST_ASSERT_NULL(label(lv_scr_act(), "ANT: last 2 (unconfirmed)"));
}

void test_tci_frequency_and_connection_fit_above_ip_and_wait_reason_is_visible() {
    tuner_state_t state;
    state.radio.connection = ldg::RadioConnection::Ready;
    state.radio.frequencyValid = state.radio.rxKnown = state.radio.tuneKnown = true;
    state.radio.txFrequencyHz = 14074000;
    state.antennaFromRadio = true;
    state.antennaInit = ldg::AntennaInit::Ready;
    state.antennaAuto = ldg::AntennaAuto::WaitingRx;
    ui->updateState(state); render();
    auto* radio = label(lv_scr_act(),"TCI RX 14.074000 20m\nready / TCI");
    TEST_ASSERT_NOT_NULL(radio);
    TEST_ASSERT_FALSE(lv_obj_has_flag(radio,LV_OBJ_FLAG_HIDDEN));
    lv_area_t area; lv_obj_get_coords(radio,&area);
    TEST_ASSERT_LESS_THAN_INT(480,area.x2);
    TEST_ASSERT_LESS_THAN_INT(22,area.y2);
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(),"ANT: Ready; TCI: waiting RX (no tune)"));
    const char* capture = getenv("LDG_TCI_LCD_PPM");
    if (capture) {
        FILE* file = fopen(capture,"wb"); TEST_ASSERT_NOT_NULL(file);
        fprintf(file,"P6\n480 320\n255\n");
        for (uint16_t color : TFT_eSPI::instance->pixels) {
            const uint8_t rgb[] = {uint8_t((color>>11)*255/31),uint8_t(((color>>5)&63)*255/63),uint8_t((color&31)*255/31)};
            fwrite(rgb,1,3,file);
        }
        fclose(file);
    }
    state.radio.connection = ldg::RadioConnection::Disconnected;
    state.radio.frequencyValid = state.radio.rxKnown = false; state.antennaFromRadio = false;
    ui->updateState(state); render();
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(),"TCI ? --\ndisconnected / LDG"));
    ui->updateState(tuner_state_t{}); render();
    TEST_ASSERT_NOT_NULL(label(lv_scr_act(),"SSID: Test WiFi"));
    TEST_ASSERT_NULL(label(lv_scr_act(),"TCI ? --\ndisconnected / LDG"));
}

void test_managed_tuning_progress_updates_without_rebuilding_screen() {
    tuner_state_t state; state.tuningManaged = true;
    auto* screen = lv_scr_act();
    state.tuningState = ldg::TuningState::MemoryTune; ui->updateState(state); render();
    TEST_ASSERT_NOT_NULL(label(screen,"MEM TUNE"));
    state.tuningState = ldg::TuningState::AutoActive; ui->updateState(state); render();
    TEST_ASSERT_NOT_NULL(label(screen,"AUTO until RF off"));
    state.tuningState = ldg::TuningState::ReturningManual; ui->updateState(state); render();
    TEST_ASSERT_NOT_NULL(label(screen,"returning to MANUAL"));
    TEST_ASSERT_EQUAL_PTR(screen,lv_scr_act());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_managed_tuning_progress_updates_without_rebuilding_screen);
    RUN_TEST(test_tci_frequency_and_connection_fit_above_ip_and_wait_reason_is_visible);
    RUN_TEST(test_last_confirmed_antenna_is_visible_as_unconfirmed_after_a_failed_command);
    RUN_TEST(test_antenna_initialization_status_is_visible_and_changes_without_recreating_screen);
    RUN_TEST(test_automatic_antenna_reasons_are_visible_on_lcd);
    RUN_TEST(test_scale_label_is_visible_and_matches_the_dial);
    RUN_TEST(test_digital_power_is_visible_for_both_scales_and_distinguishes_zero_from_missing);
    RUN_TEST(test_lcd_keeps_last_band_without_treating_expired_power_as_live);
    RUN_TEST(test_full_and_incremental_meter_draws_produce_the_same_pixels);
    RUN_TEST(test_meter_update_does_not_redraw_the_controls);
    RUN_TEST(test_partial_repaints_match_full_draw_for_both_scales_and_invalid_readings);
    RUN_TEST(test_dial_moves_with_its_widget_instead_of_using_screen_coordinates);
    RUN_TEST(test_lcd_pivots_and_needle_ends_use_the_reduced_dial_radius);
    RUN_TEST(test_mode_button_cycles_manual_auto_controlled);
    RUN_TEST(test_bypass_button_exits_via_auto_and_waits_for_confirmed_state);
    RUN_TEST(test_custom_log_scale_matches_needle_power_and_partial_repaints);
    RUN_TEST(test_fast_telemetry_bursts_render_only_the_latest_sample_without_full_screen_refresh);
    RUN_TEST(test_compressed_face_is_pixel_identical_to_vectors_for_ranges_log_and_widget_moves);
    RUN_TEST(test_warm_needle_render_benchmark_and_cache_reuse);
    RUN_TEST(test_animated_needle_settles_to_latest_data_without_changing_digital_measurement);
    RUN_TEST(test_feedback_remains_pending_until_completion_and_then_expires);
    RUN_TEST(test_changed_antenna_names_refresh_without_recreating_the_screen);
    lv_deinit();
    return UNITY_END();
}
