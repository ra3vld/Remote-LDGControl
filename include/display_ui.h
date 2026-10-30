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

class DisplayUI {
public:
    DisplayUI();
    ~DisplayUI();

    bool begin(TunerProtocol* tuner, ldg::MeterScale* meterScale);
    void loop();
    void updateMeter(const tuner_meter_t* meter);
    void updateStatus(tuner_mode_t mode, tuner_ant_t antenna);
    void updateNetworkInfo();

private:
    TFT_eSPI* m_tft;
    TunerProtocol* m_tuner;
    ldg::MeterScale* m_meterScale;

    // LVGL objects
    lv_obj_t* m_scr;
    lv_obj_t* m_meter;          // Custom cross-needle meter container
    lv_obj_t* m_bandLabel;
    lv_obj_t* m_scaleLabel;
    lv_obj_t* m_modeLabel;
    lv_obj_t* m_ssidLabel;
    lv_obj_t* m_ipLabel;

    // Buttons
    lv_obj_t* m_btnMemTune;
    lv_obj_t* m_btnFullTune;
    lv_obj_t* m_btnAnt1;
    lv_obj_t* m_btnAnt2;
    lv_obj_t* m_ant1NameLabel;
    lv_obj_t* m_ant2NameLabel;
    lv_obj_t* m_btnBypass;
    lv_obj_t* m_btnAuto;

    // Meter state
    float m_maxPower;
    float m_fwdPower;
    float m_refPower;
    float m_swr;
    tuner_ant_t m_activeAnt;
    tuner_mode_t m_activeMode = MODE_UNKNOWN;
    tuner_band_t m_band = BAND_UNKNOWN;
    ldg::MeterGeometry m_geometry;
    float m_swrTextValue = -1;
    char m_swrText[16] = {};

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

    // Custom meter drawing
    static void drawMeterEvent(lv_event_t* e);
    void drawCrossNeedle(lv_event_t* e);

    // Display flush
    static void displayFlush(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p);

    static DisplayUI* s_instance;
};

#endif // WITH_DISPLAY
#endif // DISPLAY_UI_H
