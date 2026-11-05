#ifndef DISPLAY_UI_H
#define DISPLAY_UI_H

#include <Arduino.h>

#ifdef WITH_DISPLAY

#include <lvgl.h>
#include <TFT_eSPI.h>
#include "tuner_protocol.h"
#include "config.h"
#include "meter_scale.h"
#include "meter_geometry.h"
#include "command_feedback.h"
#include "meter_face_cache.h"
#include "needle_motion.h"

class DisplayUI {
public:
    DisplayUI();
    ~DisplayUI();

    bool begin(bool (*commandHandler)(uint8_t), ldg::MeterScale* meterScale);
    void loop();
    void updateMeter(const tuner_meter_t* meter, bool bandHeld = false);
    void updateStatus(tuner_mode_t mode, tuner_ant_t antenna, tuner_ant_t last = ANT_UNKNOWN);
    void updateNetworkInfo();
    void updateState(const tuner_state_t& state);
    void setCommandPending(bool pending);
    void updateCommandFeedback(const ldg::CommandFeedback::Snapshot& feedback);
    void writeDiagnostics(JsonObject doc) const;
#ifdef DISPLAY_UI_TESTING
    void testRenderer(bool cache, bool smoothing) {
        m_faceAllowed = cache; m_smoothing = smoothing; m_faceAttempted = false;
        lv_obj_invalidate(m_meter);
    }
    void testDelta(bool enabled) { m_deltaAllowed = enabled; m_previousValid = false; }
    void testLegacyNeedles(bool legacy) { m_fastNeedles = !legacy; m_previousValid = false; }
#endif

private:
    TFT_eSPI* m_tft;
    bool (*m_commandHandler)(uint8_t);
    bool m_commandPending = false;
    ldg::MeterScale* m_meterScale;

    // LVGL objects
    lv_obj_t* m_scr;
    lv_obj_t* m_meter;          // Custom cross-needle meter container
    lv_obj_t* m_bandLabel;
    lv_obj_t* m_scaleLabel;
    lv_obj_t* m_fwdPowerLabel = nullptr;
    lv_obj_t* m_refPowerLabel = nullptr;
    lv_obj_t* m_modeLabel;
    lv_obj_t* m_ssidLabel;
    lv_obj_t* m_ipLabel;
    lv_obj_t* m_antennaInitLabel = nullptr;
    ldg::AntennaInit m_antennaInit = ldg::AntennaInit::Unknown;

    // Buttons
    lv_obj_t* m_btnMemTune;
    lv_obj_t* m_btnFullTune;
    lv_obj_t* m_btnAnt1;
    lv_obj_t* m_btnAnt2;
    lv_obj_t* m_ant1NameLabel;
    lv_obj_t* m_ant2NameLabel;
    lv_obj_t* m_btnBypass;
    lv_obj_t* m_bypassButtonLabel = nullptr;
    lv_obj_t* m_btnAuto;
    lv_obj_t* m_modeButtonLabel = nullptr;
    lv_obj_t* m_commandLabel = nullptr;
    uint32_t m_feedbackToken = 0, m_feedbackAt = 0;
    bool m_feedbackFinished = false;

    // Meter state
    float m_maxPower;
    bool m_logarithmic = false;
    float m_forwardFraction = 0, m_reflectedFraction = 0;
    char m_tickLabels[5][12] = {};
    tuner_meter_t m_pendingMeter{};
    bool m_meterPending = false, m_pendingBandHeld = false;
    uint32_t m_lastMeterRender = 0;
    float m_fwdPower;
    float m_refPower;
    float m_swr;
    tuner_ant_t m_activeAnt;
    tuner_ant_t m_lastAnt = ANT_UNKNOWN;
    tuner_mode_t m_activeMode = MODE_UNKNOWN;
    tuner_band_t m_band = BAND_UNKNOWN;
    bool m_bandHeld = false;
    bool m_fwdValid = false, m_refValid = false;
    ldg::MeterGeometry m_geometry;
    ldg::MeterFaceCache m_face;
    ldg::NeedleMotion m_forwardMotion, m_reflectedMotion;
    bool m_smoothing = true, m_faceAllowed = true, m_faceAttempted = false;
    lv_area_t m_faceBounds{};
    lv_color_t* m_deltaScratch = nullptr;
    bool m_deltaAllowed = true, m_previousValid = false;
    bool m_fastNeedles = true;
    uint32_t m_faceVersion = 0, m_previousFaceVersion = 0;
    float m_previousForward = 0, m_previousReflected = 0, m_previousSwr = NAN;
    char m_previousSwrText[16] = "--";
    char m_previousForwardText[32] = "FWD --", m_previousReflectedText[32] = "REF --";
    struct Diagnostics {
        uint32_t frames = 0, frameMs = 0, maxFrameMs = 0, pixels = 0;
        uint32_t flushUs = 0, drawUs = 0, faceBuildUs = 0, faceBytes = 0;
        uint32_t faceBuilds = 0, faceHits = 0, vectorDraws = 0;
        uint32_t sentPixels = 0, deltaFlushes = 0;
    };
    Diagnostics m_diagnostics;
    uint32_t m_frameFlushUs = 0, m_frameDrawUs = 0;
    uint32_t m_frameSentPixels = 0;
    mutable portMUX_TYPE m_diagnosticsMux = portMUX_INITIALIZER_UNLOCKED;
    char m_swrText[16] = "--";
    uint32_t m_lastNamesUpdate = 0;

    // Touch
    void setupTouch();
    void readTouch(lv_indev_drv_t* drv, lv_indev_data_t* data);

    // Button events
    static void btnMemTuneEvent(lv_event_t* e);
    static void btnFullTuneEvent(lv_event_t* e);
    static void btnAnt1Event(lv_event_t* e);
    static void btnAnt2Event(lv_event_t* e);
    static void btnBypassEvent(lv_event_t* e);
    static void btnAutoEvent(lv_event_t* e);
    static void meterTapEvent(lv_event_t* e);

    void updateAntButtons();
    void updateMeterScale();
    void invalidateNeedle(ldg::MeterGeometry::Point pivot, ldg::MeterGeometry::Point before,
                          ldg::MeterGeometry::Point after);

    // Custom meter drawing
    static void drawMeterEvent(lv_event_t* e);
    void drawCrossNeedle(lv_event_t* e);
    void drawMeterFace(lv_draw_ctx_t* context, const lv_area_t& bounds);
    void drawMeterForeground(lv_draw_ctx_t* context, const lv_area_t& bounds,
        float forward, float reflected, float swr, const char* swrText);
    bool buildMeterFace(const lv_area_t& bounds);
    void animateNeedles(uint32_t now);

    // Display flush
    static void displayFlush(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p);
    static void displayMonitor(lv_disp_drv_t*, uint32_t timeMs, uint32_t pixels);

    static DisplayUI* s_instance;
};

#endif // WITH_DISPLAY
#endif // DISPLAY_UI_H
