#ifdef WITH_DISPLAY

#include "display_ui.h"
#include "config_manager.h"
#include <WiFi.h>
#include <Wire.h>
#include <math.h>

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
    : m_tft(nullptr), m_tuner(nullptr), m_meterScale(nullptr),
      m_scr(nullptr), m_meter(nullptr),
      m_bandLabel(nullptr), m_scaleLabel(nullptr),
      m_modeLabel(nullptr),
      m_ssidLabel(nullptr), m_ipLabel(nullptr),
      m_btnMemTune(nullptr), m_btnFullTune(nullptr),
      m_btnAnt1(nullptr), m_btnAnt2(nullptr),
      m_ant1NameLabel(nullptr), m_ant2NameLabel(nullptr),
      m_btnBypass(nullptr), m_btnAuto(nullptr),
      m_maxPower(1000.0f),
      m_fwdPower(0), m_refPower(0), m_swr(1.0f),
      m_activeAnt(ANT_UNKNOWN) {
    s_instance = this;
}

DisplayUI::~DisplayUI() {
    if (m_tft) delete m_tft;
    s_instance = nullptr;
}

bool DisplayUI::begin(TunerProtocol* tuner, ldg::MeterScale* meterScale) {
    m_tuner = tuner;
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
    lv_scr_load(m_scr);

    // ============================================================
    // LEFT PANEL: Cross-needle meter (~300x300)
    // ============================================================
    m_meter = lv_obj_create(m_scr);
    lv_obj_set_size(m_meter, 300, 300);
    lv_obj_align(m_meter, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(m_meter, lv_color_hex(0x111122), LV_PART_MAIN);
    lv_obj_set_style_border_width(m_meter, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(m_meter, 0, LV_PART_MAIN);
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

    makeBtn(&m_btnBypass, "BYPASS",    btnX, antY + antH + btnGap, btnW, btnH,
            lv_color_hex(0xcc8800), lv_color_hex(0xffaa00));
    makeBtn(&m_btnAuto,   "AUTO MODE", btnX, antY + antH + btnGap + btnH + btnGap, btnW, btnH,
            lv_color_hex(0x2255aa), lv_color_hex(0x3366cc));

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

void DisplayUI::updateMeter(const tuner_meter_t* meter) {
    const float forward = float(meter->forward_power_watts);
    const float reflected = float(meter->reflected_power_watts);
    const float swr = float(meter->swr);
    if (forward != m_fwdPower || reflected != m_refPower || swr != m_swr) {
        m_fwdPower = forward;
        m_refPower = reflected;
        m_swr = swr;
        lv_obj_invalidate(m_meter);
    }
    if (meter->band != m_band) {
        m_band = meter->band;
        lv_label_set_text_fmt(m_bandLabel, "Band: %s", TunerProtocol::bandToString(m_band));
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
    const int cx = m_geometry.centerX, cy = m_geometry.centerY;
    const int arm = m_geometry.radius;
    const int fwdPx = m_geometry.forwardPivot.x, fwdPy = m_geometry.forwardPivot.y;
    const int refPx = m_geometry.reflectedPivot.x, refPy = m_geometry.reflectedPivot.y;
    const auto fwdTip = m_geometry.forwardNeedle(fminf(m_fwdPower / m_maxPower, 1.0f));
    const auto refTip = m_geometry.reflectedNeedle(fminf(m_refPower / m_maxPower, 1.0f));

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
            lv_point_t p1 = {tick.line.from.x, tick.line.from.y};
            lv_point_t p2 = {tick.line.to.x, tick.line.to.y};
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
                const int lx = tick.label.x, ly = tick.label.y;
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
            lv_point_t p1 = {line.from.x, line.from.y};
            lv_point_t p2 = {line.to.x, line.to.y};
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
        snprintf(m_swrText, sizeof(m_swrText), "%.1f:1", m_swr);
    }
    lv_draw_label_dsc_t swr_dsc;
    lv_draw_label_dsc_init(&swr_dsc);
    swr_dsc.color = (m_swr > 2.0f) ? lv_color_hex(0xff4444) :
                    (m_swr > 1.5f) ? lv_color_hex(0xffaa00) : lv_color_hex(0x00ff00);
    swr_dsc.font  = &lv_font_montserrat_24;
    swr_dsc.align = LV_TEXT_ALIGN_CENTER;
    lv_area_t swr_a = {(lv_coord_t)(cx-30),(lv_coord_t)(cy-arm/2-12),
                       (lv_coord_t)(cx+30),(lv_coord_t)(cy-arm/2+12)};
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
    if (s_instance && s_instance->m_tuner) s_instance->m_tuner->memoryTune();
}

void DisplayUI::btnFullTuneEvent(lv_event_t* e) {
    if (s_instance && s_instance->m_tuner) s_instance->m_tuner->fullTune();
}

void DisplayUI::btnAnt1Event(lv_event_t* e) {
    if (s_instance && s_instance->m_tuner && s_instance->m_activeAnt != ANT_A)
        s_instance->m_tuner->toggleAntenna();
}

void DisplayUI::btnAnt2Event(lv_event_t* e) {
    if (s_instance && s_instance->m_tuner && s_instance->m_activeAnt != ANT_B)
        s_instance->m_tuner->toggleAntenna();
}

void DisplayUI::btnBypassEvent(lv_event_t* e) {
    if (s_instance && s_instance->m_tuner) s_instance->m_tuner->bypass();
}

void DisplayUI::btnAutoEvent(lv_event_t* e) {
    if (s_instance && s_instance->m_tuner) s_instance->m_tuner->setAutoMode();
}

void DisplayUI::meterTapEvent(lv_event_t* e) {
    if (!s_instance || !s_instance->m_meterScale) return;
    s_instance->m_meterScale->toggle();
    s_instance->updateMeterScale();
}

#endif // WITH_DISPLAY
