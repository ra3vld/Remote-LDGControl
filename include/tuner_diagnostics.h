#pragma once

#include <ArduinoJson.h>
#include <stdint.h>

namespace ldg {

// Fixed-size copies: status readers never wait for a tune or borrow UART buffers.
struct TunerDiagnostics {
    uint32_t rxBytes = 0, frames = 0, framingErrors = 0, resyncs = 0;
    uint32_t bufferFull = 0, fifoOverflow = 0, frameErrors = 0, parityErrors = 0, breaks = 0;
    uint32_t lastFrameMs = 0;
    bool hasFrame = false;
    double psuVoltage = 0;
    uint8_t lastFrame[8]{};
    uint8_t rxTail[32]{};
    uint8_t rxTailCount = 0;
    bool hasCommand = false, commandSuccess = false;
    uint8_t command = 0, response = 0;
    uint32_t commandDurationMs = 0;
};

inline void writeTunerDiagnostics(JsonObject out, const TunerDiagnostics& d, uint32_t now) {
    out["source"] = "local";
    out["rx_bytes"] = d.rxBytes;
    out["frames"] = d.frames;
    out["framing_errors"] = d.framingErrors;
    out["resyncs"] = d.resyncs;
    JsonObject errors = out["errors"].to<JsonObject>();
    errors["buffer_full"] = d.bufferFull;
    errors["fifo_overflow"] = d.fifoOverflow;
    errors["frame"] = d.frameErrors;
    errors["parity"] = d.parityErrors;
    errors["break"] = d.breaks;
    out["last_command"] = nullptr;
    if (d.hasCommand) {
        JsonObject command = out["last_command"].to<JsonObject>();
        char sent[] = {char(d.command), '\0'};
        char reply[] = {char(d.response), '\0'};
        command["command"] = sent;
        command["ack"] = nullptr;
        if (d.response) {
            if (d.command == 'Z') command["ack"] = "sync";
            else command["ack"] = reply;
        }
        command["success"] = d.commandSuccess;
        command["duration_ms"] = d.commandDurationMs;
    }
    out["frame_age_ms"] = nullptr;
    out["last_frame_hex"] = nullptr;
    out["forward_raw"] = nullptr;
    out["reflected_raw"] = nullptr;
    out["band_raw"] = nullptr;
    out["psu_voltage"] = nullptr;
    static const char hex[] = "0123456789abcdef";
    char text[sizeof(d.rxTail) * 2 + 1];
    for (unsigned i = 0; i < d.rxTailCount; ++i) {
        text[2*i] = hex[d.rxTail[i] >> 4];
        text[2*i+1] = hex[d.rxTail[i] & 15];
    }
    text[2*d.rxTailCount] = '\0';
    out["rx_tail_hex"] = text;
    if (d.hasFrame) {
        for (unsigned i = 0; i < sizeof(d.lastFrame); ++i) {
            text[2*i] = hex[d.lastFrame[i] >> 4];
            text[2*i+1] = hex[d.lastFrame[i] & 15];
        }
        text[sizeof(d.lastFrame)*2] = '\0';
        out["last_frame_hex"] = text;
        out["frame_age_ms"] = uint32_t(now - d.lastFrameMs);
        out["forward_raw"] = uint16_t((d.lastFrame[0] << 8) | d.lastFrame[1]);
        out["reflected_raw"] = uint16_t((d.lastFrame[2] << 8) | d.lastFrame[3]);
        out["band_raw"] = uint16_t((d.lastFrame[4] << 8) | d.lastFrame[5]);
        out["psu_voltage"] = d.psuVoltage;
    }
}

} // namespace ldg
