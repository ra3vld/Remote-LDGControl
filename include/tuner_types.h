#ifndef TUNER_TYPES_H
#define TUNER_TYPES_H

#include <stdint.h>

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
    MODE_BYPASS
} tuner_mode_t;

// Antenna selection
typedef enum {
    ANT_UNKNOWN = 0,
    ANT_A,
    ANT_B
} tuner_ant_t;

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
    tuner_meter_t meter{};
    tuner_mode_t mode = MODE_UNKNOWN;
    tuner_ant_t antenna = ANT_UNKNOWN;
    bool meterValid = false;
    bool remote = false;
    // Last observed band belongs to this source and survives RF silence.
    tuner_band_t lastBand = BAND_UNKNOWN;
};

namespace ldg {
inline bool knownBand(tuner_band_t band) { return band > BAND_UNKNOWN && band < BAND_COUNT; }
inline tuner_band_t lastKnownBand(const tuner_state_t& state) {
    return state.meterValid && knownBand(state.meter.band) ? state.meter.band :
           knownBand(state.lastBand) ? state.lastBand : BAND_UNKNOWN;
}
inline bool bandIsHeld(const tuner_state_t& state) {
    return knownBand(lastKnownBand(state)) && !(state.meterValid && knownBand(state.meter.band));
}
}

#endif // TUNER_TYPES_H
