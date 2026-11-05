#pragma once

#include <ArduinoJson.h>
#include <stdint.h>

namespace ldg {

enum class MemoryTuneResult : uint8_t {
    NotRequired, Tuning, Succeeded, Failed, NoRf, HighPower, InvalidPower, BandChanged, ManualOverride
};
inline const char* memoryTuneResultName(MemoryTuneResult result) {
    switch (result) {
        case MemoryTuneResult::Tuning: return "tuning";
        case MemoryTuneResult::Succeeded: return "succeeded";
        case MemoryTuneResult::Failed: return "failed";
        case MemoryTuneResult::NoRf: return "no_rf";
        case MemoryTuneResult::HighPower: return "power_limit";
        case MemoryTuneResult::InvalidPower: return "invalid_power";
        case MemoryTuneResult::BandChanged: return "band_changed";
        case MemoryTuneResult::ManualOverride: return "manual_override";
        default: return "not_required";
    }
}

struct AntennaSelectionDiagnostics {
    bool present = false, modeGuard = false, manualConfirmed = false;
    bool selectionAttempted = false, selectionConfirmed = false, autoRestored = false;
    bool deferred = false;
    uint8_t recovery = 0;
    uint8_t target = 0, response = 0, attempts = 0, manualAttempts = 0, restoreAttempts = 0;
    uint32_t durationMs = 0;
    MemoryTuneResult memoryTune = MemoryTuneResult::NotRequired;
    uint8_t tuneResponse = 0;
};

// Fixed-size copies: status readers never wait for a tune or borrow UART buffers.
struct TunerDiagnostics {
    AntennaSelectionDiagnostics antennaSelection;
    uint32_t rxBytes = 0, frames = 0, framingErrors = 0, resyncs = 0;
    uint32_t meterRestarts = 0;
    uint32_t bufferFull = 0, fifoOverflow = 0, frameErrors = 0, parityErrors = 0, breaks = 0;
    uint32_t lastFrameMs = 0;
    bool hasFrame = false;
    double psuVoltage = 0;
    uint8_t lastFrame[8]{};
    uint8_t rxTail[32]{};
    uint8_t rxTailCount = 0;
    bool hasCommand = false, commandSuccess = false;
    uint8_t command = 0, response = 0;
    uint8_t lateResponse = 0;
    uint32_t commandDurationMs = 0;
    uint8_t exchange = 0;
    bool exchangeWritten = false, exchangeUartError = false, meterRestarted = false;
    uint8_t commandRx[32]{};
    uint8_t commandRxCount = 0;
    uint32_t commandRxBytes = 0;
    uint32_t responseTimeoutMs = 0, responseWaitMs = 0;
};

inline void writeTunerDiagnostics(JsonObject out, const TunerDiagnostics& d, uint32_t now) {
    out["source"] = "local";
    out["rx_bytes"] = d.rxBytes;
    out["frames"] = d.frames;
    out["framing_errors"] = d.framingErrors;
    out["resyncs"] = d.resyncs;
    out["meter_restarts"] = d.meterRestarts;
    out["antenna_selection"] = nullptr;
    if (d.antennaSelection.present) {
        const auto& a = d.antennaSelection;
        JsonObject selection = out["antenna_selection"].to<JsonObject>();
        selection["target"] = a.target;
        selection["mode_guard"] = a.modeGuard;
        selection["manual_confirmed"] = a.manualConfirmed;
        selection["attempted"] = a.selectionAttempted;
        selection["confirmed"] = a.selectionConfirmed;
        selection["attempts"] = a.attempts;
        selection["manual_attempts"] = a.manualAttempts;
        selection["restore_attempts"] = a.restoreAttempts;
        selection["auto_restored"] = a.autoRestored;
        selection["deferred"] = a.deferred;
        selection["recovery"] = a.recovery;
        selection["duration_ms"] = a.durationMs;
        selection["memory_tune"] = memoryTuneResultName(a.memoryTune);
        selection["memory_tune_ack"] = nullptr;
        if (a.tuneResponse) {
            char ack[] = {char(a.tuneResponse), '\0'};
            selection["memory_tune_ack"] = ack;
        }
        selection["ack"] = nullptr;
        if (a.response) {
            char ack[] = {char(a.response), '\0'};
            selection["ack"] = ack;
        }
    }
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
        command["late_ack"] = nullptr;
        if (d.lateResponse) {
            char late[] = {char(d.lateResponse), '\0'};
            command["late_ack"] = d.lateResponse == 'Z' ? "sync" : late;
        }
        command["duration_ms"] = d.commandDurationMs;
        command["exchange"] = nullptr;
        if (d.exchange) {
            char exchange[] = {char(d.exchange), '\0'};
            command["exchange"] = exchange;
        }
        command["written"] = d.exchangeWritten;
        command["uart_error"] = d.exchangeUartError;
        command["meter_restarted"] = d.meterRestarted;
        command["rx_bytes"] = d.commandRxBytes;
        command["timeout_ms"] = d.responseTimeoutMs;
        command["wait_ms"] = d.responseWaitMs;
        char rx[sizeof(d.commandRx) * 2 + 1];
        static const char hex[] = "0123456789abcdef";
        for (unsigned i = 0; i < d.commandRxCount; ++i) {
            rx[2*i] = hex[d.commandRx[i] >> 4];
            rx[2*i+1] = hex[d.commandRx[i] & 15];
        }
        rx[2*d.commandRxCount] = '\0';
        command["rx_hex"] = rx;
        const char* error = nullptr;
        if (!d.commandSuccess) {
            if (!d.exchange) error = "meter_stop_failed";
            else if (!d.exchangeWritten) error = "write_failed";
            else if (d.exchangeUartError) error = "uart_error";
            else if (!d.response) error = "timeout";
            else if (!d.meterRestarted) error = "meter_restart_failed";
            else error = "tune_failed";
        }
        command["error"] = error;
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
