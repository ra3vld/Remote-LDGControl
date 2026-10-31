#include <unity.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
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
    render();
    TFT_eSPI::instance->flushes.clear();
}
void tearDown() { delete ui; ui=nullptr; lv_deinit(); }

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
        TEST_ASSERT_GREATER_THAN_INT(0, f.x1);
        TEST_ASSERT_LESS_THAN_INT(r.x1, f.x2);
        TEST_ASSERT_LESS_THAN_INT(300, r.x2);
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
void test_mode_button_selects_the_opposite_confirmed_mode() {
    ui->updateStatus(MODE_AUTO,ANT_A); render();
    auto* manual=label(lv_scr_act(),"SET MANUAL"); TEST_ASSERT_NOT_NULL(manual);
    lv_event_send(lv_obj_get_parent(manual),LV_EVENT_CLICKED,nullptr);
    TEST_ASSERT_EQUAL_UINT8('M',lastCommand);
    ui->updateStatus(MODE_MANUAL,ANT_A); render();
    auto* automatic=label(lv_scr_act(),"SET AUTO"); TEST_ASSERT_NOT_NULL(automatic);
    lv_event_send(lv_obj_get_parent(automatic),LV_EVENT_CLICKED,nullptr);
    TEST_ASSERT_EQUAL_UINT8('C',lastCommand);
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
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_scale_label_is_visible_and_matches_the_dial);
    RUN_TEST(test_digital_power_is_visible_for_both_scales_and_distinguishes_zero_from_missing);
    RUN_TEST(test_lcd_keeps_last_band_without_treating_expired_power_as_live);
    RUN_TEST(test_full_and_incremental_meter_draws_produce_the_same_pixels);
    RUN_TEST(test_meter_update_does_not_redraw_the_controls);
    RUN_TEST(test_partial_repaints_match_full_draw_for_both_scales_and_invalid_readings);
    RUN_TEST(test_dial_moves_with_its_widget_instead_of_using_screen_coordinates);
    RUN_TEST(test_lcd_pivots_and_needle_ends_use_the_reduced_dial_radius);
    RUN_TEST(test_mode_button_selects_the_opposite_confirmed_mode);
    RUN_TEST(test_feedback_remains_pending_until_completion_and_then_expires);
    RUN_TEST(test_changed_antenna_names_refresh_without_recreating_the_screen);
    return UNITY_END();
}
