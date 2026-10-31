#ifdef WITH_DISPLAY

#include "display_ui.h"
#include "config_manager.h"
#include <WiFi.h>
#include <Wire.h>
#include <math.h>
#include <algorithm>

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
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

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

#ifdef BOARD_ESP32_3248S035C
    lv_refr_now(NULL);
    digitalWrite(TFT_BL, HIGH);
    Serial.println("Display: Sunton ST7796 480x320 ready");
#endif
    return true;
}

void DisplayUI::loop() {
    // Web requests only change the atomic model; all LVGL calls stay here.
    updateMeterScale();
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
    const uint16_t watts = m_meterScale->snapshot().watts;
    if (m_maxPower == watts) return;
    m_maxPower = watts;
    lv_label_set_text_fmt(m_scaleLabel, "%uW", unsigned(watts));
    lv_obj_invalidate(m_meter);
}

void DisplayUI::updateState(const tuner_state_t& state) {
    tuner_meter_t meter = state.meter;
    if (!state.meterValid) {
        meter = tuner_meter_t{};
        meter.forward_power_watts = meter.reflected_power_watts = NAN;
        meter.swr = NAN;
    }
    meter.band = ldg::lastKnownBand(state);
    updateMeter(&meter, ldg::bandIsHeld(state));
    updateStatus(state.mode, state.antenna);
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
    m_fwdValid = forwardValid;
    m_refValid = reflectedValid;
    if (forward != m_fwdPower || reflected != m_refPower) {
        m_geometry.resize(lv_obj_get_width(m_meter), lv_obj_get_height(m_meter));
        const auto oldForward = m_geometry.forwardNeedle(fminf(m_fwdPower / m_maxPower, 1.0f));
        const auto oldReflected = m_geometry.reflectedNeedle(fminf(m_refPower / m_maxPower, 1.0f));
        m_fwdPower = forward;
        m_refPower = reflected;
        invalidateNeedle(m_geometry.forwardPivot, oldForward,
            m_geometry.forwardNeedle(fminf(m_fwdPower / m_maxPower, 1.0f)));
        invalidateNeedle(m_geometry.reflectedPivot, oldReflected,
            m_geometry.reflectedNeedle(fminf(m_refPower / m_maxPower, 1.0f)));
    }
    if (swr != m_swr && !(std::isnan(swr) && std::isnan(m_swr))) {
        m_swr = swr;
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
    if (m_ssidLabel)
        lv_label_set_text_fmt(m_ssidLabel, "SSID: %s", WiFi.SSID().c_str());
    if (m_ipLabel)
        lv_label_set_text_fmt(m_ipLabel, "IP: %s", WiFi.localIP().toString().c_str());
}

void DisplayUI::updateStatus(tuner_mode_t mode, tuner_ant_t antenna) {
    if (mode != m_activeMode) {
        m_activeMode = mode;
        switch (mode) {
            case MODE_AUTO:    lv_label_set_text(m_modeLabel, "Mode: AUTO"); break;
            case MODE_MANUAL:  lv_label_set_text(m_modeLabel, "Mode: MANUAL"); break;
            case MODE_BYPASS:  lv_label_set_text(m_modeLabel, "Mode: BYPASS"); break;
            default:           lv_label_set_text(m_modeLabel, "Mode: --"); break;
        }
        lv_label_set_text(m_modeButtonLabel, mode == MODE_AUTO ? "SET MANUAL" : "SET AUTO");
    }
    if (antenna != m_activeAnt) {
        m_activeAnt = antenna;
        updateAntButtons();
    }
}

void DisplayUI::updateAntButtons() {
    const DeviceConfig& cfg = configManager.get();
    lv_label_set_text(m_ant1NameLabel, cfg.ant1Name);
    lv_label_set_text(m_ant2NameLabel, cfg.ant2Name);

    lv_color_t activeColor   = lv_color_hex(0x2255aa);
    lv_color_t inactiveColor = lv_color_hex(0x2a2a3a);
    lv_obj_set_style_bg_color(m_btnAnt1,
        m_activeAnt == ANT_A ? activeColor : inactiveColor, LV_PART_MAIN);
    lv_obj_set_style_bg_color(m_btnAnt2,
        m_activeAnt == ANT_B ? activeColor : inactiveColor, LV_PART_MAIN);
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

void DisplayUI::drawCrossNeedle(lv_event_t* e) {
    lv_draw_ctx_t* draw_ctx = lv_event_get_draw_ctx(e);
    lv_obj_t* obj = lv_event_get_target(e);
    m_geometry.resize(lv_obj_get_width(obj), lv_obj_get_height(obj));
    lv_area_t bounds;
    lv_obj_get_coords(obj, &bounds);
    const int ox = bounds.x1, oy = bounds.y1;
    const int cx = m_geometry.centerX + ox, cy = m_geometry.centerY + oy;
    const int arm = m_geometry.radius;
    const int fwdPx = m_geometry.forwardPivot.x + ox, fwdPy = m_geometry.forwardPivot.y + oy;
    const int refPx = m_geometry.reflectedPivot.x + ox, refPy = m_geometry.reflectedPivot.y + oy;
    const auto localFwd = m_geometry.forwardNeedle(fminf(m_fwdPower / m_maxPower, 1.0f));
    const auto localRef = m_geometry.reflectedNeedle(fminf(m_refPower / m_maxPower, 1.0f));
    const lv_point_t fwdTip = {lv_coord_t(localFwd.x + ox), lv_coord_t(localFwd.y + oy)};
    const lv_point_t refTip = {lv_coord_t(localRef.x + ox), lv_coord_t(localRef.y + oy)};

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
    static const char* const highLabels[] = {"", "250", "500", "750", "1000"};
    static const char* const lowLabels[] = {"", "25", "50", "75", "100"};
    const char* const* labels = m_maxPower == 100 ? lowLabels : highLabels;
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
            lv_draw_line(draw_ctx, &tick_dsc, &p1, &p2);
            if (major && i > 0) {
                lv_draw_label_dsc_t ld;
                lv_draw_label_dsc_init(&ld);
                ld.color = lv_color_hex(color);
                ld.font  = &lv_font_montserrat_14;
                ld.align = LV_TEXT_ALIGN_CENTER;
                const int lx = tick.label.x + ox, ly = tick.label.y + oy;
                lv_area_t la = {(lv_coord_t)(lx-16),(lv_coord_t)(ly-8),
                                (lv_coord_t)(lx+16),(lv_coord_t)(ly+8)};
                lv_draw_label(draw_ctx, &ld, &la, labels[i / 2], NULL);
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
            lv_draw_line(draw_ctx, &curve_dsc, &p1, &p2);
        }
    }

    // ----- Needles -----
    lv_draw_line_dsc_t needle_dsc;
    lv_draw_line_dsc_init(&needle_dsc);
    needle_dsc.width     = 3;
    needle_dsc.round_end = true;
    needle_dsc.color = lv_color_hex(0x00ff00);
    { lv_point_t p1={(lv_coord_t)fwdPx,(lv_coord_t)fwdPy},
                 p2={fwdTip.x, fwdTip.y};
      lv_draw_line(draw_ctx, &needle_dsc, &p1, &p2); }
    needle_dsc.color = lv_color_hex(0xff0000);
    { lv_point_t p1={(lv_coord_t)refPx,(lv_coord_t)refPy},
                 p2={refTip.x, refTip.y};
      lv_draw_line(draw_ctx, &needle_dsc, &p1, &p2); }

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
    if (m_swrTextValue != m_swr) {
        m_swrTextValue = m_swr;
        if (std::isfinite(m_swr)) snprintf(m_swrText, sizeof(m_swrText), "%.1f:1", m_swr);
        else snprintf(m_swrText, sizeof(m_swrText), "--");
    }
    lv_draw_label_dsc_t swr_dsc;
    lv_draw_label_dsc_init(&swr_dsc);
    swr_dsc.color = !std::isfinite(m_swr) ? lv_color_hex(0x888888) : (m_swr > 2.0f) ? lv_color_hex(0xff4444) :
                    (m_swr > 1.5f) ? lv_color_hex(0xffaa00) : lv_color_hex(0x00ff00);
    swr_dsc.font  = &lv_font_montserrat_24;
    swr_dsc.align = LV_TEXT_ALIGN_CENTER;
    lv_area_t swr_a = {(lv_coord_t)(cx-50),(lv_coord_t)(cy-arm/2-12),
                       (lv_coord_t)(cx+50),(lv_coord_t)(cy-arm/2+12)};
    lv_draw_label(draw_ctx, &swr_dsc, &swr_a, m_swrText, NULL);
}

void DisplayUI::displayFlush(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;

    s_instance->m_tft->startWrite();
    s_instance->m_tft->setAddrWindow(area->x1, area->y1, w, h);
    s_instance->m_tft->pushColors((uint16_t*)&color_p->full, w * h, true);
    s_instance->m_tft->endWrite();

    lv_disp_flush_ready(disp);
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
    if (s_instance && s_instance->m_commandHandler) s_instance->m_commandHandler(TUNER_CMD_BYPASS);
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
