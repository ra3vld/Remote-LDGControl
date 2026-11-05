#ifndef TUNER_TYPES_H
#define TUNER_TYPES_H

#include <stdint.h>
#include "radio_types.h"
#include "tuning_types.h"

// Command bytes (single ASCII characters)
#define TUNER_CMD_WAKE          ' '
#define TUNER_CMD_TOGGLE_ANT    'A'
#define TUNER_CMD_MEM_TUNE      'T'
#define TUNER_CMD_FULL_TUNE     'F'
#define TUNER_CMD_BYPASS        'P'
#define TUNER_CMD_AUTO_MODE     'C'
#define TUNER_CMD_MANUAL_MODE   'M'
#define TUNER_CMD_SYNC          'Z'
#define TUNER_CMD_METER_START   'S'
#define TUNER_CMD_METER_STOP    'X'
// Application commands; never written directly to the tuner UART.
#define TUNER_CMD_SELECT_ANT_A  0x81
#define TUNER_CMD_SELECT_ANT_B  0x82
#define TUNER_CMD_CONTROLLED_MODE 0x83

// Response codes
#define TUNER_RESP_GOOD         'T'
#define TUNER_RESP_OKAY         'M'
#define TUNER_RESP_FAIL         'F'
#define TUNER_RESP_ERROR        'E'
#define TUNER_RESP_BYPASS       'P'
#define TUNER_RESP_AUTO         'A'
#define TUNER_RESP_MANUAL       'M'
#define TUNER_RESP_ANT_A        '1'
#define TUNER_RESP_ANT_B        '2'

// Meter packet structure
#define TUNER_METER_DATA_LEN    6
#define TUNER_METER_EOM         0x3B3B  // ";;"

// Band detection thresholds (from reverse-engineered "wtf" bytes)
typedef enum {
    BAND_UNKNOWN = 0,
    BAND_160M,
    BAND_80M,
    BAND_60M,
    BAND_40M,
    BAND_30M,
    BAND_20M,
    BAND_17M,
    BAND_15M,
    BAND_12M,
    BAND_10M,
    BAND_6M,
    BAND_COUNT
} tuner_band_t;

// Tuner operating mode
typedef enum {
    MODE_UNKNOWN = 0,
    MODE_AUTO,
    MODE_MANUAL,
    MODE_BYPASS,
    MODE_CONTROLLED // Application mode; never sent as an LDG UART byte.
} tuner_mode_t;

// Antenna selection
typedef enum {
    ANT_UNKNOWN = 0,
    ANT_A,
    ANT_B
} tuner_ant_t;

namespace ldg {
enum class AntennaInit : uint8_t { Unknown, Waiting, WaitingRfOff, Checking, Restoring, Ready, Error };
enum class AntennaAuto : uint8_t {
    Disabled, Idle, Pending, Selected, AlreadySelected, HighPower,
    InvalidPower, Unassigned, Uninitialized, Manual, Error, WaitingRfOff,
    ManualSelection, InitialBand, Retrying, WaitingMeter, Tuning, TuneFailed,
    TuneNoRf, TuneHighPower, TuneInvalidPower, TuneBandChanged, TuneManual, WaitingTuner,
    WaitingRadio, WaitingRx, WaitingBand
};
inline const char* antennaInitName(AntennaInit state) {
    switch (state) {
        case AntennaInit::Waiting: return "waiting_meter";
        case AntennaInit::WaitingRfOff: return "waiting_rf_off";
        case AntennaInit::Checking: return "checking";
        case AntennaInit::Restoring: return "restoring";
        case AntennaInit::Ready: return "ready";
        case AntennaInit::Error: return "error";
        default: return "unknown";
    }
}
inline const char* antennaAutoName(AntennaAuto state) {
    switch (state) {
        case AntennaAuto::Idle: return "idle";
        case AntennaAuto::Pending: return "pending";
        case AntennaAuto::Selected: return "selected";
        case AntennaAuto::AlreadySelected: return "already_selected";
        case AntennaAuto::HighPower: return "skipped_power";
        case AntennaAuto::InvalidPower: return "invalid_power";
        case AntennaAuto::Unassigned: return "unassigned";
        case AntennaAuto::Uninitialized: return "uninitialized";
        case AntennaAuto::Manual: return "manual_override";
        case AntennaAuto::Error: return "error";
        case AntennaAuto::WaitingRfOff: return "waiting_rf_off";
        case AntennaAuto::ManualSelection: return "manual_selection";
        case AntennaAuto::InitialBand: return "initial_band";
        case AntennaAuto::Retrying: return "retrying";
        case AntennaAuto::WaitingMeter: return "waiting_meter";
        case AntennaAuto::Tuning: return "tuning";
        case AntennaAuto::TuneFailed: return "tune_failed";
        case AntennaAuto::TuneNoRf: return "tune_no_rf";
        case AntennaAuto::TuneHighPower: return "tune_power_limit";
        case AntennaAuto::TuneInvalidPower: return "tune_invalid_power";
        case AntennaAuto::TuneBandChanged: return "tune_band_changed";
        case AntennaAuto::TuneManual: return "tune_manual_override";
        case AntennaAuto::WaitingTuner: return "waiting_tuner";
        case AntennaAuto::WaitingRadio: return "waiting_radio";
        case AntennaAuto::WaitingRx: return "waiting_rx";
        case AntennaAuto::WaitingBand: return "waiting_band";
        default: return "disabled";
    }
}
}

// Meter data structure
typedef struct {
    uint16_t forward_power_raw;
    uint16_t reflected_power_raw;
    uint16_t band_indicator;
    double forward_power_watts;
    double reflected_power_watts;
    double swr;
    tuner_band_t band;
} tuner_meter_t;

// A value copy, safe to pass between transports without borrowing UART state.
struct tuner_state_t {
    ldg::RadioState radio;
    bool antennaFromRadio = false;
    bool tuningManaged = false;
    ldg::TuningState tuningState = ldg::TuningState::Native;
    tuner_meter_t meter{};
    tuner_mode_t mode = MODE_UNKNOWN; // Last acknowledged physical LDG mode.
    tuner_ant_t antenna = ANT_UNKNOWN;
    tuner_ant_t lastAntenna = ANT_UNKNOWN;
    uint32_t meterAgeMs = UINT32_MAX;
    bool meterValid = false;
    bool remote = false;
    // Last observed band belongs to this source and survives RF silence.
    tuner_band_t lastBand = BAND_UNKNOWN;
    ldg::AntennaInit antennaInit = ldg::AntennaInit::Unknown;
    ldg::AntennaAuto antennaAuto = ldg::AntennaAuto::Disabled;
    tuner_band_t antennaAutoBand = BAND_UNKNOWN;
};

namespace ldg {
inline tuner_mode_t operatingMode(const tuner_state_t& state) {
    return state.tuningManaged ? MODE_CONTROLLED : state.mode;
}
inline uint8_t nextModeCommand(tuner_mode_t mode) {
    return mode == MODE_MANUAL ? TUNER_CMD_AUTO_MODE :
           mode == MODE_AUTO ? TUNER_CMD_CONTROLLED_MODE : TUNER_CMD_MANUAL_MODE;
}
inline bool isModeCommand(uint8_t command) {
    return command == TUNER_CMD_MANUAL_MODE || command == TUNER_CMD_AUTO_MODE || command == TUNER_CMD_CONTROLLED_MODE;
}
inline bool knownBand(tuner_band_t band) { return band > BAND_UNKNOWN && band < BAND_COUNT; }
inline bool knownAntenna(tuner_ant_t antenna) { return antenna == ANT_A || antenna == ANT_B; }
inline tuner_ant_t lastKnownAntenna(const tuner_state_t& state) {
    return knownAntenna(state.antenna) ? state.antenna :
           knownAntenna(state.lastAntenna) ? state.lastAntenna : ANT_UNKNOWN;
}
inline tuner_band_t lastKnownBand(const tuner_state_t& state) {
    return state.meterValid && knownBand(state.meter.band) ? state.meter.band :
           knownBand(state.lastBand) ? state.lastBand : BAND_UNKNOWN;
}
inline bool bandIsHeld(const tuner_state_t& state) {
    return knownBand(lastKnownBand(state)) && !(state.meterValid && knownBand(state.meter.band));
}
}

#endif // TUNER_TYPES_H
