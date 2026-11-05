#ifdef WITH_DISPLAY

#include "display_ui.h"
#include "config_manager.h"
#include <WiFi.h>
#include <Wire.h>
#include <math.h>
#include <algorithm>
#include "display_backlight.h"
#include <src/draw/sw/lv_draw_sw.h>

DisplayUI* DisplayUI::s_instance = nullptr;

#ifdef TOUCH_GT911
#include "gt911_touch.h"
static Gt911Touch<TwoWire> touch(Wire);
#else
// NS2009 I2C touch controller (BTT TFT35-SPI V2.1)
// I2C address: 0x48
#define NS2009_ADDR 0x48
#define NS2009_CMD_READ_X 0xC0
#define NS2009_CMD_READ_Y 0xD0

static bool ns2009Read(uint16_t* x, uint16_t* y) {
    Wire.requestFrom(NS2009_ADDR, (uint8_t)5);
    if (Wire.available() < 5) return false;
    Wire.read(); // skip status
    uint8_t xh = Wire.read();
    uint8_t xl = Wire.read();
    uint8_t yh = Wire.read();
    uint8_t yl = Wire.read();
    *x = (xh << 5) | (xl >> 3);
    *y = (yh << 5) | (yl >> 3);
    return true;
}

static bool tsTouched() {
    Wire.beginTransmission(NS2009_ADDR);
    Wire.write(NS2009_CMD_READ_X);
    return (Wire.endTransmission() == 0);
}
#endif

DisplayUI::DisplayUI()
    : m_tft(nullptr), m_commandHandler(nullptr), m_meterScale(nullptr),
      m_scr(nullptr), m_meter(nullptr),
      m_bandLabel(nullptr), m_scaleLabel(nullptr),
      m_modeLabel(nullptr),
      m_ssidLabel(nullptr), m_ipLabel(nullptr),
      m_btnMemTune(nullptr), m_btnFullTune(nullptr),
      m_btnAnt1(nullptr), m_btnAnt2(nullptr),
      m_ant1NameLabel(nullptr), m_ant2NameLabel(nullptr),
      m_btnBypass(nullptr), m_btnAuto(nullptr),
      m_maxPower(1000.0f),
      m_fwdPower(0), m_refPower(0), m_swr(NAN),
      m_activeAnt(ANT_UNKNOWN) {
    s_instance = this;
}

DisplayUI::~DisplayUI() {
    std::free(m_deltaScratch);
    if (m_tft) delete m_tft;
    s_instance = nullptr;
}

bool DisplayUI::begin(bool (*commandHandler)(uint8_t), ldg::MeterScale* meterScale) {
    m_commandHandler = commandHandler;
    m_meterScale = meterScale;
#ifdef BOARD_ESP32_3248S035C
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, LOW);
    pinMode(5, OUTPUT);
    digitalWrite(5, HIGH);  // Deselect the onboard microSD.
    const uint8_t ledPins[] = {4, 16, 17};
    for (uint8_t pin : ledPins) {
        pinMode(pin, OUTPUT);
        digitalWrite(pin, HIGH);  // Active-low RGB LED off.
    }
#endif
    m_tft = new TFT_eSPI();
    m_tft->init();
    m_tft->setRotation(DISPLAY_ROTATION);
    m_tft->fillScreen(TFT_BLACK);

    lv_init();

    static lv_disp_draw_buf_t draw_buf;
    // SPI flushing is synchronous, so a second buffer cannot overlap transfer
    // with rendering. One 10-line buffer uses the same RAM as two 5-line
    // buffers and halves the number of custom draw callbacks per refresh.
    static lv_color_t buf[DISPLAY_WIDTH * 10];
    lv_disp_draw_buf_init(&draw_buf, buf, nullptr, DISPLAY_WIDTH * 10);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = DISPLAY_WIDTH;
    disp_drv.ver_res = DISPLAY_HEIGHT;
    disp_drv.flush_cb = displayFlush;
    disp_drv.monitor_cb = displayMonitor;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);
    m_deltaScratch = static_cast<lv_color_t*>(std::malloc(DISPLAY_WIDTH * 10 * sizeof(lv_color_t)));

    setupTouch();

    m_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(m_scr, lv_color_hex(0x0a0a1a), LV_PART_MAIN);
    lv_obj_set_style_border_width(m_scr, 0, LV_PART_MAIN);
    lv_obj_clear_flag(m_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_scr_load(m_scr);

    // ============================================================
    // Leave a complete footer row below the dial inside the 320 px display.
    // ============================================================
    m_meter = lv_obj_create(m_scr);
    lv_obj_set_size(m_meter, 300, 280);
    lv_obj_set_pos(m_meter, 0, 0);
    lv_obj_set_style_bg_color(m_meter, lv_color_hex(0x111122), LV_PART_MAIN);
    lv_obj_set_style_border_width(m_meter, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(m_meter, 0, LV_PART_MAIN);
    lv_obj_clear_flag(m_meter, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(m_meter, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(m_meter, drawMeterEvent, LV_EVENT_DRAW_POST, NULL);
    lv_obj_add_event_cb(m_meter, meterTapEvent, LV_EVENT_CLICKED, NULL);

    // Band label
    m_bandLabel = lv_label_create(m_scr);
    lv_label_set_text(m_bandLabel, "Band: --");
    lv_obj_set_style_text_color(m_bandLabel, lv_color_hex(0x00ff00), LV_PART_MAIN);
    lv_obj_set_style_text_font(m_bandLabel, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align_to(m_bandLabel, m_meter, LV_ALIGN_OUT_BOTTOM_LEFT, 10, 5);

    // Scale label
    m_scaleLabel = lv_label_create(m_scr);
    lv_label_set_text(m_scaleLabel, "1000W");
    lv_obj_set_style_text_color(m_scaleLabel, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_set_style_text_font(m_scaleLabel, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align_to(m_scaleLabel, m_meter, LV_ALIGN_OUT_BOTTOM_RIGHT, -10, 5);

    // Digital readings fit below the pivots, above the band/scale footer.
    auto powerLabel = [&](int x, const char* text, uint32_t color) {
        lv_obj_t* label = lv_label_create(m_scr);
        lv_label_set_text(label, text);
        lv_obj_set_pos(label, x, 264);
        lv_obj_set_width(label, 138);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
        return label;
    };
    // Match the browser and needle pivots: reflected left, forward right.
    m_refPowerLabel = powerLabel(8, "REF --", 0xef4444);
    m_fwdPowerLabel = powerLabel(156, "FWD --", 0x22c55e);
    m_antennaInitLabel = lv_label_create(m_scr);
    lv_obj_set_pos(m_antennaInitLabel, 8, 305);
    lv_obj_set_width(m_antennaInitLabel, 290);
    lv_obj_set_style_text_font(m_antennaInitLabel, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_label_set_long_mode(m_antennaInitLabel, LV_LABEL_LONG_CLIP);
    lv_label_set_text(m_antennaInitLabel, "");

    // ============================================================
    // RIGHT PANEL: Info + buttons (~150px wide)
    // ============================================================
    int rightX = 310;

    // SSID
    m_ssidLabel = lv_label_create(m_scr);
    lv_label_set_text_fmt(m_ssidLabel, "SSID: %s", WiFi.SSID().c_str());
    lv_obj_set_style_text_color(m_ssidLabel, lv_color_hex(0x666666), LV_PART_MAIN);
    lv_obj_set_style_text_font(m_ssidLabel, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(m_ssidLabel, LV_ALIGN_TOP_RIGHT, -5, 5);

    // IP
    m_ipLabel = lv_label_create(m_scr);
    lv_label_set_text_fmt(m_ipLabel, "IP: %s", WiFi.localIP().toString().c_str());
    lv_obj_set_style_text_color(m_ipLabel, lv_color_hex(0x666666), LV_PART_MAIN);
    lv_obj_set_style_text_font(m_ipLabel, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(m_ipLabel, LV_ALIGN_TOP_RIGHT, -5, 22);

    // Mode
    m_modeLabel = lv_label_create(m_scr);
    lv_label_set_text(m_modeLabel, "Mode: --");
    lv_obj_set_style_text_color(m_modeLabel, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_font(m_modeLabel, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(m_modeLabel, LV_ALIGN_TOP_RIGHT, -5, 45);

    // Buttons — full-width single-line buttons for tuning commands
    int btnX = rightX + 5;
    int btnW = 140;
    int btnH = 36;
    int btnGap = 5;

    auto makeBtn = [&](lv_obj_t** out, const char* label, int x, int y, int w, int h,
                       lv_color_t bg, lv_color_t pressed) {
        *out = lv_btn_create(m_scr);
        lv_obj_set_size(*out, w, h);
        lv_obj_set_pos(*out, x, y);
        lv_obj_set_style_bg_color(*out, bg, LV_PART_MAIN);
        lv_obj_set_style_bg_color(*out, pressed, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_radius(*out, 4, LV_PART_MAIN);
        lv_obj_t* lbl = lv_label_create(*out);
        lv_label_set_text(lbl, label);
        lv_obj_center(lbl);
    };

    // Buttons start just below the Mode label (y=45 + ~18px font height + 4px gap)
    const int btnY0 = 67;

    makeBtn(&m_btnMemTune,  "MEM TUNE",  btnX, btnY0,              btnW, btnH,
            lv_color_hex(0xcc3333), lv_color_hex(0xff4444));
    makeBtn(&m_btnFullTune, "FULL TUNE", btnX, btnY0 + btnH + btnGap, btnW, btnH,
            lv_color_hex(0xcc3333), lv_color_hex(0xff4444));

    // ANT buttons — side-by-side, nearly square, two-line (identifier + configurable name)
    // Width = (btnW - gap) / 2 = 67 each; height = 67 for near-square
    const int antW = (btnW - btnGap) / 2;   // 67
    const int antH = antW;                   // 67 — square
    const int antY = btnY0 + 2 * (btnH + btnGap);
    const DeviceConfig& cfg = configManager.get();

    auto makeAntBtn = [&](lv_obj_t** btn, lv_obj_t** nameLabel,
                          const char* idText, const char* name, int x) {
        *btn = lv_btn_create(m_scr);
        lv_obj_set_size(*btn, antW, antH);
        lv_obj_set_pos(*btn, x, antY);
        lv_obj_set_style_bg_color(*btn, lv_color_hex(0x2a2a3a), LV_PART_MAIN);
        lv_obj_set_style_radius(*btn, 4, LV_PART_MAIN);
        lv_obj_set_style_pad_all(*btn, 2, LV_PART_MAIN);

        lv_obj_t* id = lv_label_create(*btn);
        lv_label_set_text(id, idText);
        lv_obj_set_style_text_font(id, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(id, LV_ALIGN_TOP_MID, 0, 4);

        *nameLabel = lv_label_create(*btn);
        lv_label_set_text(*nameLabel, name);
        lv_obj_set_style_text_font(*nameLabel, &lv_font_montserrat_10, LV_PART_MAIN);
        lv_obj_set_style_text_color(*nameLabel, lv_color_hex(0xaaaaaa), LV_PART_MAIN);
        lv_label_set_long_mode(*nameLabel, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(*nameLabel, antW - 4);
        lv_obj_set_style_text_align(*nameLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(*nameLabel, LV_ALIGN_BOTTOM_MID, 0, -4);
    };

    makeAntBtn(&m_btnAnt1, &m_ant1NameLabel, "ANT 1", cfg.ant1Name, btnX);
    makeAntBtn(&m_btnAnt2, &m_ant2NameLabel, "ANT 2", cfg.ant2Name, btnX + antW + btnGap);

    makeBtn(&m_btnBypass, "BYPASS ON", btnX, antY + antH + btnGap, btnW, btnH,
            lv_color_hex(0xcc8800), lv_color_hex(0xffaa00));
    m_bypassButtonLabel = lv_obj_get_child(m_btnBypass, 0);
    makeBtn(&m_btnAuto,   "SET AUTO", btnX, antY + antH + btnGap + btnH + btnGap, btnW, btnH,
            lv_color_hex(0x2255aa), lv_color_hex(0x3366cc));
    m_modeButtonLabel = lv_obj_get_child(m_btnAuto, 0);
    m_commandLabel = lv_label_create(m_scr);
    lv_obj_set_pos(m_commandLabel, btnX, 303);
    lv_obj_set_width(m_commandLabel, 150);
    lv_obj_set_style_text_font(m_commandLabel, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_label_set_long_mode(m_commandLabel, LV_LABEL_LONG_CLIP);
    lv_label_set_text(m_commandLabel, "");

    lv_obj_add_event_cb(m_btnMemTune,  btnMemTuneEvent,  LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(m_btnFullTune, btnFullTuneEvent, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(m_btnAnt1,     btnAnt1Event,     LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(m_btnAnt2,     btnAnt2Event,     LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(m_btnBypass,   btnBypassEvent,   LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(m_btnAuto,     btnAutoEvent,     LV_EVENT_CLICKED, NULL);
    updateMeterScale();

#ifdef BOARD_ESP32_3248S035C
    lv_refr_now(NULL);
    displayBacklight.begin();
    Serial.println("Display: Sunton ST7796 480x320 ready");
#endif
    return true;
}

void DisplayUI::loop() {
#ifdef BOARD_ESP32_3248S035C
    displayBacklight.loop();
#endif
    // Web requests only change the atomic model; all LVGL calls stay here.
    updateMeterScale();
    // UART can publish much faster than the LCD refreshes. Consume only the
    // latest sample once per frame so invalid rectangles cannot accumulate
    // across intermediate samples and overflow LVGL's invalid-area list.
    if (millis() - m_lastMeterRender >= LV_DISP_DEF_REFR_PERIOD) {
        if (m_meterPending) {
            updateMeter(&m_pendingMeter, m_pendingBandHeld);
            m_meterPending = false;
        }
        animateNeedles(millis());
        m_lastMeterRender = millis();
    }
    if (m_feedbackFinished && millis() - m_feedbackAt >= 4000) {
        lv_label_set_text(m_commandLabel, "");
        m_feedbackFinished = false;
    }
    if (millis() - m_lastNamesUpdate >= 1000) {
        const DeviceConfig cfg = configManager.get();
        if (strcmp(lv_label_get_text(m_ant1NameLabel), cfg.ant1Name))
            lv_label_set_text(m_ant1NameLabel, cfg.ant1Name);
        if (strcmp(lv_label_get_text(m_ant2NameLabel), cfg.ant2Name))
            lv_label_set_text(m_ant2NameLabel, cfg.ant2Name);
        m_lastNamesUpdate = millis();
    }
    lv_timer_handler();
    delay(5);
}

void DisplayUI::updateMeterScale() {
    if (!m_meterScale) return;
    const auto settings = m_meterScale->snapshot();
    const uint16_t watts = settings.watts;
    if (m_maxPower == watts && m_logarithmic == settings.logarithmic && m_tickLabels[4][0]) return;
    m_faceAttempted = false;
    ++m_faceVersion;
    m_face.clear();
    m_maxPower = watts;
    m_logarithmic = settings.logarithmic;
    m_forwardFraction = ldg::meterFraction(m_fwdPower, m_maxPower, m_logarithmic);
    m_reflectedFraction = ldg::meterFraction(m_refPower, m_maxPower, m_logarithmic);
    m_forwardMotion.snap(m_forwardFraction, millis());
    m_reflectedMotion.snap(m_reflectedFraction, millis());
    for (int i = 0; i <= 4; ++i) {
        const float value = ldg::meterPower(float(i) / 4, watts, m_logarithmic);
        snprintf(m_tickLabels[i], sizeof(m_tickLabels[i]), value > 0 && value < 1 ? "%.2f" :
                 value > 0 && value < 10 ? "%.1f" : "%.0f", double(value));
    }
    lv_label_set_text_fmt(m_scaleLabel, "%uW%s", unsigned(watts), m_logarithmic ? " LOG" : "");
    lv_obj_invalidate(m_meter);
}

void DisplayUI::updateState(const tuner_state_t& state) {
    const auto lastAntenna = ldg::lastKnownAntenna(state);
    const bool unconfirmed = state.antenna == ANT_UNKNOWN && ldg::knownAntenna(lastAntenna);
    if (m_antennaInit != state.antennaInit || m_activeAnt != state.antenna || m_lastAnt != lastAntenna) {
        m_antennaInit = state.antennaInit;
        const char* text = "";
        switch (state.antennaInit) {
            case ldg::AntennaInit::Waiting: text = "ANT: waiting for live power"; break;
            case ldg::AntennaInit::WaitingRfOff: text = "ANT: waiting for RF off"; break;
            case ldg::AntennaInit::Checking: text = "ANT: checking"; break;
            case ldg::AntennaInit::Restoring: text = "ANT: restoring original"; break;
            case ldg::AntennaInit::Ready: text = "ANT: Ready"; break;
            case ldg::AntennaInit::Error: text = "ANT: check failed - select manually"; break;
            default: break;
        }
        if (unconfirmed) lv_label_set_text_fmt(m_antennaInitLabel, "ANT: last %u (unconfirmed)", unsigned(lastAntenna));
        else lv_label_set_text(m_antennaInitLabel, text);
        lv_obj_set_style_text_color(m_antennaInitLabel,
            lv_color_hex(unconfirmed ? 0xfbbf24 : state.antennaInit == ldg::AntennaInit::Error ? 0xff5555 : 0x888888), LV_PART_MAIN);
    }
    tuner_meter_t meter = state.meter;
    if (!state.meterValid) {
        meter = tuner_meter_t{};
        meter.forward_power_watts = meter.reflected_power_watts = NAN;
        meter.swr = NAN;
    }
    meter.band = ldg::lastKnownBand(state);
    m_pendingMeter = meter;
    m_pendingBandHeld = ldg::bandIsHeld(state);
    m_meterPending = true;
    updateStatus(state.mode, state.antenna, lastAntenna);
}

void DisplayUI::setCommandPending(bool pending) {
    if (m_commandPending == pending) return;
    m_commandPending = pending;
    for (lv_obj_t* button : {m_btnMemTune, m_btnFullTune, m_btnAnt1, m_btnAnt2, m_btnBypass, m_btnAuto}) {
        if (pending) lv_obj_add_state(button, LV_STATE_DISABLED);
        else lv_obj_clear_state(button, LV_STATE_DISABLED);
    }
}

void DisplayUI::updateCommandFeedback(const ldg::CommandFeedback::Snapshot& feedback) {
    if (feedback.token == m_feedbackToken) return;
    m_feedbackToken = feedback.token;
    m_feedbackAt = millis();
    const bool pending = feedback.phase == ldg::CommandFeedback::Phase::Pending;
    const bool success = feedback.phase == ldg::CommandFeedback::Phase::Success;
    m_feedbackFinished = !pending;
    lv_obj_set_style_text_color(m_commandLabel,
        lv_color_hex(pending ? 0xaaaaaa : success ? 0x22c55e : 0xef4444), LV_PART_MAIN);
    lv_label_set_text_fmt(m_commandLabel, "%s: %s", ldg::commandLabel(feedback.command),
        pending ? "waiting..." : success ? "OK" : "not confirmed");
}

void DisplayUI::updateMeter(const tuner_meter_t* meter, bool bandHeld) {
    const bool forwardValid = std::isfinite(meter->forward_power_watts) && meter->forward_power_watts >= 0;
    const bool reflectedValid = std::isfinite(meter->reflected_power_watts) && meter->reflected_power_watts >= 0;
    const float forward = forwardValid ? float(meter->forward_power_watts) : 0;
    const float reflected = reflectedValid ? float(meter->reflected_power_watts) : 0;
    const float swr = float(meter->swr);
    auto updatePowerText = [](lv_obj_t* label, const char* prefix, float value, bool valid) {
        char text[32];
        if (valid) snprintf(text, sizeof(text), "%s %.1fW", prefix, double(value));
        else snprintf(text, sizeof(text), "%s --", prefix);
        if (strcmp(lv_label_get_text(label), text)) lv_label_set_text(label, text);
    };
    if (forward != m_fwdPower || forwardValid != m_fwdValid)
        updatePowerText(m_fwdPowerLabel, "FWD", forward, forwardValid);
    if (reflected != m_refPower || reflectedValid != m_refValid)
        updatePowerText(m_refPowerLabel, "REF", reflected, reflectedValid);
    m_fwdPower = forward;
    m_refPower = reflected;
    m_fwdValid = forwardValid;
    m_refValid = reflectedValid;
    const float forwardTarget = ldg::meterFraction(forward, m_maxPower, m_logarithmic);
    const float reflectedTarget = ldg::meterFraction(reflected, m_maxPower, m_logarithmic);
    m_forwardMotion.target(forwardTarget);
    m_reflectedMotion.target(reflectedTarget);
    if (!forwardValid || !m_smoothing) m_forwardMotion.snap(forwardTarget, millis());
    if (!reflectedValid || !m_smoothing) m_reflectedMotion.snap(reflectedTarget, millis());
    char swrText[16];
    if (std::isfinite(swr)) snprintf(swrText, sizeof(swrText), "%.1f:1", double(swr));
    else snprintf(swrText, sizeof(swrText), "--");
    auto swrColor = [](float value) { return !std::isfinite(value) ? 0 : value > 2 ? 3 : value > 1.5f ? 2 : 1; };
    const bool swrChanged = strcmp(swrText, m_swrText) || swrColor(swr) != swrColor(m_swr);
    m_swr = swr;
    if (swrChanged) {
        strcpy(m_swrText, swrText);
        lv_area_t bounds;
        lv_obj_get_coords(m_meter, &bounds);
        const int cx = lv_obj_get_width(m_meter) / 2;
        const int cy = lv_obj_get_height(m_meter) - 20;
        const int y = cy - (cy - 20) / 2;
        lv_area_t text = {lv_coord_t(bounds.x1 + cx - 50), lv_coord_t(bounds.y1 + y - 14),
                          lv_coord_t(bounds.x1 + cx + 50), lv_coord_t(bounds.y1 + y + 14)};
        lv_obj_invalidate_area(m_meter, &text);
    }
    if (meter->band != m_band || bandHeld != m_bandHeld) {
        m_band = meter->band;
        m_bandHeld = bandHeld;
        lv_label_set_text_fmt(m_bandLabel, "Band: %s%s", TunerProtocol::bandToString(m_band),
                              bandHeld ? " (last)" : "");
    }
}

void DisplayUI::animateNeedles(uint32_t now) {
    m_geometry.resize(lv_obj_get_width(m_meter), lv_obj_get_height(m_meter), m_logarithmic);
    const auto oldForward = m_geometry.forwardNeedle(m_forwardFraction);
    const auto oldReflected = m_geometry.reflectedNeedle(m_reflectedFraction);
    m_forwardFraction = m_forwardMotion.advance(now);
    m_reflectedFraction = m_reflectedMotion.advance(now);
    invalidateNeedle(m_geometry.forwardPivot, oldForward, m_geometry.forwardNeedle(m_forwardFraction));
    invalidateNeedle(m_geometry.reflectedPivot, oldReflected, m_geometry.reflectedNeedle(m_reflectedFraction));
}

void DisplayUI::invalidateNeedle(ldg::MeterGeometry::Point pivot,
                                 ldg::MeterGeometry::Point before,
                                 ldg::MeterGeometry::Point after) {
    if (before.x == after.x && before.y == after.y) return;
    lv_area_t bounds;
    lv_obj_get_coords(m_meter, &bounds);
    // Four rectangles per line cover the old/new needle without transferring
    // its whole bounding box. Keep below LVGL's invalid-area capacity; too
    // many tiny rectangles would make it fall back to a full-screen refresh.
    for (const auto tip : {before, after}) for (int part = 0; part < 4; ++part) {
        const int x1 = pivot.x + (tip.x - pivot.x) * part / 4;
        const int y1 = pivot.y + (tip.y - pivot.y) * part / 4;
        const int x2 = pivot.x + (tip.x - pivot.x) * (part + 1) / 4;
        const int y2 = pivot.y + (tip.y - pivot.y) * (part + 1) / 4;
        lv_area_t area = {lv_coord_t(bounds.x1 + std::min(x1, x2) - 4),
                          lv_coord_t(bounds.y1 + std::min(y1, y2) - 4),
                          lv_coord_t(bounds.x1 + std::max(x1, x2) + 4),
                          lv_coord_t(bounds.y1 + std::max(y1, y2) + 4)};
        lv_obj_invalidate_area(m_meter, &area);
    }
}

void DisplayUI::updateNetworkInfo() {
    m_previousValid = false;
    if (m_ssidLabel)
        lv_label_set_text_fmt(m_ssidLabel, "SSID: %s", WiFi.SSID().c_str());
    if (m_ipLabel)
        lv_label_set_text_fmt(m_ipLabel, "IP: %s", WiFi.localIP().toString().c_str());
}

void DisplayUI::updateStatus(tuner_mode_t mode, tuner_ant_t antenna, tuner_ant_t last) {
    if (ldg::knownAntenna(antenna)) last = antenna;
    if (mode != m_activeMode) {
        m_activeMode = mode;
        switch (mode) {
            case MODE_AUTO:    lv_label_set_text(m_modeLabel, "Mode: AUTO"); break;
            case MODE_MANUAL:  lv_label_set_text(m_modeLabel, "Mode: MANUAL"); break;
            case MODE_BYPASS:  lv_label_set_text(m_modeLabel, "Mode: BYPASS"); break;
            default:           lv_label_set_text(m_modeLabel, "Mode: --"); break;
        }
        lv_label_set_text(m_modeButtonLabel, mode == MODE_AUTO ? "SET MANUAL" : "SET AUTO");
        lv_label_set_text(m_bypassButtonLabel, mode == MODE_BYPASS ? "BYPASS OFF" : "BYPASS ON");
    }
    if (antenna != m_activeAnt || last != m_lastAnt) {
        m_activeAnt = antenna;
        m_lastAnt = last;
        updateAntButtons();
    }
}

void DisplayUI::updateAntButtons() {
    const DeviceConfig& cfg = configManager.get();
    lv_label_set_text(m_ant1NameLabel, cfg.ant1Name);
    lv_label_set_text(m_ant2NameLabel, cfg.ant2Name);

    lv_color_t activeColor   = lv_color_hex(0x2255aa);
    lv_color_t inactiveColor = lv_color_hex(0x2a2a3a);
    lv_color_t uncertainColor = lv_color_hex(0x806020);
    lv_obj_set_style_bg_color(m_btnAnt1,
        m_activeAnt == ANT_A ? activeColor : m_activeAnt == ANT_UNKNOWN && m_lastAnt == ANT_A ? uncertainColor : inactiveColor, LV_PART_MAIN);
    lv_obj_set_style_bg_color(m_btnAnt2,
        m_activeAnt == ANT_B ? activeColor : m_activeAnt == ANT_UNKNOWN && m_lastAnt == ANT_B ? uncertainColor : inactiveColor, LV_PART_MAIN);
}

void DisplayUI::setupTouch() {
#ifdef WITH_TOUCH
#ifdef TOUCH_GT911
    pinMode(TOUCH_RST, OUTPUT);
    digitalWrite(TOUCH_RST, LOW);
    delay(10);
    digitalWrite(TOUCH_RST, HIGH);
    delay(100);
    // INT is grounded on some stock boards. Reset without driving INT,
    // probe both addresses, and use bounded polling instead of interrupts.
    Wire.setTimeOut(10);
    const bool busReady = Wire.begin(TOUCH_SDA, TOUCH_SCL, 100000);
    if (busReady && touch.begin()) {
        Serial.printf("Touch: GT911 ready at 0x%02x\n", touch.address());
    } else {
        Serial.println("Touch: GT911 unavailable; retrying during polling");
    }
#else
    Wire.begin(TOUCH_SDA, TOUCH_SCL);
#endif
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = [](lv_indev_drv_t* drv, lv_indev_data_t* data) {
        if (s_instance) s_instance->readTouch(drv, data);
    };
    lv_indev_drv_register(&indev_drv);
#endif
}

void DisplayUI::readTouch(lv_indev_drv_t* drv, lv_indev_data_t* data) {
#ifdef TOUCH_GT911
    const auto point = touch.poll(millis());
    data->point.x = point.x;
    data->point.y = point.y;
    data->state = point.pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
#else
    if (tsTouched()) {
        uint16_t x, y;
        if (ns2009Read(&x, &y)) {
            data->state = LV_INDEV_STATE_PR;
            data->point.x = map(x, 200, 3900, 0, DISPLAY_WIDTH);
            data->point.y = map(y, 200, 3900, 0, DISPLAY_HEIGHT);
        } else {
            data->state = LV_INDEV_STATE_REL;
        }
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
#endif
}

void DisplayUI::drawMeterEvent(lv_event_t* e) {
    if (s_instance) s_instance->drawCrossNeedle(e);
}

void DisplayUI::drawMeterFace(lv_draw_ctx_t* draw_ctx, const lv_area_t& bounds) {
    const int ox = bounds.x1, oy = bounds.y1;
    const int arm = m_geometry.radius;
    const int fwdPx = m_geometry.forwardPivot.x + ox, fwdPy = m_geometry.forwardPivot.y + oy;
    const int refPx = m_geometry.reflectedPivot.x + ox, refPy = m_geometry.reflectedPivot.y + oy;
    // ----- Scale arcs (one per pivot, each spanning the full 90° sweep) -----
    // Forward arc: upper-left quarter around fwd pivot (LVGL 180° → 270°, clockwise = left→up)
    // Reflected arc: upper-right quarter around ref pivot (LVGL 270° → 360°, clockwise = up→right)
    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);
    arc_dsc.width = 2;
    arc_dsc.color = lv_color_hex(0x22c55e);
    lv_point_t fwd_ctr = {(lv_coord_t)fwdPx, (lv_coord_t)fwdPy};
    lv_draw_arc(draw_ctx, &arc_dsc, &fwd_ctr, arm, 180, 270);
    arc_dsc.color = lv_color_hex(0xef4444);
    lv_point_t ref_ctr = {(lv_coord_t)refPx, (lv_coord_t)refPy};
    lv_draw_arc(draw_ctx, &arc_dsc, &ref_ctr, arm, 270, 360);

    // ----- Tick marks + labels along each arc -----
    const auto visible = [draw_ctx](int x1, int y1, int x2, int y2, int padding) {
        const auto* clip = draw_ctx->clip_area;
        return std::max(x1, x2) + padding >= clip->x1 && std::min(x1, x2) - padding <= clip->x2 &&
               std::max(y1, y2) + padding >= clip->y1 && std::min(y1, y2) - padding <= clip->y2;
    };
    for (int side = 0; side < 2; ++side) {
        const uint32_t color = side == 0 ? 0x22c55e : 0xef4444;
        for (int i = 0; i <= 8; ++i) {
            const auto& tick = m_geometry.ticks[side][i];
            const bool major = i % 2 == 0;
            lv_point_t p1 = {lv_coord_t(tick.line.from.x + ox), lv_coord_t(tick.line.from.y + oy)};
            lv_point_t p2 = {lv_coord_t(tick.line.to.x + ox), lv_coord_t(tick.line.to.y + oy)};
            lv_draw_line_dsc_t tick_dsc;
            lv_draw_line_dsc_init(&tick_dsc);
            tick_dsc.color = major ? lv_color_hex(color) : lv_color_hex(0x666666);
            tick_dsc.width = major ? 2 : 1;
            if (visible(p1.x, p1.y, p2.x, p2.y, 3)) lv_draw_line(draw_ctx, &tick_dsc, &p1, &p2);
            if (major && i > 0) {
                lv_draw_label_dsc_t ld;
                lv_draw_label_dsc_init(&ld);
                ld.color = lv_color_hex(color);
                ld.font  = &lv_font_montserrat_14;
                ld.align = LV_TEXT_ALIGN_CENTER;
                const int lx = tick.label.x + ox, ly = tick.label.y + oy;
                lv_area_t la = {(lv_coord_t)(lx-16),(lv_coord_t)(ly-8),
                                (lv_coord_t)(lx+16),(lv_coord_t)(ly+8)};
                if (visible(la.x1, la.y1, la.x2, la.y2, 0))
                    lv_draw_label(draw_ctx, &ld, &la, m_tickLabels[i / 2], NULL);
            }
        }
    }

    // ----- SWR iso-curves -----
    static const uint32_t swrColors[] = {0x84cc16, 0xeab308, 0xf97316, 0xef4444, 0xa855f7};
    lv_draw_line_dsc_t curve_dsc;
    lv_draw_line_dsc_init(&curve_dsc);
    curve_dsc.width = 1;
    curve_dsc.opa   = LV_OPA_60;
    for (int si = 0; si < 5; si++) {
        curve_dsc.color = lv_color_hex(swrColors[si]);
        const auto& curve = m_geometry.curves[si];
        for (int i = 0; i < curve.count; ++i) {
            const auto& line = curve.lines[i];
            lv_point_t p1 = {lv_coord_t(line.from.x + ox), lv_coord_t(line.from.y + oy)};
            lv_point_t p2 = {lv_coord_t(line.to.x + ox), lv_coord_t(line.to.y + oy)};
            if (visible(p1.x, p1.y, p2.x, p2.y, 2)) lv_draw_line(draw_ctx, &curve_dsc, &p1, &p2);
        }
    }

}

bool DisplayUI::buildMeterFace(const lv_area_t& bounds) {
    const int width = lv_area_get_width(&bounds), height = lv_area_get_height(&bounds);
    const auto background = lv_obj_get_style_bg_color(m_meter, LV_PART_MAIN);
    if (!m_face.count(width, height, background.full)) return false;
    auto* scratch = static_cast<lv_color_t*>(std::malloc(width * 8 * sizeof(lv_color_t)));
    if (!scratch) return false;
    lv_draw_sw_ctx_t software;
    lv_draw_sw_init_ctx(nullptr, &software.base_draw);
    auto* context = &software.base_draw;
    bool success = true;
    for (int pass = 0; pass < 2 && success; ++pass) {
        for (int y = 0; y < height && success; y += 8) {
            const int rows = std::min(8, height - y);
            std::fill(scratch, scratch + width * rows, background);
            lv_area_t stripe = {bounds.x1, lv_coord_t(bounds.y1 + y), bounds.x2, lv_coord_t(bounds.y1 + y + rows - 1)};
            context->buf = scratch;
            context->buf_area = &stripe;
            context->clip_area = &stripe;
            drawMeterFace(context, bounds);
            for (int row = 0; row < rows && success; ++row)
                success = m_face.append(reinterpret_cast<const uint16_t*>(scratch + row * width));
        }
        if (success) success = pass == 0 ? m_face.allocate() : m_face.finish();
    }
    lv_draw_sw_deinit_ctx(nullptr, context);
    std::free(scratch);
    if (!success) m_face.clear();
    return success;
}

// Direct antialiased RGB565 stroke: visit only rows crossing a 3-pixel
// needle, rather than constructing LVGL line masks again for every strip.
// The endpoint/pivot circles are still rendered by LVGL below.
static void rasterNeedle(lv_draw_ctx_t* context, lv_point_t pivot, lv_point_t tip, lv_color_t color) {
    const float dx = tip.x - pivot.x, dy = tip.y - pivot.y;
    const float squared = dx * dx + dy * dy;
    if (squared < 1) return;
    const float length = sqrtf(squared), inverse = 1.0f / length;
    const auto& clip = *context->clip_area;
    const int top = std::max<int>(clip.y1,std::min(pivot.y,tip.y)-2);
    const int bottom = std::min<int>(clip.y2,std::max(pivot.y,tip.y)+2);
    const int minimumX = std::max<int>(clip.x1,std::min(pivot.x,tip.x)-2);
    const int maximumX = std::min<int>(clip.x2,std::max(pivot.x,tip.x)+2);
    const float slope = dy != 0 ? dx / dy : 0, half = dy != 0 ? 2 * length / fabsf(dy) : 0;
    const int stride = lv_area_get_width(context->buf_area);
    auto* buffer = static_cast<lv_color_t*>(context->buf);
    for (int y=top;y<=bottom;++y) {
        int left=minimumX,right=maximumX;
        if (dy != 0) {
            const float center=pivot.x+slope*(y-pivot.y);
            left=std::max(left,int(floorf(center-half)));
            right=std::min(right,int(ceilf(center+half)));
        }
        auto* row=buffer+(y-context->buf_area->y1)*stride;
        for(int x=left;x<=right;++x) {
            const float vx=x-pivot.x,vy=y-pivot.y;
            const float dot=vx*dx+vy*dy;
            float distance;
            if(dot<0)distance=sqrtf(vx*vx+vy*vy);
            else if(dot>squared) {
                const float ex=x-tip.x,ey=y-tip.y;distance=sqrtf(ex*ex+ey*ey);
            } else distance=fabsf(vx*dy-vy*dx)*inverse;
            if(distance>=2)continue;
            auto& pixel=row[x-context->buf_area->x1];
            if(distance<=1)pixel=color;
            else pixel=lv_color_mix(color,pixel,lv_opa_t((2-distance)*255+0.5f));
        }
    }
}

void DisplayUI::drawMeterForeground(lv_draw_ctx_t* draw_ctx, const lv_area_t& bounds,
                                    float forward, float reflected, float swr, const char* swrText) {
    const int ox = bounds.x1, oy = bounds.y1;
    const int cx = m_geometry.centerX + ox, cy = m_geometry.centerY + oy, arm = m_geometry.radius;
    const int fwdPx = m_geometry.forwardPivot.x + ox, fwdPy = m_geometry.forwardPivot.y + oy;
    const int refPx = m_geometry.reflectedPivot.x + ox, refPy = m_geometry.reflectedPivot.y + oy;
    const auto f = m_geometry.forwardNeedle(forward), r = m_geometry.reflectedNeedle(reflected);
    const lv_point_t fwdTip = {lv_coord_t(f.x + ox),lv_coord_t(f.y + oy)};
    const lv_point_t refTip = {lv_coord_t(r.x + ox),lv_coord_t(r.y + oy)};
    // ----- Needles -----
    lv_draw_line_dsc_t needle_dsc;
    lv_draw_line_dsc_init(&needle_dsc);
    needle_dsc.width     = 3;
    needle_dsc.round_end = true;
    if (m_fastNeedles) {
        rasterNeedle(draw_ctx,{lv_coord_t(fwdPx),lv_coord_t(fwdPy)},fwdTip,lv_color_hex(0x00ff00));
        rasterNeedle(draw_ctx,{lv_coord_t(refPx),lv_coord_t(refPy)},refTip,lv_color_hex(0xff0000));
    } else {
    needle_dsc.color = lv_color_hex(0x00ff00);
    { lv_point_t p1={(lv_coord_t)fwdPx,(lv_coord_t)fwdPy},
                 p2={fwdTip.x, fwdTip.y};
      lv_draw_line(draw_ctx, &needle_dsc, &p1, &p2); }
    needle_dsc.color = lv_color_hex(0xff0000);
    { lv_point_t p1={(lv_coord_t)refPx,(lv_coord_t)refPy},
                 p2={refTip.x, refTip.y};
      lv_draw_line(draw_ctx, &needle_dsc, &p1, &p2); }
    }

    // ----- Pivot dots -----
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.radius = LV_RADIUS_CIRCLE;
    rd.bg_color = lv_color_hex(0x00aa00);
    lv_area_t fa={(lv_coord_t)(fwdPx-5),(lv_coord_t)(fwdPy-5),
                  (lv_coord_t)(fwdPx+5),(lv_coord_t)(fwdPy+5)};
    lv_draw_rect(draw_ctx, &rd, &fa);
    rd.bg_color = lv_color_hex(0xaa0000);
    lv_area_t ra={(lv_coord_t)(refPx-5),(lv_coord_t)(refPy-5),
                  (lv_coord_t)(refPx+5),(lv_coord_t)(refPy+5)};
    lv_draw_rect(draw_ctx, &rd, &ra);

    // ----- SWR text -----
    lv_draw_label_dsc_t swr_dsc;
    lv_draw_label_dsc_init(&swr_dsc);
    swr_dsc.color = !std::isfinite(swr) ? lv_color_hex(0x888888) : (swr > 2.0f) ? lv_color_hex(0xff4444) :
                    (swr > 1.5f) ? lv_color_hex(0xffaa00) : lv_color_hex(0x00ff00);
    swr_dsc.font  = &lv_font_montserrat_24;
    swr_dsc.align = LV_TEXT_ALIGN_CENTER;
    lv_area_t swr_a = {(lv_coord_t)(cx-50),(lv_coord_t)(cy-arm/2-12),
                       (lv_coord_t)(cx+50),(lv_coord_t)(cy-arm/2+12)};
    lv_draw_label(draw_ctx, &swr_dsc, &swr_a, swrText, NULL);
}

void DisplayUI::drawCrossNeedle(lv_event_t* e) {
    const uint32_t drawStarted = micros();
    lv_draw_ctx_t* draw_ctx = lv_event_get_draw_ctx(e);
    lv_obj_t* obj = lv_event_get_target(e);
    m_geometry.resize(lv_obj_get_width(obj), lv_obj_get_height(obj), m_logarithmic);
    lv_area_t bounds;
    lv_obj_get_coords(obj, &bounds);
    if (memcmp(&bounds, &m_faceBounds, sizeof(bounds))) {
        m_faceBounds = bounds; m_faceAttempted = false; ++m_faceVersion; m_face.clear();
    }
    const int ox = bounds.x1, oy = bounds.y1;
    const int cx = m_geometry.centerX + ox, cy = m_geometry.centerY + oy;
    const int arm = m_geometry.radius;
    const int fwdPx = m_geometry.forwardPivot.x + ox, fwdPy = m_geometry.forwardPivot.y + oy;
    const int refPx = m_geometry.reflectedPivot.x + ox, refPy = m_geometry.reflectedPivot.y + oy;
    const auto localFwd = m_geometry.forwardNeedle(m_forwardFraction);
    const auto localRef = m_geometry.reflectedNeedle(m_reflectedFraction);
    const lv_point_t fwdTip = {lv_coord_t(localFwd.x + ox), lv_coord_t(localFwd.y + oy)};
    const lv_point_t refTip = {lv_coord_t(localRef.x + ox), lv_coord_t(localRef.y + oy)};

    if (m_faceAllowed && !m_faceAttempted) {
        m_faceAttempted = true;
        const uint32_t started = micros();
        const bool built = buildMeterFace(bounds);
        portENTER_CRITICAL(&m_diagnosticsMux);
        ++m_diagnostics.faceBuilds;
        m_diagnostics.faceBuildUs = micros() - started;
        m_diagnostics.faceBytes = built ? m_face.bytes() : 0;
        portEXIT_CRITICAL(&m_diagnosticsMux);
    }
    if (m_faceAllowed && m_face.valid()) {
        lv_area_t clip;
        if (_lv_area_intersect(&clip, &bounds, draw_ctx->clip_area)) {
            const int stride = lv_area_get_width(draw_ctx->buf_area);
            auto* destination = static_cast<uint16_t*>(draw_ctx->buf);
            for (int y = clip.y1; y <= clip.y2; ++y) {
                auto* row = destination + (y - draw_ctx->buf_area->y1) * stride + clip.x1 - draw_ctx->buf_area->x1;
                m_face.copyRow(row, y - oy, clip.x1 - ox, clip.x2 - ox);
            }
        }
        portENTER_CRITICAL(&m_diagnosticsMux);
        ++m_diagnostics.faceHits;
        portEXIT_CRITICAL(&m_diagnosticsMux);
    } else {
        drawMeterFace(draw_ctx, bounds);
        portENTER_CRITICAL(&m_diagnosticsMux);
        ++m_diagnostics.vectorDraws;
        portEXIT_CRITICAL(&m_diagnosticsMux);
    }

    drawMeterForeground(draw_ctx, bounds, m_forwardFraction, m_reflectedFraction, m_swr, m_swrText);
    m_frameDrawUs += micros() - drawStarted;
}

void DisplayUI::displayFlush(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p) {
    auto* self = s_instance;
    const uint32_t started = micros();
    const int width = lv_area_get_width(area), height = lv_area_get_height(area);
    lv_area_t graph = self->m_faceBounds, difference;
    // Reconstruct the previous dial including its two overlaid power labels,
    // so unchanged glyphs and empty label backgrounds need no SPI transfer.
    const bool delta = self->m_deltaAllowed && self->m_deltaScratch && self->m_faceAllowed &&
        self->m_face.valid() && self->m_previousValid && self->m_previousFaceVersion == self->m_faceVersion &&
        _lv_area_intersect(&difference, area, &graph);
    if (delta) {
        const uint32_t drawStarted = micros();
        const int stride = lv_area_get_width(&difference);
        for (int y = difference.y1; y <= difference.y2; ++y)
            self->m_face.copyRow(reinterpret_cast<uint16_t*>(self->m_deltaScratch + (y - difference.y1) * stride),
                y - graph.y1, difference.x1 - graph.x1, difference.x2 - graph.x1);
        lv_draw_sw_ctx_t software;
        lv_draw_sw_init_ctx(disp, &software.base_draw);
        software.base_draw.buf = self->m_deltaScratch;
        software.base_draw.buf_area = &difference;
        software.base_draw.clip_area = &difference;
        self->drawMeterForeground(&software.base_draw, self->m_faceBounds,
            self->m_previousForward, self->m_previousReflected, self->m_previousSwr, self->m_previousSwrText);
        const auto oldLabel = [&software](lv_obj_t* label, const char* text) {
            lv_draw_label_dsc_t dsc;
            lv_draw_label_dsc_init(&dsc);
            lv_obj_init_draw_label_dsc(label, LV_PART_MAIN, &dsc);
            lv_area_t bounds;
            lv_obj_get_content_coords(label, &bounds);
            lv_draw_label(&software.base_draw, &dsc, &bounds, text, nullptr);
        };
        oldLabel(self->m_fwdPowerLabel,self->m_previousForwardText);
        oldLabel(self->m_refPowerLabel,self->m_previousReflectedText);
        lv_draw_sw_deinit_ctx(disp, &software.base_draw);
        self->m_frameDrawUs += micros() - drawStarted;
        portENTER_CRITICAL(&self->m_diagnosticsMux);
        ++self->m_diagnostics.deltaFlushes;
        portEXIT_CRITICAL(&self->m_diagnosticsMux);
    }
    self->m_tft->startWrite();
    const auto send = [self](int x, int y, int w, int h, lv_color_t* source) {
        if (w <= 0 || h <= 0) return;
        self->m_tft->setAddrWindow(x, y, w, h);
        self->m_tft->pushColors(reinterpret_cast<uint16_t*>(source), w * h, true);
        self->m_frameSentPixels += w * h;
    };
    if (!delta) send(area->x1, area->y1, width, height, color_p);
    else {
        const int length=lv_area_get_width(&difference);
        // Batch changed pixels into short rectangles. One SPI address window
        // per tiny run on every row would cost more than the saved pixel data.
        for(int top=difference.y1;top<=difference.y2;top+=4) {
            const int rows=std::min(4,int(difference.y2)-top+1);
            bool dirty[DISPLAY_WIDTH] = {};
            for(int row=0;row<rows;++row) {
                const auto* before=self->m_deltaScratch+(top+row-difference.y1)*length;
                const auto* after=color_p+(top+row-area->y1)*width+difference.x1-area->x1;
                for(int x=0;x<length;++x)dirty[x]|=before[x].full!=after[x].full;
            }
            auto* packed=self->m_deltaScratch+(top-difference.y1)*length;
            for(int x=0;x<length;) {
                while(x<length && !dirty[x])++x;
                if(x==length)break;
                const int first=x++;
                int unchanged=0;
                while(x<length) {
                    unchanged=dirty[x]?0:unchanged+1;++x;
                    if(unchanged==4){x-=4;break;}
                }
                const int columns=x-first;
                for(int row=0;row<rows;++row)
                    memcpy(packed+row*columns,color_p+(top+row-area->y1)*width+difference.x1-area->x1+first,
                        columns*sizeof(lv_color_t));
                send(difference.x1+first,top,columns,rows,packed);
            }
        }
        // Uncached siblings/outer areas retain the normal LVGL pixels.
        send(area->x1,area->y1,width,difference.y1-area->y1,color_p);
        send(area->x1,difference.y2+1,width,area->y2-difference.y2,
            color_p+(difference.y2+1-area->y1)*width);
        const auto side=[&](int x,int columns) {
            if(columns<=0)return;
            const int rows=lv_area_get_height(&difference);
            for(int row=0;row<rows;++row)
                memcpy(self->m_deltaScratch+row*columns,
                    color_p+(difference.y1+row-area->y1)*width+x-area->x1,columns*sizeof(lv_color_t));
            send(x,difference.y1,columns,rows,self->m_deltaScratch);
        };
        side(area->x1,difference.x1-area->x1);
        side(difference.x2+1,area->x2-difference.x2);
    }
    self->m_tft->endWrite();
    self->m_frameFlushUs += micros() - started;
    lv_disp_flush_ready(disp);
}

void DisplayUI::displayMonitor(lv_disp_drv_t*, uint32_t timeMs, uint32_t pixels) {
    auto* self = s_instance;
    portENTER_CRITICAL(&self->m_diagnosticsMux);
    auto& d = self->m_diagnostics;
    ++d.frames; d.frameMs = timeMs; d.maxFrameMs = std::max(d.maxFrameMs, timeMs);
    d.pixels = pixels; d.flushUs = self->m_frameFlushUs; d.drawUs = self->m_frameDrawUs;
    d.sentPixels = self->m_frameSentPixels;
    portEXIT_CRITICAL(&self->m_diagnosticsMux);
    self->m_frameFlushUs = self->m_frameDrawUs = self->m_frameSentPixels = 0;
    self->m_previousValid = self->m_face.valid();
    self->m_previousFaceVersion = self->m_faceVersion;
    self->m_previousForward = self->m_forwardFraction; self->m_previousReflected = self->m_reflectedFraction;
    self->m_previousSwr = self->m_swr;
    strcpy(self->m_previousSwrText,self->m_swrText);
    snprintf(self->m_previousForwardText,sizeof(self->m_previousForwardText),"%s",lv_label_get_text(self->m_fwdPowerLabel));
    snprintf(self->m_previousReflectedText,sizeof(self->m_previousReflectedText),"%s",lv_label_get_text(self->m_refPowerLabel));
}

void DisplayUI::writeDiagnostics(JsonObject doc) const {
    portENTER_CRITICAL(&m_diagnosticsMux);
    const auto d = m_diagnostics;
    portEXIT_CRITICAL(&m_diagnosticsMux);
    doc["frames"] = d.frames; doc["frame_ms"] = d.frameMs; doc["max_frame_ms"] = d.maxFrameMs;
    doc["pixels"] = d.pixels; doc["flush_us"] = d.flushUs; doc["draw_us"] = d.drawUs;
    doc["face_bytes"] = d.faceBytes; doc["face_build_us"] = d.faceBuildUs;
    doc["sent_pixels"] = d.sentPixels; doc["delta_flushes"] = d.deltaFlushes;
    doc["face_builds"] = d.faceBuilds; doc["cache_hits"] = d.faceHits; doc["vector_draws"] = d.vectorDraws;
#ifdef ARDUINO_ARCH_ESP32
    doc["spi_write_hz"] = SPI_FREQUENCY;
    doc["free_heap"] = ESP.getFreeHeap();
    doc["largest_block"] = ESP.getMaxAllocHeap();
#endif
}

void DisplayUI::btnMemTuneEvent(lv_event_t* e) {
    if (s_instance && s_instance->m_commandHandler) s_instance->m_commandHandler(TUNER_CMD_MEM_TUNE);
}

void DisplayUI::btnFullTuneEvent(lv_event_t* e) {
    if (s_instance && s_instance->m_commandHandler) s_instance->m_commandHandler(TUNER_CMD_FULL_TUNE);
}

void DisplayUI::btnAnt1Event(lv_event_t* e) {
    if (s_instance && s_instance->m_commandHandler)
        s_instance->m_commandHandler(TUNER_CMD_SELECT_ANT_A);
}

void DisplayUI::btnAnt2Event(lv_event_t* e) {
    if (s_instance && s_instance->m_commandHandler)
        s_instance->m_commandHandler(TUNER_CMD_SELECT_ANT_B);
}

void DisplayUI::btnBypassEvent(lv_event_t* e) {
    if (s_instance && s_instance->m_commandHandler)
        s_instance->m_commandHandler(s_instance->m_activeMode == MODE_BYPASS
            ? TUNER_CMD_AUTO_MODE : TUNER_CMD_BYPASS);
}

void DisplayUI::btnAutoEvent(lv_event_t* e) {
    if (s_instance && s_instance->m_commandHandler)
        s_instance->m_commandHandler(s_instance->m_activeMode == MODE_AUTO
            ? TUNER_CMD_MANUAL_MODE : TUNER_CMD_AUTO_MODE);
}

void DisplayUI::meterTapEvent(lv_event_t* e) {
    if (!s_instance || !s_instance->m_meterScale) return;
    s_instance->m_meterScale->toggle();
    s_instance->updateMeterScale();
}

#endif // WITH_DISPLAY
