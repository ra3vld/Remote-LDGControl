#include <unity.h>
#include <vector>
#include <string>
#include <utility>
#include "tuner_protocol.h"
#include "voltage_sensor.h"
#include "mqtt_protocol.h"
#include "operating_mode.h"
#include "../../src/tuner_protocol.cpp"

uint32_t testMillis = 0;
unsigned testMutexTakes = 0;
HardwareSerial* HardwareSerial::latest = nullptr;
std::function<void(HardwareSerial&, uint8_t)> HardwareSerial::written;
VoltageSensor voltageSensor;
double VoltageSensor::readVoltage() { return 13.8; }
static std::vector<tuner_meter_t> received;

// Literal wire fixtures from LDG MeterProtocol.pdf, pp. 5-6. In particular,
// antenna acknowledgements are ASCII digits and Z returns this whole string.
static const std::string syncReply = "000000000000000AzAz";
static void respondSync(HardwareSerial& serial, uint8_t command) {
    if (command == 'Z') for (char byte : syncReply) serial.bytes.push_back(byte);
}
static void onMeter(const tuner_meter_t* meter) { received.push_back(*meter); }
static void begin(TunerProtocol& tuner) {
    TEST_ASSERT_TRUE(tuner.begin(35, 22, 38400));
    tuner.setMeterCallback(onMeter);
}
static std::vector<uint8_t> frame(uint16_t fwd = 375, uint16_t ref = 75, uint16_t band = 1160) {
    return {uint8_t(fwd >> 8), uint8_t(fwd), uint8_t(ref >> 8), uint8_t(ref),
            uint8_t(band >> 8), uint8_t(band), 0x3b, 0x3b};
}
static void feed(TunerProtocol& tuner, const std::vector<uint8_t>& bytes) {
    for (uint8_t byte : bytes) HardwareSerial::latest->bytes.push_back(byte);
    tuner.process();
}
void setUp() { testMillis = 0; HardwareSerial::written = nullptr; received.clear(); }
void tearDown() {}

void test_tune_response_and_uart_failure_are_not_success() {
    TunerProtocol tuner;
    begin(tuner);
    for (const char response : {'F', 'E', 'P', 'T', 'M'}) {
        HardwareSerial::written = [response](HardwareSerial& serial, uint8_t command) {
            respondSync(serial, command);
            if (command == 'F' || command == 'T') serial.bytes.push_back(response);
        };
        TEST_ASSERT_EQUAL(response == 'T' || response == 'M', tuner.fullTune());
        TEST_ASSERT_EQUAL(response == 'T' || response == 'M', tuner.memoryTune());
    }
    HardwareSerial::latest->failWrites = true;
    TEST_ASSERT_FALSE(tuner.memoryTune());
    TEST_ASSERT_FALSE(tuner.fullTune());
    TEST_ASSERT_FALSE(tuner.toggleAntenna());
}

void test_meter_timeout_and_recovery_survive_clock_wrap() {
    testMillis = 0xfffff000u;
    TunerProtocol tuner;
    begin(tuner);
    TEST_ASSERT_FALSE(tuner.isConnected());
    feed(tuner, frame());
    TEST_ASSERT_TRUE(tuner.isConnected());
    TEST_ASSERT_TRUE(tuner.getState().meterValid);
    const unsigned mutexTakes = testMutexTakes;
    tuner.getState();
    tuner.getMode();
    tuner.isConnected();
    TEST_ASSERT_EQUAL_UINT(mutexTakes, testMutexTakes);
    testMillis += 3000;
    TEST_ASSERT_TRUE(tuner.getState().meterValid);
    testMillis += 1;
    TEST_ASSERT_FALSE(tuner.getState().meterValid);
    TEST_ASSERT_FALSE(tuner.isConnected());
    feed(tuner, frame());
    TEST_ASSERT_TRUE(tuner.isConnected());
    TEST_ASSERT_TRUE(tuner.getState().meterValid);
}

void test_absolute_selection_uses_numeric_ack_and_does_not_repeat_toggle() {
    TunerProtocol tuner;
    begin(tuner);
    uint8_t antenna = 1;
    unsigned toggles = 0;
    HardwareSerial::written = [&](HardwareSerial& serial, uint8_t command) {
        if (command == 'A') {
            antenna = antenna == 1 ? 2 : 1;
            ++toggles;
            serial.bytes.push_back(antenna == 1 ? '1' : '2');
        }
    };
    TEST_ASSERT_TRUE(tuner.selectAntenna(ANT_A));
    TEST_ASSERT_EQUAL_INT(ANT_A, tuner.getAntenna());
    TEST_ASSERT_EQUAL_UINT(1, antenna);
    TEST_ASSERT_EQUAL_UINT(2, toggles);
    TEST_ASSERT_TRUE(tuner.selectAntenna(ANT_A));
    TEST_ASSERT_EQUAL_UINT(2, toggles);
    TEST_ASSERT_TRUE(tuner.selectAntenna(ANT_B));
    TEST_ASSERT_EQUAL_INT(ANT_B, tuner.getAntenna());
    TEST_ASSERT_EQUAL_UINT(2, antenna);
    TEST_ASSERT_EQUAL_UINT(3, toggles);
    TEST_ASSERT_TRUE(tuner.selectAntenna(ANT_B));
    TEST_ASSERT_EQUAL_UINT(3, toggles);
    TEST_ASSERT_FALSE(tuner.selectAntenna(ANT_UNKNOWN));
}
void test_last_band_survives_rf_silence_and_unknown_band_packets() {
    TunerProtocol tuner;
    begin(tuner);
    TEST_ASSERT_EQUAL_INT(BAND_UNKNOWN, tuner.getState().lastBand);
    feed(tuner, frame());
    TEST_ASSERT_EQUAL_INT(BAND_20M, tuner.getState().lastBand);
    testMillis += 3001;
    auto state = tuner.getState();
    TEST_ASSERT_FALSE(state.meterValid);
    JsonDocument status;
    ldg::writeState(status, state, false);
    TEST_ASSERT_EQUAL_STRING("20m", status["band"].as<const char*>());
    TEST_ASSERT_TRUE(status["band_held"].as<bool>());
    TEST_ASSERT_TRUE(status["fwd_power"].isNull());
    feed(tuner, frame(0, 0, 0));
    state = tuner.getState();
    TEST_ASSERT_TRUE(state.meterValid);
    TEST_ASSERT_EQUAL_INT(BAND_UNKNOWN, state.meter.band);
    TEST_ASSERT_EQUAL_INT(BAND_20M, state.lastBand);
    ldg::writeState(status, state, false);
    TEST_ASSERT_EQUAL_STRING("20m", status["band"].as<const char*>());
    TEST_ASSERT_TRUE(status["band_held"].as<bool>());
    TEST_ASSERT_EQUAL_DOUBLE(0, status["fwd_power"].as<double>());
    feed(tuner, frame(375, 0, 1610));
    state = tuner.getState();
    TEST_ASSERT_EQUAL_INT(BAND_30M, state.lastBand);
    ldg::writeState(status, state, false);
    TEST_ASSERT_EQUAL_STRING("30m", status["band"].as<const char*>());
    TEST_ASSERT_FALSE(status["band_held"].as<bool>());
}

void test_idle_telemetry_does_not_forget_confirmed_controls_or_toggle_the_same_antenna() {
    for (uint32_t clock : {0u, 0xfffffc00u}) {
        testMillis = clock;
        HardwareSerial::written = nullptr;
        TunerProtocol tuner;
        begin(tuner);
        unsigned toggles = 0;
        bool allowSync = false;
        HardwareSerial::written = [&](HardwareSerial& serial, uint8_t command) {
            if (command == 'A') { ++toggles; serial.bytes.push_back('2'); }
            if (command == 'C') serial.bytes.push_back('A');
            if (allowSync) respondSync(serial, command);
        };
        TEST_ASSERT_TRUE(tuner.selectAntenna(ANT_B));
        TEST_ASSERT_TRUE(tuner.setAutoMode());
        feed(tuner, frame(150, 3));
        testMillis += 3001;
        TEST_ASSERT_FALSE(tuner.isConnected());
        TEST_ASSERT_FALSE(tuner.getState().meterValid);
        TEST_ASSERT_EQUAL_INT(ANT_B, tuner.getState().antenna);
        TEST_ASSERT_EQUAL_INT(MODE_AUTO, tuner.getState().mode);
        TEST_ASSERT_FALSE(tuner.selectAntenna(ANT_B)); // No reply: no false success, no relay toggle.
        TEST_ASSERT_EQUAL_UINT(1, toggles);
        TEST_ASSERT_EQUAL_INT(ANT_B, tuner.getState().antenna);
        allowSync = true;
        TEST_ASSERT_TRUE(tuner.selectAntenna(ANT_B));
        TEST_ASSERT_EQUAL_UINT(1, toggles);
        feed(tuner, frame(150, 3));
        TEST_ASSERT_TRUE(tuner.isConnected());
        TEST_ASSERT_EQUAL_INT(ANT_B, tuner.getState().antenna);
    }
}

void test_startup_wait_preserves_a_full_rate_uart_burst() {
    HardwareSerial::written = [](HardwareSerial& serial, uint8_t command) {
        if (command != 'S') return;
        std::vector<uint8_t> burst;
        for (unsigned i = 0; i < 120; ++i) {
            const auto bytes = frame(150, 3);
            burst.insert(burst.end(), bytes.begin(), bytes.end());
        }
        // 960 bytes fit in the 250 ms S-settling interval at 38400 8N1.
        for (unsigned i = 0; i < burst.size(); ++i)
            serial.schedule((i + 1) * 1000 / 3840, {burst[i]});
    };
    TunerProtocol tuner;
    begin(tuner);
    tuner.process();
    TEST_ASSERT_EQUAL_UINT(120, received.size());
    TEST_ASSERT_EQUAL_UINT(0, tuner.getDiagnostics().bufferFull);
    TEST_ASSERT_EQUAL_UINT(960, tuner.getDiagnostics().rxBytes);
}

void test_uart_fault_discards_the_damaged_batch_before_resynchronizing() {
    for (auto error : {UART_BREAK_ERROR, UART_BUFFER_FULL_ERROR, UART_FIFO_OVF_ERROR,
                       UART_FRAME_ERROR, UART_PARITY_ERROR}) {
        TunerProtocol tuner;
        begin(tuner);
        received.clear();
        HardwareSerial::latest->receiveError(error);
        feed(tuner, frame(2211, 769)); // A buffered, apparently framed sample is untrusted.
        TEST_ASSERT_TRUE(received.empty());
        TEST_ASSERT_FALSE(tuner.isConnected());
        feed(tuner, frame(150, 3)); // Only establishes a fresh boundary.
        TEST_ASSERT_TRUE(received.empty());
        feed(tuner, frame(150, 3));
        TEST_ASSERT_EQUAL_UINT(1, received.size());
        TEST_ASSERT_EQUAL_UINT(150, received.back().forward_power_raw);
        const auto diagnostics = tuner.getDiagnostics();
        TEST_ASSERT_EQUAL_UINT(1, diagnostics.bufferFull + diagnostics.fifoOverflow +
            diagnostics.frameErrors + diagnostics.parityErrors + diagnostics.breaks);
        TEST_ASSERT_EQUAL_UINT(1, diagnostics.resyncs);
    }
}

void test_uart_fault_during_an_ack_requires_recovery_instead_of_reporting_success() {
    TunerProtocol tuner;
    begin(tuner);
    HardwareSerial::written = [](HardwareSerial& serial, uint8_t command) {
        if (command == 'C') {
            serial.receiveError(UART_FRAME_ERROR);
            serial.bytes.push_back('A');
        }
    };
    TEST_ASSERT_FALSE(tuner.setAutoMode());
    TEST_ASSERT_EQUAL_INT(MODE_UNKNOWN, tuner.getMode());
    TEST_ASSERT_FALSE(tuner.isConnected());
    TEST_ASSERT_EQUAL_UINT(1, tuner.getDiagnostics().frameErrors);
    HardwareSerial::written = [](HardwareSerial& serial, uint8_t command) {
        respondSync(serial, command);
        if (command == 'C') serial.bytes.push_back('A');
    };
    TEST_ASSERT_TRUE(tuner.setAutoMode());
    TEST_ASSERT_EQUAL_INT(MODE_AUTO, tuner.getMode());
}

void test_uart_diagnostics_are_bounded_copies_in_wire_order_for_any_read_size() {
    for (unsigned chunk : {1u, 2u, 3u, 7u, 43u}) {
        testMillis = 0xfffffc00u;
        TunerProtocol tuner;
        begin(tuner);
        JsonDocument initial;
        ldg::writeTunerDiagnostics(initial.to<JsonObject>(), tuner.getDiagnostics(), millis());
        TEST_ASSERT_TRUE(initial["last_frame_hex"].isNull());
        TEST_ASSERT_TRUE(initial["frame_age_ms"].isNull());
        TEST_ASSERT_EQUAL_STRING("", initial["rx_tail_hex"].as<const char*>());
        std::vector<uint8_t> stream;
        for (unsigned i = 0; i < 5; ++i) {
            const auto bytes = frame(150, 3);
            stream.insert(stream.end(), bytes.begin(), bytes.end());
        }
        stream.insert(stream.end(), {0x12, 0x34, 0x56}); // Ring wraps inside a frame.
        for (unsigned i = 0; i < stream.size(); i += chunk)
            feed(tuner, {stream.begin() + i, stream.begin() + std::min(i + chunk, unsigned(stream.size()))});
        const unsigned takes = testMutexTakes;
        const auto diagnostics = tuner.getDiagnostics();
        TEST_ASSERT_EQUAL_UINT(takes, testMutexTakes); // Status cannot wait on a UART command.
        TEST_ASSERT_EQUAL_UINT(43, diagnostics.rxBytes);
        TEST_ASSERT_EQUAL_UINT(5, diagnostics.frames);
        TEST_ASSERT_EQUAL_UINT(32, diagnostics.rxTailCount);
        TEST_ASSERT_EQUAL_MEMORY(stream.data() + stream.size() - 32, diagnostics.rxTail, 32);
        testMillis += 3001;
        JsonDocument doc;
        ldg::writeTunerDiagnostics(doc.to<JsonObject>(), diagnostics, millis());
        TEST_ASSERT_EQUAL_STRING("0096000304883b3b", doc["last_frame_hex"].as<const char*>());
        TEST_ASSERT_EQUAL_UINT(3001, doc["frame_age_ms"].as<uint32_t>());
        TEST_ASSERT_EQUAL_UINT(150, doc["forward_raw"].as<uint16_t>());
        TEST_ASSERT_EQUAL_UINT(3, doc["reflected_raw"].as<uint16_t>());
        TEST_ASSERT_EQUAL_UINT(1160, doc["band_raw"].as<uint16_t>());
        TEST_ASSERT_EQUAL_DOUBLE(13.8, doc["psu_voltage"].as<double>());
        TEST_ASSERT_EQUAL_STRING("local", doc["source"].as<const char*>());
        TEST_ASSERT_EQUAL_UINT(64, strlen(doc["rx_tail_hex"].as<const char*>()));
        TEST_ASSERT_LESS_THAN_UINT(800, measureJson(doc));
        feed(tuner, {0xab, 0xcd, 0xef});
        TEST_ASSERT_EQUAL_MEMORY(stream.data() + stream.size() - 32, diagnostics.rxTail, 32);
    }
}

void test_invalid_trailers_are_counted_separately_from_driver_errors() {
    TunerProtocol tuner;
    begin(tuner);
    auto damaged = frame(150, 3);
    damaged[6] = 0x55;
    feed(tuner, damaged);
    feed(tuner, frame(150, 3));
    feed(tuner, frame(150, 3));
    TEST_ASSERT_EQUAL_UINT(1, tuner.getDiagnostics().framingErrors);
    TEST_ASSERT_EQUAL_UINT(1, tuner.getDiagnostics().resyncs);
    TEST_ASSERT_EQUAL_UINT(1, tuner.getDiagnostics().frames);
    TEST_ASSERT_EQUAL_UINT(0, tuner.getDiagnostics().frameErrors);
}

void test_command_specific_acknowledgements() {
    TunerProtocol tuner;
    begin(tuner);
    HardwareSerial::written = [](HardwareSerial& serial, uint8_t command) {
        respondSync(serial, command);
        if (command == 'C') serial.bytes.push_back('P');
        if (command == 'M') serial.bytes.push_back('M');
        if (command == 'P') serial.bytes.push_back('P');
    };
    TEST_ASSERT_FALSE(tuner.setAutoMode());
    TEST_ASSERT_FALSE(tuner.isConnected());
    TEST_ASSERT_TRUE(tuner.setManualMode());
    TEST_ASSERT_EQUAL_INT(MODE_MANUAL, tuner.getState().mode);
    TEST_ASSERT_TRUE(tuner.bypass());
    TEST_ASSERT_EQUAL_INT(MODE_BYPASS, tuner.getState().mode);
    TEST_ASSERT_TRUE(tuner.sync());
}

void test_mode_commands_do_not_inherit_a_fault_from_the_drained_meter_batch() {
    for (uint8_t command : {'C', 'M', 'P'}) {
        HardwareSerial::written = nullptr;
        TunerProtocol tuner;
        begin(tuner);
        HardwareSerial::written = [](HardwareSerial& serial, uint8_t sent) {
            if (sent == 'X') {
                serial.receiveError(UART_FRAME_ERROR);
                for (uint8_t b : frame(150, 3)) serial.bytes.push_back(b);
            }
            if (sent == 'C') serial.bytes.push_back('A');
            if (sent == 'M') serial.bytes.push_back('M');
            if (sent == 'P') serial.bytes.push_back('P');
        };
        TEST_ASSERT_TRUE(command == 'C' ? tuner.setAutoMode() :
                         command == 'M' ? tuner.setManualMode() : tuner.bypass());
        TEST_ASSERT_EQUAL_INT(command == 'C' ? MODE_AUTO :
                              command == 'M' ? MODE_MANUAL : MODE_BYPASS, tuner.getMode());
        JsonDocument status;
        ldg::writeTunerDiagnostics(status.to<JsonObject>(), tuner.getDiagnostics(), millis());
        const char sent[] = {char(command), '\0'};
        const char ack[] = {char(command == 'C' ? 'A' : command), '\0'};
        TEST_ASSERT_EQUAL_STRING(sent, status["last_command"]["command"].as<const char*>());
        TEST_ASSERT_EQUAL_STRING(ack, status["last_command"]["ack"].as<const char*>());
        TEST_ASSERT_TRUE(status["last_command"]["success"].as<bool>());
    }
}

static void commandStatus(JsonDocument& doc, TunerProtocol& tuner) {
    ldg::writeTunerDiagnostics(doc.to<JsonObject>(), tuner.getDiagnostics(), millis());
    std::string wire;
    serializeJson(doc, wire);
    TEST_ASSERT_FALSE(deserializeJson(doc, wire));
}

void test_command_trace_keeps_the_actual_reply_after_metering_overwrites_the_live_rx_tail() {
    TunerProtocol tuner;
    begin(tuner);
    HardwareSerial::written = [](HardwareSerial& serial, uint8_t command) {
        if (command == 'C') serial.bytes.push_back('A');
    };
    TEST_ASSERT_TRUE(tuner.setAutoMode());
    for (unsigned i = 0; i < 20; ++i) feed(tuner, frame());
    JsonDocument doc;
    commandStatus(doc, tuner);
    JsonObject command = doc["last_command"].as<JsonObject>();
    TEST_ASSERT_EQUAL_STRING("C", command["exchange"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("41", command["rx_hex"].as<const char*>());
    TEST_ASSERT_EQUAL_UINT(1, command["rx_bytes"].as<unsigned>());
    TEST_ASSERT_EQUAL_STRING("A", command["ack"].as<const char*>());
    TEST_ASSERT_TRUE(command["written"].as<bool>());
    TEST_ASSERT_TRUE(command["meter_restarted"].as<bool>());
    TEST_ASSERT_FALSE(command["uart_error"].as<bool>());
    TEST_ASSERT_TRUE(command["error"].isNull());
    TEST_ASSERT_EQUAL_UINT(64, strlen(doc["rx_tail_hex"].as<const char*>()));
}

void test_command_trace_distinguishes_no_reply_unexpected_reply_and_blocking_sync() {
    for (bool unexpected : {false, true}) {
        HardwareSerial::written = nullptr;
        TunerProtocol tuner;
        begin(tuner);
        HardwareSerial::written = [unexpected](HardwareSerial& serial, uint8_t command) {
            if (command == 'P' && unexpected) serial.bytes.push_back('p');
            if (command == 'Z') for (char b : std::string("000Az")) serial.bytes.push_back(b);
        };
        TEST_ASSERT_FALSE(tuner.bypass());
        JsonDocument doc;
        commandStatus(doc, tuner);
        TEST_ASSERT_EQUAL_STRING("P", doc["last_command"]["exchange"].as<const char*>());
        TEST_ASSERT_EQUAL_STRING("timeout", doc["last_command"]["error"].as<const char*>());
        TEST_ASSERT_EQUAL_STRING(unexpected ? "70" : "", doc["last_command"]["rx_hex"].as<const char*>());
        TEST_ASSERT_TRUE(doc["last_command"]["written"].as<bool>());
        TEST_ASSERT_FALSE(tuner.setAutoMode()); // C is blocked by an incomplete Z reply.
        commandStatus(doc, tuner);
        TEST_ASSERT_EQUAL_STRING("C", doc["last_command"]["command"].as<const char*>());
        TEST_ASSERT_EQUAL_STRING("Z", doc["last_command"]["exchange"].as<const char*>());
        TEST_ASSERT_EQUAL_STRING("303030417a", doc["last_command"]["rx_hex"].as<const char*>());
        TEST_ASSERT_EQUAL_STRING("timeout", doc["last_command"]["error"].as<const char*>());
    }
}

void test_command_trace_is_bounded_and_keeps_uart_faults_without_accepting_the_ack() {
    TunerProtocol tuner;
    begin(tuner);
    HardwareSerial::written = [](HardwareSerial& serial, uint8_t command) {
        if (command != 'P') return;
        serial.receiveError(UART_FRAME_ERROR);
        for (unsigned i = 0; i < 60; ++i) serial.bytes.push_back(i);
        serial.bytes.push_back('P');
    };
    TEST_ASSERT_FALSE(tuner.bypass());
    JsonDocument doc;
    commandStatus(doc, tuner);
    TEST_ASSERT_EQUAL_STRING("uart_error", doc["last_command"]["error"].as<const char*>());
    TEST_ASSERT_TRUE(doc["last_command"]["uart_error"].as<bool>());
    TEST_ASSERT_TRUE(doc["last_command"]["ack"].isNull());
    TEST_ASSERT_EQUAL_UINT(61, doc["last_command"]["rx_bytes"].as<unsigned>());
    const char* tail = doc["last_command"]["rx_hex"].as<const char*>();
    TEST_ASSERT_EQUAL_UINT(64, strlen(tail));
    TEST_ASSERT_EQUAL_STRING("1d1e1f202122232425262728292a2b2c2d2e2f303132333435363738393a3b50", tail);
    const auto frozen = tuner.getDiagnostics();
    feed(tuner, frame());
    TEST_ASSERT_EQUAL_MEMORY(frozen.commandRx, tuner.getDiagnostics().commandRx, sizeof(frozen.commandRx));
}

void test_command_trace_distinguishes_write_failure_and_meter_restart_failure() {
    for (uint8_t failed : {'X', 'P', 'S'}) {
        HardwareSerial::written = nullptr;
        TunerProtocol tuner;
        begin(tuner);
        HardwareSerial::latest->failCommand = failed;
        HardwareSerial::written = [](HardwareSerial& serial, uint8_t command) {
            if (command == 'P') serial.bytes.push_back('P');
        };
        TEST_ASSERT_FALSE(tuner.bypass());
        JsonDocument doc;
        commandStatus(doc, tuner);
        TEST_ASSERT_EQUAL_STRING(failed == 'X' ? "meter_stop_failed" :
                                 failed == 'P' ? "write_failed" : "meter_restart_failed",
                                 doc["last_command"]["error"].as<const char*>());
        TEST_ASSERT_EQUAL(failed == 'S', !doc["last_command"]["ack"].isNull());
        if (failed == 'X') TEST_ASSERT_TRUE(doc["last_command"]["exchange"].isNull());
        TEST_ASSERT_EQUAL(failed == 'S', doc["last_command"]["written"].as<bool>());
    }
}

void test_full_and_memory_tune_wait_for_complete_long_cycle() {
    for (uint8_t cmd : {'T', 'F'}) for (uint32_t duration : {2000u, 15000u, 20000u}) {
        TunerProtocol tuner;
        HardwareSerial::written = nullptr;
        begin(tuner);
        uint32_t sent = 0, ackAt = 0, meterAt = 0;
        HardwareSerial::written = [&](HardwareSerial& serial, uint8_t command) {
            if (command == cmd) {
                sent = millis(); ackAt = sent + duration;
                serial.schedule(duration, {'T'});
            }
            if (command == 'S') meterAt = millis();
        };
        TEST_ASSERT_TRUE(cmd == 'T' ? tuner.memoryTune() : tuner.fullTune());
        TEST_ASSERT_TRUE(uint32_t(meterAt - sent) >= duration);
        TEST_ASSERT_TRUE(int32_t(meterAt - ackAt) >= 200);
        TEST_ASSERT_TRUE(tuner.isConnected());
    }
}

void test_mode_commands_wait_for_delayed_ack_before_restarting_metering() {
    for (uint8_t cmd : {'P', 'C', 'M'}) for (uint32_t latency : {600u, 1500u, 2199u}) {
        HardwareSerial::written = nullptr;
        TunerProtocol tuner;
        begin(tuner);
        uint32_t sentAt = 0, restartedAt = 0;
        unsigned writes = 0;
        const uint8_t ack = cmd == 'C' ? 'A' : cmd;
        HardwareSerial::written = [&](HardwareSerial& serial, uint8_t command) {
            if (command == cmd) {
                ++writes;
                sentAt = millis();
                serial.schedule(latency, {ack});
            }
            if (command == 'S') restartedAt = millis();
        };
        TEST_ASSERT_TRUE(cmd == 'P' ? tuner.bypass() : cmd == 'C' ? tuner.setAutoMode() : tuner.setManualMode());
        TEST_ASSERT_EQUAL_UINT(1, writes);
        TEST_ASSERT_TRUE(uint32_t(restartedAt - sentAt) >= latency + 200);
        TEST_ASSERT_EQUAL_INT(cmd == 'P' ? MODE_BYPASS : cmd == 'C' ? MODE_AUTO : MODE_MANUAL, tuner.getMode());
        JsonDocument doc;
        commandStatus(doc, tuner);
        TEST_ASSERT_EQUAL_UINT(2200, doc["last_command"]["timeout_ms"].as<unsigned>());
        TEST_ASSERT_EQUAL_UINT(latency, doc["last_command"]["wait_ms"].as<unsigned>());
    }
}

void test_mode_timeout_remains_bounded_and_rejects_late_ack_across_clock_wrap() {
    for (uint32_t clock : {0u, 0xfffffc00u}) for (uint8_t cmd : {'P', 'C'}) {
        testMillis = clock;
        HardwareSerial::written = nullptr;
        TunerProtocol tuner;
        begin(tuner);
        unsigned writes = 0;
        HardwareSerial::written = [&](HardwareSerial& serial, uint8_t command) {
            if (command == cmd) {
                ++writes;
                serial.schedule(2201, {uint8_t(cmd == 'C' ? 'A' : 'P')});
            }
        };
        const uint32_t started = millis();
        TEST_ASSERT_FALSE(cmd == 'P' ? tuner.bypass() : tuner.setAutoMode());
        TEST_ASSERT_EQUAL_UINT(1, writes);
        TEST_ASSERT_LESS_THAN_UINT(3000, uint32_t(millis() - started));
        TEST_ASSERT_EQUAL_INT(MODE_UNKNOWN, tuner.getMode());
        JsonDocument doc;
        commandStatus(doc, tuner);
        TEST_ASSERT_EQUAL_STRING("timeout", doc["last_command"]["error"].as<const char*>());
        TEST_ASSERT_EQUAL_UINT(2200, doc["last_command"]["timeout_ms"].as<unsigned>());
        TEST_ASSERT_EQUAL_UINT(2200, doc["last_command"]["wait_ms"].as<unsigned>());
        TEST_ASSERT_EQUAL_UINT(1, doc["last_command"]["rx_bytes"].as<unsigned>());
        TEST_ASSERT_EQUAL_STRING(cmd == 'C' ? "A" : "P", doc["last_command"]["late_ack"].as<const char*>());
        TEST_ASSERT_TRUE(doc["last_command"]["ack"].isNull());
        TEST_ASSERT_FALSE(cmd == 'P' ? tuner.bypass() : tuner.setAutoMode());
        TEST_ASSERT_EQUAL_UINT(1, writes); // Recovery Z must succeed before any retry.
        commandStatus(doc, tuner);
        TEST_ASSERT_EQUAL_STRING("Z", doc["last_command"]["exchange"].as<const char*>());
        TEST_ASSERT_EQUAL_UINT(2200, doc["last_command"]["timeout_ms"].as<unsigned>());
    }
}

void test_post_ack_guard_includes_wake_and_survives_clock_wrap() {
    for (uint32_t clock : {0u, 0xfffffc00u}) {
        testMillis = clock;
        HardwareSerial::written = nullptr;
        TunerProtocol tuner;
        begin(tuner);
        uint32_t ackAt = 0, firstWakeAt = 0, meterAt = 0;
        bool commandSent = false, wakeSeen = false;
        HardwareSerial::written = [&](HardwareSerial& serial, uint8_t command) {
            if (command == 'C') {
                commandSent = true;
                ackAt = millis() + 300;
                serial.schedule(300, {'A'});
            } else if (commandSent && command == ' ' && !wakeSeen) {
                firstWakeAt = millis(); wakeSeen = true;
            } else if (command == 'S') meterAt = millis();
        };
        TEST_ASSERT_TRUE(tuner.setAutoMode());
        TEST_ASSERT_TRUE(wakeSeen);
        TEST_ASSERT_TRUE(int32_t(firstWakeAt - ackAt) >= 200);
        TEST_ASSERT_TRUE(int32_t(meterAt - ackAt) >= 200);
    }
}

void test_sync_requires_complete_signature_and_rejects_wrong_case() {
    for (const std::string& reply : {std::string("0"), std::string("Z"), std::string("Q"),
            std::string("000000000000000AzA"), std::string("000000000000000AzAZ"), syncReply}) {
        HardwareSerial::written = nullptr;
        TunerProtocol tuner;
        begin(tuner);
        HardwareSerial::written = [reply](HardwareSerial& serial, uint8_t command) {
            if (command == 'Z') for (char byte : reply) serial.bytes.push_back(byte);
        };
        TEST_ASSERT_EQUAL(reply == syncReply, tuner.sync());
        TEST_ASSERT_EQUAL(reply == syncReply, tuner.isConnected());
    }
}

void test_sync_streamed_and_fragmented_replies_finish_before_meter_restart() {
    for (size_t split = 0; split <= syncReply.size(); ++split) {
        HardwareSerial::written = nullptr;
        TunerProtocol tuner;
        begin(tuner);
        uint32_t lastByteAt = 0, meterAt = 0;
        HardwareSerial::written = [&](HardwareSerial& serial, uint8_t command) {
            if (command == 'Z') {
                serial.schedule(0, {syncReply.begin(), syncReply.begin() + split});
                serial.schedule(300, {syncReply.begin() + split, syncReply.end()}, 20);
                lastByteAt = millis() + (split == syncReply.size() ? 0 : 300 + 20 * (syncReply.size() - split - 1));
            }
            if (command == 'S') meterAt = millis();
        };
        TEST_ASSERT_TRUE(tuner.sync());
        TEST_ASSERT_TRUE(int32_t(meterAt - lastByteAt) >= 200);
        TEST_ASSERT_TRUE(tuner.isConnected());
    }
}

void test_sync_finds_signature_after_noise_and_overlapping_zero_prefix() {
    TunerProtocol tuner;
    begin(tuner);
    HardwareSerial::written = [](HardwareSerial& serial, uint8_t command) {
        if (command != 'Z') return;
        const std::string damaged = "Q\n000000000000000AzAQ00000000000000000000AzAz";
        serial.schedule(20, {damaged.begin(), damaged.end()}, 2);
    };
    TEST_ASSERT_TRUE(tuner.sync());
    TEST_ASSERT_TRUE(tuner.isConnected());
}

void test_unknown_responses_never_establish_connection() {
    using Command = bool (TunerProtocol::*)();
    const Command commands[] = {&TunerProtocol::toggleAntenna, &TunerProtocol::setAutoMode,
        &TunerProtocol::setManualMode, &TunerProtocol::bypass, &TunerProtocol::memoryTune, &TunerProtocol::fullTune};
    for (Command command : commands) {
        HardwareSerial::written = nullptr;
        TunerProtocol tuner;
        begin(tuner);
        HardwareSerial::written = [](HardwareSerial& serial, uint8_t byte) {
            if (byte != ' ' && byte != 'X' && byte != 'S') serial.bytes.push_back('Q');
        };
        TEST_ASSERT_FALSE((tuner.*command)());
        TEST_ASSERT_FALSE(tuner.isConnected());
    }
}

void test_noise_before_expected_ack_is_ignored_but_failed_tune_proves_communication() {
    TunerProtocol tuner;
    begin(tuner);
    HardwareSerial::written = [](HardwareSerial& serial, uint8_t command) {
        if (command == 'C') serial.schedule(300, {0, '\r', '\n', 'Q', 'P', 'A'});
        if (command == 'F') serial.schedule(2000, {'F'});
    };
    TEST_ASSERT_TRUE(tuner.setAutoMode());
    TEST_ASSERT_EQUAL_INT(MODE_AUTO, tuner.getMode());
    TEST_ASSERT_FALSE(tuner.fullTune());
    TEST_ASSERT_TRUE(tuner.isConnected());
}

void test_timeout_requires_sync_before_next_command_and_discards_late_ack() {
    TunerProtocol tuner;
    begin(tuner);
    uint32_t oldSent = 0, newSent = 0, newAck = 0, syncEnd = 0;
    unsigned syncs = 0;
    HardwareSerial::written = [&](HardwareSerial& serial, uint8_t command) {
        if (command == 'F') {
            oldSent = millis();
            // Arrives during recovery Z, after the second X drained the UART.
            serial.schedule(20750, {'T'});
        }
        if (command == 'Z') {
            ++syncs;
            syncEnd = millis() + 500 + syncReply.size() - 1;
            serial.schedule(500, {syncReply.begin(), syncReply.end()}, 1);
        }
        if (command == 'T') {
            newSent = millis(); newAck = newSent + 300;
            serial.schedule(300, {'M'});
        }
    };
    TEST_ASSERT_FALSE(tuner.fullTune());
    TEST_ASSERT_TRUE(uint32_t(millis() - oldSent) >= 20000);
    TEST_ASSERT_FALSE(tuner.isConnected());
    TEST_ASSERT_TRUE(tuner.memoryTune());
    TEST_ASSERT_EQUAL_UINT(1, syncs);
    TEST_ASSERT_TRUE(int32_t(newSent - syncEnd) >= 200);
    TEST_ASSERT_TRUE(int32_t(millis() - newAck) >= 0);
    TEST_ASSERT_TRUE(tuner.isConnected());
}

void test_failed_sync_blocks_new_command_until_explicit_recovery() {
    TunerProtocol tuner;
    begin(tuner);
    unsigned tunes = 0, syncs = 0;
    bool allowSync = false;
    HardwareSerial::written = [&](HardwareSerial& serial, uint8_t command) {
        if (command == 'F') ++tunes; // Never answers: establish ambiguous state.
        if (command == 'Z') { ++syncs; if (allowSync) respondSync(serial, command); }
        if (command == 'M') serial.bytes.push_back('M');
    };
    TEST_ASSERT_FALSE(tuner.fullTune());
    TEST_ASSERT_FALSE(tuner.fullTune());
    TEST_ASSERT_EQUAL_UINT(1, tunes);
    TEST_ASSERT_EQUAL_UINT(1, syncs);
    allowSync = true;
    TEST_ASSERT_TRUE(tuner.sync());
    TEST_ASSERT_TRUE(tuner.setManualMode());
    TEST_ASSERT_EQUAL_UINT(2, syncs);
    TEST_ASSERT_EQUAL_INT(MODE_MANUAL, tuner.getMode());
}

void test_failure_to_restore_metering_is_reported_and_uncertain_mode_is_cleared() {
    TunerProtocol tuner;
    begin(tuner);
    HardwareSerial::written = [](HardwareSerial& serial, uint8_t command) {
        respondSync(serial, command);
        if (command == 'C') serial.bytes.push_back('A');
        if (command == 'P') serial.bytes.push_back('P');
        if (command == 'F') serial.bytes.push_back('T');
    };
    TEST_ASSERT_TRUE(tuner.setAutoMode());
    TEST_ASSERT_EQUAL_INT(MODE_AUTO, tuner.getMode());
    TEST_ASSERT_FALSE(tuner.setManualMode()); // Sent M, but acknowledgement was lost.
    TEST_ASSERT_EQUAL_INT(MODE_UNKNOWN, tuner.getMode());
    TEST_ASSERT_TRUE(tuner.bypass());
    TEST_ASSERT_TRUE(tuner.fullTune());
    TEST_ASSERT_EQUAL_INT(MODE_UNKNOWN, tuner.getMode());
    HardwareSerial::latest->failCommand = 'S';
    TEST_ASSERT_FALSE(tuner.setAutoMode());
}

void test_cached_antenna_does_not_skip_recovery_after_meter_restart_failure() {
    TunerProtocol tuner;
    begin(tuner);
    unsigned toggles = 0, syncs = 0;
    HardwareSerial::written = [&](HardwareSerial& serial, uint8_t command) {
        if (command == 'Z') { ++syncs; respondSync(serial, command); }
        if (command == 'A') serial.bytes.push_back(++toggles % 2 ? '2' : '1');
    };
    HardwareSerial::latest->failCommand = 'S';
    TEST_ASSERT_FALSE(tuner.selectAntenna(ANT_B));
    TEST_ASSERT_EQUAL_INT(ANT_B, tuner.getAntenna());
    HardwareSerial::latest->failCommand = -1;
    TEST_ASSERT_TRUE(tuner.selectAntenna(ANT_B));
    TEST_ASSERT_EQUAL_UINT(1, syncs);
    TEST_ASSERT_EQUAL_UINT(1, toggles);
    TEST_ASSERT_EQUAL_INT(ANT_B, tuner.getAntenna());
}

void test_meter_all_chunk_partitions_and_consecutive_frames() {
    TunerProtocol tuner;
    begin(tuner);
    const auto bytes = frame();
    for (unsigned mask = 0; mask < 128; ++mask) {
        received.clear();
        for (unsigned i = 0; i < bytes.size(); ++i) {
            HardwareSerial::latest->bytes.push_back(bytes[i]);
            if (i == 7 || (mask & (1u << i))) tuner.process();
            if (i < 7) TEST_ASSERT_TRUE(received.empty());
        }
        TEST_ASSERT_EQUAL_UINT(1, received.size());
        TEST_ASSERT_EQUAL_UINT(375, received.back().forward_power_raw);
        TEST_ASSERT_EQUAL_UINT(75, received.back().reflected_power_raw);
        TEST_ASSERT_EQUAL_UINT(1160, received.back().band_indicator);
        TEST_ASSERT_EQUAL_INT(BAND_20M, received.back().band);
    }
    received.clear();
    std::vector<uint8_t> stream;
    for (unsigned i = 0; i < 10; ++i) {
        auto bytes = frame(100 + i, 10);
        stream.insert(stream.end(), bytes.begin(), bytes.end());
    }
    feed(tuner, stream);
    TEST_ASSERT_EQUAL_UINT(10, received.size());
    TEST_ASSERT_EQUAL_UINT(109, received.back().forward_power_raw);
}

void test_meter_resynchronizes_after_single_byte_corruption() {
    for (unsigned insert = 0; insert < 2; ++insert) for (unsigned pos = 0; pos < 8 + insert; ++pos) {
        TunerProtocol tuner;
        begin(tuner);
        auto damaged = frame();
        if (insert) damaged.insert(damaged.begin() + pos, 0x55);
        else damaged.erase(damaged.begin() + pos);
        for (int i = 0; i < 4; ++i) {
            auto good = frame(999, 30);
            damaged.insert(damaged.end(), good.begin(), good.end());
        }
        feed(tuner, damaged);
        TEST_ASSERT_EQUAL_UINT(999, tuner.getState().meter.forward_power_raw);
        TEST_ASSERT_EQUAL_UINT(30, tuner.getState().meter.reflected_power_raw);
    }
}

void test_meter_payload_semicolons_and_band_boundaries() {
    TunerProtocol tuner;
    begin(tuner);
    feed(tuner, frame(0x3b3b, 0x003b, 0x3b3b));
    TEST_ASSERT_EQUAL_UINT(1, received.size());
    TEST_ASSERT_EQUAL_UINT(0x3b3b, received.back().forward_power_raw);
    TEST_ASSERT_EQUAL_UINT(0x003b, received.back().reflected_power_raw);
    const uint16_t limits[][2] = {{8230,9145}, {4110,4710}, {3060,3080}, {2250,2355},
        {1600,1630}, {1140,1180}, {900,915}, {766,782}, {655,662}, {550,590}, {320,360}};
    for (unsigned band = 0; band < 11; ++band) for (int edge = 0; edge < 4; ++edge) {
        const uint16_t indicator = edge == 0 ? limits[band][0] - 1 : edge == 1 ? limits[band][0] :
            edge == 2 ? limits[band][1] : limits[band][1] + 1;
        feed(tuner, frame(375, 75, indicator));
        const tuner_band_t expected = edge == 1 || edge == 2
            ? static_cast<tuner_band_t>(band + 1) : BAND_UNKNOWN;
        TEST_ASSERT_EQUAL_INT(expected, tuner.getState().meter.band);
    }
}

void test_swr_valid_measurements_and_invalid_rf_stay_unknown_in_json() {
    TunerProtocol tuner;
    begin(tuner);
    feed(tuner, frame());
    TEST_ASSERT_DOUBLE_WITHIN(0.000001, 249.489352, tuner.getState().meter.forward_power_watts);
    TEST_ASSERT_DOUBLE_WITHIN(0.000001, 9.979574081, tuner.getState().meter.reflected_power_watts);
    TEST_ASSERT_DOUBLE_WITHIN(0.000001, 1.5, tuner.getState().meter.swr);
    feed(tuner, frame(100, 0));
    TEST_ASSERT_EQUAL_DOUBLE(1.0, tuner.getState().meter.swr);
    for (auto raw : {std::pair<uint16_t,uint16_t>{0,0}, {0,100}, {1,100}, {100,100}}) {
        feed(tuner, frame(raw.first, raw.second));
        const tuner_state_t state = tuner.getState();
        TEST_ASSERT_TRUE(state.meterValid); // Power readings remain useful.
        TEST_ASSERT_TRUE(std::isnan(state.meter.swr));
        JsonDocument written, parsed;
        ldg::writeState(written, state, false);
        std::string wire;
        serializeJson(written, wire);
        TEST_ASSERT_FALSE(deserializeJson(parsed, wire));
        TEST_ASSERT_TRUE(parsed["swr"].isNull());
        const auto restored = ldg::readWebState(parsed.as<JsonVariantConst>());
        TEST_ASSERT_TRUE(restored.meterValid);
        TEST_ASSERT_TRUE(std::isnan(restored.meter.swr));
    }
}

void test_antenna_startup_probe_restores_both_ports_and_respects_ack_guard() {
    for (uint8_t initial : {1, 2}) {
        TunerProtocol tuner; begin(tuner);
        uint8_t selected = initial;
        unsigned toggles = 0;
        std::vector<uint32_t> at;
        HardwareSerial::written = [&](HardwareSerial& serial, uint8_t cmd) {
            if (cmd == 'A') {
                selected = selected == 1 ? 2 : 1; ++toggles; at.push_back(millis());
                serial.bytes.push_back(selected == 1 ? '1' : '2');
            }
        };
        tuner.serviceAntennas();
        TEST_ASSERT_EQUAL_UINT(0, toggles); // Missing telemetry is not zero RF.
        feed(tuner, frame(150, 0, 2328)); tuner.serviceAntennas();
        TEST_ASSERT_EQUAL_UINT(0, toggles); // Even 40 W must not start the probe.
        feed(tuner, frame(0, 0, 2328)); testMillis += 501; tuner.serviceAntennas();
        TEST_ASSERT_EQUAL_UINT(0, toggles);
        feed(tuner, frame(0, 0, 2328)); tuner.serviceAntennas();
        TEST_ASSERT_EQUAL_UINT(2, toggles);
        TEST_ASSERT_EQUAL_UINT(initial, selected);
        TEST_ASSERT_TRUE(tuner.getState().antennaInit == ldg::AntennaInit::Ready);
        TEST_ASSERT_TRUE(at[1] - at[0] >= 200);
        for (int i = 0; i < 5; ++i) { feed(tuner, frame(0, 0, 2328)); tuner.serviceAntennas(); }
        TEST_ASSERT_EQUAL_UINT(2, toggles);
    }
}
void test_startup_missing_either_ack_never_blindly_retries_and_manual_selection_recovers() {
    for (unsigned missing : {1u, 2u}) {
        TunerProtocol tuner; begin(tuner);
        unsigned toggles = 0;
        HardwareSerial::written = [&](HardwareSerial& serial, uint8_t cmd) {
            respondSync(serial, cmd);
            if (cmd == 'A' && ++toggles != missing) serial.bytes.push_back(toggles % 2 ? '2' : '1');
        };
        feed(tuner, frame(0, 0, 2328)); tuner.serviceAntennas();
        TEST_ASSERT_EQUAL_UINT(missing, toggles);
        TEST_ASSERT_TRUE(tuner.getState().antennaInit == ldg::AntennaInit::Error);
        for (int i = 0; i < 5; ++i) { feed(tuner, frame(0, 0, 2328)); tuner.serviceAntennas(); }
        TEST_ASSERT_EQUAL_UINT(missing, toggles);
        tuner.noteManualCommand(); tuner.manualCommandStarted();
        const bool success = tuner.selectAntenna(ANT_B);
        tuner.manualCommandComplete(TUNER_CMD_SELECT_ANT_B, success);
        TEST_ASSERT_TRUE(success);
        TEST_ASSERT_TRUE(tuner.getState().antennaInit == ldg::AntennaInit::Ready);
    }
}
void test_real_wire_band_selection_is_one_shot_and_never_restores_a_manual_choice() {
    TunerProtocol tuner; begin(tuner);
    ldg::AntennaSettings settings;
    settings.enabled = true; settings.bands[BAND_40M] = ANT_A; settings.bands[BAND_20M] = ANT_B;
    tuner.configureAntennas(settings);
    unsigned toggles = 0; uint8_t selected = 1;
    HardwareSerial::written = [&](HardwareSerial& serial, uint8_t cmd) {
        respondSync(serial, cmd);
        if (cmd == 'M') serial.bytes.push_back('M');
        if (cmd == 'A') {
            ++toggles; selected = selected == 1 ? 2 : 1;
            serial.bytes.push_back(selected == 1 ? '1' : '2');
        }
    };
    TEST_ASSERT_TRUE(tuner.setManualMode());
    feed(tuner, frame(0, 0, 2328)); tuner.serviceAntennas();
    TEST_ASSERT_EQUAL_UINT(2, toggles);
    for (int i = 0; i < 3; ++i) feed(tuner, frame(150, 0, 2328));
    tuner.serviceAntennas();
    TEST_ASSERT_EQUAL_UINT(2, toggles);
    for (int i = 0; i < 3; ++i) feed(tuner, frame(150, 0, 1160));
    tuner.serviceAntennas();
    TEST_ASSERT_EQUAL_UINT(3, toggles); TEST_ASSERT_EQUAL_UINT(2, selected);
    tuner.noteManualCommand(); tuner.manualCommandStarted();
    tuner.manualCommandComplete(TUNER_CMD_SELECT_ANT_A, tuner.selectAntenna(ANT_A));
    TEST_ASSERT_EQUAL_UINT(4, toggles);
    testMillis += 10000;
    feed(tuner, frame(0, 0, 0));
    for (int i = 0; i < 3; ++i) feed(tuner, frame(150, 0, 1160));
    tuner.serviceAntennas();
    TEST_ASSERT_EQUAL_UINT(4, toggles); TEST_ASSERT_EQUAL_UINT(1, selected);
    // High power consumes the next change, even after power falls again.
    for (int i = 0; i < 3; ++i) feed(tuner, frame(375, 0, 2328));
    tuner.serviceAntennas();
    TEST_ASSERT_TRUE(tuner.getState().antennaAuto == ldg::AntennaAuto::HighPower);
    for (int i = 0; i < 3; ++i) feed(tuner, frame(150, 0, 2328));
    tuner.serviceAntennas(); TEST_ASSERT_EQUAL_UINT(4, toggles);
    for (int i = 0; i < 3; ++i) feed(tuner, frame(150, 0, 1160));
    tuner.serviceAntennas(); TEST_ASSERT_EQUAL_UINT(5, toggles);
    JsonDocument status; ldg::writeState(status, tuner.getState(), false);
    TEST_ASSERT_EQUAL_STRING("ready", status["antenna_init"].as<const char*>());
    const auto restored = ldg::readWebState(status.as<JsonVariantConst>());
    TEST_ASSERT_TRUE(restored.antennaInit == ldg::AntennaInit::Ready);
    TEST_ASSERT_TRUE(restored.antennaAuto == ldg::AntennaAuto::Selected);
}

void test_antenna_and_recovery_sync_accept_delayed_replies_without_repeating_a_toggle() {
    TunerProtocol tuner; begin(tuner);
    unsigned toggles = 0;
    HardwareSerial::written = [&](HardwareSerial& serial, uint8_t cmd) {
        if (cmd == 'C') serial.bytes.push_back('A');
        if (cmd == 'A') { ++toggles; serial.schedule(800, {'2'}); }
    };
    TEST_ASSERT_TRUE(tuner.setAutoMode());
    TEST_ASSERT_TRUE(tuner.selectAntenna(ANT_B));
    TEST_ASSERT_EQUAL_UINT(1, toggles);
    TEST_ASSERT_EQUAL_INT(ANT_B, tuner.getAntenna());
    TEST_ASSERT_TRUE(tuner.selectAntenna(ANT_B));
    TEST_ASSERT_EQUAL_UINT(1, toggles);
    TEST_ASSERT_EQUAL_UINT(800, tuner.getDiagnostics().responseWaitMs);
    // A genuinely missing response requires Z. Its entire signature may also
    // take longer than the old one-second response window.
    HardwareSerial::written = nullptr;
    TEST_ASSERT_FALSE(tuner.setManualMode());
    HardwareSerial::written = [&](HardwareSerial& serial, uint8_t cmd) {
        if (cmd == 'Z') serial.schedule(1500, {syncReply.begin(), syncReply.end()});
        if (cmd == 'A') { ++toggles; serial.schedule(800, {'1'}); }
    };
    TEST_ASSERT_TRUE(tuner.selectAntenna(ANT_A));
    TEST_ASSERT_EQUAL_UINT(2, toggles);
}

void test_antenna_timeout_preserves_last_port_without_claiming_current_confirmation() {
    TunerProtocol tuner; begin(tuner);
    HardwareSerial::written = [](HardwareSerial& serial, uint8_t cmd) {
        if (cmd == 'A') serial.bytes.push_back('2');
    };
    TEST_ASSERT_TRUE(tuner.selectAntenna(ANT_B));
    HardwareSerial::written = nullptr;
    TEST_ASSERT_FALSE(tuner.selectAntenna(ANT_A));
    auto state = tuner.getState();
    TEST_ASSERT_EQUAL_INT(ANT_UNKNOWN, state.antenna);
    TEST_ASSERT_EQUAL_INT(ANT_B, state.lastAntenna);
    JsonDocument doc; ldg::writeState(doc, state, false);
    TEST_ASSERT_FALSE(doc["antenna_confirmed"].as<bool>());
    TEST_ASSERT_EQUAL_INT(ANT_B, doc["antenna_last"].as<int>());
    state = ldg::readWebState(doc.as<JsonVariantConst>());
    TEST_ASSERT_EQUAL_INT(ANT_UNKNOWN, state.antenna);
    TEST_ASSERT_EQUAL_INT(ANT_B, state.lastAntenna);
    // Still uncertain: selecting the last known port must not report success
    // or blindly toggle after the recovery signature is missing.
    TEST_ASSERT_FALSE(tuner.selectAntenna(ANT_B));
    TEST_ASSERT_EQUAL_INT(ANT_B, tuner.getState().lastAntenna);
    TEST_ASSERT_EQUAL_INT('Z', tuner.getDiagnostics().exchange);
}

void test_failed_recovery_before_antenna_command_preserves_the_confirmed_port() {
    TunerProtocol tuner; begin(tuner);
    unsigned toggles = 0;
    HardwareSerial::written = [&](HardwareSerial& serial, uint8_t cmd) {
        if (cmd == 'A') { ++toggles; serial.bytes.push_back('2'); }
    };
    TEST_ASSERT_TRUE(tuner.selectAntenna(ANT_B));
    TEST_ASSERT_FALSE(tuner.setAutoMode());
    TEST_ASSERT_FALSE(tuner.selectAntenna(ANT_A)); // Fails in Z; A never sent.
    TEST_ASSERT_EQUAL_UINT(1, toggles);
    TEST_ASSERT_EQUAL_INT(ANT_B, tuner.getAntenna());
    TEST_ASSERT_EQUAL_INT('Z', tuner.getDiagnostics().exchange);
}

void test_just_late_antenna_reply_is_quarantined_and_guards_meter_restart() {
    for (uint32_t clock : {0u, 0xffffff00u}) {
        testMillis = clock; HardwareSerial::written = nullptr;
        TunerProtocol tuner; begin(tuner);
        uint32_t ackAt = 0, wakeAt = 0, meterAt = 0;
        bool sent = false, woke = false;
        unsigned toggles = 0;
        HardwareSerial::written = [&](HardwareSerial& serial, uint8_t cmd) {
            if (cmd == 'A') {
                ++toggles; sent = true; ackAt = millis() + 2201;
                serial.schedule(2201, {'2'});
            } else if (sent && cmd == ' ' && !woke) { wakeAt = millis(); woke = true; }
            else if (cmd == 'S') meterAt = millis();
        };
        TEST_ASSERT_FALSE(tuner.selectAntenna(ANT_B));
        TEST_ASSERT_EQUAL_UINT(1, toggles);
        TEST_ASSERT_TRUE(woke);
        TEST_ASSERT_TRUE(int32_t(wakeAt - ackAt) >= 200);
        TEST_ASSERT_TRUE(int32_t(meterAt - ackAt) >= 200);
        const auto diagnostics = tuner.getDiagnostics();
        TEST_ASSERT_EQUAL_UINT(2200, diagnostics.responseWaitMs);
        TEST_ASSERT_EQUAL_INT('2', diagnostics.lateResponse);
        TEST_ASSERT_FALSE(diagnostics.commandSuccess);
        TEST_ASSERT_EQUAL_INT(ANT_B, tuner.getAntenna());
        feed(tuner, frame(0, 0, 2328));
        TEST_ASSERT_EQUAL_UINT(0, tuner.getDiagnostics().framingErrors);
    }
}

void test_auto_band_selection_guards_mode_and_restores_it_even_after_failures() {
    for (int scenario = 0; scenario < 14; ++scenario) {
        HardwareSerial::written = nullptr;
        TunerProtocol tuner; begin(tuner);
        ldg::AntennaSettings settings;
        settings.enabled = true; settings.bands[BAND_40M] = ANT_A; settings.bands[BAND_20M] = ANT_B;
        tuner.configureAntennas(settings);
        unsigned toggles = 0, manuals = 0, restores = 0; uint8_t selected = 1;
        bool operation = false, manual = false;
        std::string wire;
        HardwareSerial::written = [&](HardwareSerial& serial, uint8_t cmd) {
            respondSync(serial, cmd);
            if (cmd == 'M' || cmd == 'A' || cmd == 'T' || cmd == 'C') wire += char(cmd);
            if (cmd == 'T') serial.bytes.push_back('T');
            if (cmd == 'M') {
                manual = true;
                ++manuals;
                if (scenario != 1 && !(scenario == 12 && manuals == 1)) serial.schedule(10, {'M'});
            }
            if (cmd == 'C') {
                manual = false;
                if (operation) ++restores;
                if (!operation || (scenario != 3 && !(scenario == 13 && restores == 1))) serial.bytes.push_back('A');
            }
            if (cmd == 'A') {
                // The tuner rejects RF antenna selection in AUTO.
                TEST_ASSERT_TRUE(!operation || manual);
                ++toggles;
                const bool firstLost = operation && toggles == 3 && scenario >= 8 && scenario <= 10;
                if (!(firstLost && scenario == 9)) selected = selected == 1 ? 2 : 1;
                if ((!operation || scenario != 2) && !firstLost) serial.bytes.push_back(selected == 1 ? '1' : '2');
            }
            if (cmd == 'S' && operation && manual && scenario != 5) {
                const auto bytes = frame(scenario == 4 || (scenario == 10 && toggles >= 3) ? 375 : 150, 0, scenario == 6 ? 2328 : 1160);
                if (scenario == 11) serial.schedule(700, bytes);
                else for (auto b : bytes) serial.bytes.push_back(b);
                if (scenario == 7) {
                    auto damaged = frame(375, 0, 1160); damaged.back() = 0;
                    for (auto b : damaged) serial.bytes.push_back(b);
                }
            }
        };
        feed(tuner, frame(0, 0, 2328)); tuner.serviceAntennas();
        TEST_ASSERT_EQUAL_UINT(2, toggles);
        TEST_ASSERT_TRUE(tuner.setAutoMode());
        for (int i = 0; i < 3; ++i) feed(tuner, frame(150, 0, 2328));
        tuner.serviceAntennas();
        for (int i = 0; i < 3; ++i) feed(tuner, frame(150, 0, 1160));
        operation = true; wire.clear();
        tuner.serviceAntennas();
        const auto d = tuner.getDiagnostics().antennaSelection;
        TEST_ASSERT_TRUE(d.present && d.modeGuard);
        TEST_ASSERT_EQUAL_INT(ANT_B, d.target);
        TEST_ASSERT_EQUAL(scenario != 1, d.manualConfirmed);
        TEST_ASSERT_EQUAL(scenario == 0 || scenario == 2 || scenario == 3 || scenario >= 8, d.selectionAttempted);
        TEST_ASSERT_EQUAL(scenario == 0 || scenario == 3 || scenario == 8 || scenario == 9 || scenario >= 11, d.selectionConfirmed);
        TEST_ASSERT_EQUAL(scenario != 1 && scenario != 3, d.autoRestored);
        TEST_ASSERT_EQUAL(scenario == 1, d.deferred);
        std::string expected = scenario == 1 ? "MM" : scenario == 3 || scenario == 13 ? "MACC" :
            scenario == 12 ? "MMAC" : scenario == 8 ? "MAAAC" : (scenario == 2 || scenario == 9) ? "MAAC" : d.selectionAttempted ? "MAC" : "MC";
        if (d.selectionConfirmed) expected.insert(expected.find('C'), "T");
        TEST_ASSERT_EQUAL_STRING(expected.c_str(), wire.c_str());
        TEST_ASSERT_EQUAL_UINT((scenario == 2 || scenario == 8 || scenario == 9) ? 2 : d.selectionAttempted ? 1 : 0, d.attempts);
        TEST_ASSERT_EQUAL_UINT(scenario == 1 || scenario == 12 ? 2 : 1, d.manualAttempts);
        TEST_ASSERT_EQUAL_UINT(scenario == 1 ? 0 : scenario == 3 || scenario == 13 ? 2 : 1, d.restoreAttempts);
        TEST_ASSERT_EQUAL_INT(scenario == 1 || scenario == 3 ? MODE_UNKNOWN : MODE_AUTO, tuner.getMode());
        if (scenario == 0 || scenario == 8 || scenario == 9 || scenario >= 11) TEST_ASSERT_TRUE(tuner.getState().antennaAuto == ldg::AntennaAuto::Selected);
        else if (scenario == 4 || scenario == 10) TEST_ASSERT_TRUE(tuner.getState().antennaAuto == ldg::AntennaAuto::HighPower);
        else if (scenario == 5 || scenario == 7) TEST_ASSERT_TRUE(tuner.getState().antennaAuto == ldg::AntennaAuto::InvalidPower);
        else if (scenario == 1) TEST_ASSERT_TRUE(tuner.getState().antennaAuto == ldg::AntennaAuto::WaitingTuner);
        else TEST_ASSERT_TRUE(tuner.getState().antennaAuto == ldg::AntennaAuto::Error);
        const unsigned done = toggles;
        feed(tuner, frame(150, 0, 1160)); tuner.serviceAntennas();
        TEST_ASSERT_EQUAL_UINT(done, toggles); // Neither failed toggles nor mode changes repeat.
        JsonDocument status;
        ldg::writeTunerDiagnostics(status.to<JsonObject>(), tuner.getDiagnostics(), millis());
        TEST_ASSERT_EQUAL(d.selectionConfirmed, status["antenna_selection"]["confirmed"].as<bool>());
        TEST_ASSERT_EQUAL(d.autoRestored, status["antenna_selection"]["auto_restored"].as<bool>());
    }
}
void test_failed_old_auto_tune_keeps_first_band_choice_and_recovers_without_a_successful_match() {
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        HardwareSerial::written = nullptr;
        TunerProtocol tuner; begin(tuner);
        ldg::AntennaSettings settings;
        settings.enabled = true; settings.bands[BAND_40M] = ANT_A; settings.bands[BAND_20M] = ANT_B;
        tuner.configureAntennas(settings);
        bool operation = false, busy = false, manualConfirmed = false, rf = true;
        unsigned toggles = 0, tunes = 0, manuals = 0, autos = 0;
        HardwareSerial::written = [&](HardwareSerial& serial, uint8_t cmd) {
            if (!busy) respondSync(serial, cmd);
            if (cmd == 'M') {
                if (operation) ++manuals;
                if (!busy) { manualConfirmed = true; serial.bytes.push_back('M'); }
                else serial.bytes.push_back('F'); // Failure of the old tune is not a MANUAL ACK.
            }
            if (cmd == 'C') {
                if (operation) ++autos;
                if (!busy) { manualConfirmed = false; serial.bytes.push_back('A'); }
            }
            if (cmd == 'A') {
                TEST_ASSERT_TRUE(!operation || manualConfirmed);
                ++toggles; serial.bytes.push_back(toggles % 2 ? '2' : '1');
            }
            if (cmd == 'T') { TEST_ASSERT_TRUE(manualConfirmed); ++tunes; serial.bytes.push_back('T'); }
            if (cmd == 'S' && operation && !busy)
                for (auto b : frame(rf ? 150 : 0, 0, 1160)) serial.bytes.push_back(b);
        };
        TEST_ASSERT_TRUE(tuner.setAutoMode());
        feed(tuner, frame(0, 0, 2328)); tuner.serviceAntennas();
        TEST_ASSERT_EQUAL_UINT(2, toggles);
        operation = busy = true;
        const uint32_t transitionAt = millis();
        for (int i = 0; i < 3; ++i) feed(tuner, frame(150, 0, 1160));
        tuner.serviceAntennas();
        TEST_ASSERT_TRUE(tuner.getState().antennaAuto == ldg::AntennaAuto::WaitingTuner);
        TEST_ASSERT_EQUAL_UINT(2, toggles); TEST_ASSERT_EQUAL_UINT(0, tunes);
        TEST_ASSERT_EQUAL_UINT(0, autos); // Do not restart AUTO on the old antenna.
        TEST_ASSERT_TRUE(tuner.getDiagnostics().antennaSelection.deferred);
        JsonDocument state, diagnostics;
        ldg::writeState(state, tuner.getState(), false);
        TEST_ASSERT_TRUE(ldg::readWebState(state.as<JsonVariantConst>()).antennaAuto == ldg::AntennaAuto::WaitingTuner);
        ldg::writeTunerDiagnostics(diagnostics.to<JsonObject>(), tuner.getDiagnostics(), millis());
        TEST_ASSERT_TRUE(diagnostics["antenna_selection"]["deferred"].as<bool>());
        TEST_ASSERT_EQUAL_UINT(0, diagnostics["antenna_selection"]["recovery"].as<unsigned>());
        const unsigned attemptedManuals = manuals;
        if (scenario == 0 || scenario == 2 || scenario == 7) {
            testMillis = transitionAt + ldg::AntennaAutomation::TunerWaitMs - 1;
            feed(tuner, frame(150, 0, 1160)); tuner.serviceAntennas();
            TEST_ASSERT_EQUAL_UINT(attemptedManuals, manuals);
            ++testMillis;
            if (scenario == 0) busy = false; // Old tuning failed, but the tuner is responsive again.
            tuner.serviceAntennas();
            if (scenario == 2 || scenario == 7) {
                TEST_ASSERT_TRUE(tuner.getDiagnostics().antennaSelection.deferred);
                TEST_ASSERT_EQUAL_UINT(1, tuner.getDiagnostics().antennaSelection.recovery);
                testMillis += 100000;
                feed(tuner, frame(150, 0, 1160)); tuner.serviceAntennas();
                TEST_ASSERT_EQUAL_UINT(2, toggles); // No endless M/A retries during RF.
                busy = scenario == 7; rf = false;
                feed(tuner, frame(0, 0, 1160)); tuner.serviceAntennas();
            }
        } else {
            busy = false;
            if (scenario == 3) {
                tuner.noteManualCommand(); tuner.manualCommandStarted();
                tuner.manualCommandComplete(TUNER_CMD_MANUAL_MODE, tuner.setManualMode());
            } else if (scenario == 4) {
                feed(tuner, frame(375, 0, 1160)); feed(tuner, frame(150, 0, 1160));
            } else if (scenario == 5) {
                for (int i = 0; i < 3; ++i) feed(tuner, frame(150, 0, 2328));
            } else if (scenario == 6) {
                busy = true; testMillis += 60000; tuner.serviceAntennas();
                TEST_ASSERT_TRUE(tuner.getState().antennaAuto == ldg::AntennaAuto::WaitingTuner);
                busy = false;
            }
            rf = false;
            feed(tuner, frame(0, 0, 1160)); tuner.serviceAntennas();
        }
        const bool recovered = scenario <= 2 || scenario == 6;
        TEST_ASSERT_EQUAL_UINT(recovered ? 3 : 2, toggles);
        TEST_ASSERT_EQUAL_UINT(scenario == 0 ? 1 : 0, tunes);
        TEST_ASSERT_EQUAL_INT(recovered ? ANT_B : ANT_A, tuner.getAntenna());
        TEST_ASSERT_EQUAL_INT(scenario == 3 ? MODE_MANUAL : scenario == 7 ? MODE_UNKNOWN : MODE_AUTO, tuner.getMode());
        if (recovered) {
            const auto trace = tuner.getDiagnostics().antennaSelection;
            TEST_ASSERT_TRUE(trace.selectionConfirmed && trace.autoRestored && !trace.deferred);
            TEST_ASSERT_EQUAL_UINT(scenario == 2 ? 2 : 1, trace.recovery);
        }
        const unsigned completedManuals = manuals;
        for (int i = 0; i < 5; ++i) {
            feed(tuner, frame(0, 0, 0)); tuner.serviceAntennas();
        }
        TEST_ASSERT_EQUAL_UINT(recovered ? 3 : 2, toggles);
        TEST_ASSERT_EQUAL_UINT(scenario == 0 ? 1 : 0, tunes);
        TEST_ASSERT_EQUAL_UINT(completedManuals, manuals);
    }
}

void test_auto_antenna_change_retunes_once_after_ack_and_before_restoring_auto() {
    // The old AUTO tuning cycle belongs to the old antenna. Only a confirmed
    // new port plus fresh low-power RF on the same band can authorize T.
    for (unsigned scenario = 0; scenario < 15; ++scenario) {
        HardwareSerial::written = nullptr;
        TunerProtocol tuner; begin(tuner);
        ldg::AntennaSettings settings;
        settings.enabled = true; settings.bands[BAND_20M] = ANT_B;
        tuner.configureAntennas(settings);
        bool operation = false, manual = false;
        unsigned toggles = 0, tunes = 0;
        uint32_t tuneAt = 0;
        std::string wire;
        HardwareSerial::written = [&](HardwareSerial& serial, uint8_t cmd) {
            respondSync(serial, cmd);
            if (operation && (cmd == 'M' || cmd == 'A' || cmd == 'T' || cmd == 'C')) wire += char(cmd);
            if (cmd == 'M') { manual = true; serial.bytes.push_back('M'); }
            if (cmd == 'C') {
                if (tunes && scenario != 3) TEST_ASSERT_TRUE(millis() - tuneAt >= 350 + 200);
                manual = false; serial.bytes.push_back('A');
            }
            if (cmd == 'P') serial.bytes.push_back('P');
            if (cmd == 'A') {
                ++toggles;
                if (!(operation && scenario == 12)) serial.bytes.push_back(toggles % 2 ? '2' : '1');
                if (operation && scenario == 9) tuner.noteManualCommand();
            }
            if (cmd == 'S' && operation) {
                const bool switched = toggles >= 3;
                if (switched && scenario == 6) return; // Lost post-switch metering.
                auto bytes = frame(switched && scenario == 4 ? 0 : switched && scenario == 5 ? 375 : 150,
                                   0, switched && scenario == 8 ? 2328 : 1160);
                if (switched && scenario == 7) bytes.back() = 0; // Damaged frame.
                if (switched && scenario == 10) serial.schedule(700, bytes);
                else for (auto b : bytes) serial.bytes.push_back(b);
            }
            if (cmd == 'T') {
                TEST_ASSERT_TRUE(operation && manual);
                TEST_ASSERT_EQUAL_UINT(3, toggles); // New antenna ACK precedes tuning.
                TEST_ASSERT_TRUE(tuner.getState().antennaAuto == ldg::AntennaAuto::Tuning);
                TEST_ASSERT_TRUE(tuner.getDiagnostics().antennaSelection.memoryTune == ldg::MemoryTuneResult::Tuning);
                tuneAt = millis();
                ++tunes;
                if (scenario != 3) serial.schedule(350, {uint8_t(scenario == 1 ? 'M' : scenario == 2 ? 'F' : 'T')});
            }
        };
        if (scenario == 11) TEST_ASSERT_TRUE(tuner.setManualMode());
        else if (scenario == 13) TEST_ASSERT_TRUE(tuner.bypass());
        else TEST_ASSERT_TRUE(tuner.setAutoMode());
        feed(tuner, frame(0, 0, 2328)); tuner.serviceAntennas();
        TEST_ASSERT_EQUAL_UINT(2, toggles);
        for (int i = 0; i < 3; ++i) feed(tuner, frame(150, 0, 2328));
        tuner.serviceAntennas();
        if (scenario == 14) TEST_ASSERT_TRUE(tuner.selectAntenna(ANT_B));
        operation = true;
        for (int i = 0; i < 3; ++i) feed(tuner, frame(150, 0, 1160));
        tuner.serviceAntennas();
        const bool shouldTune = scenario <= 3 || scenario == 10;
        TEST_ASSERT_EQUAL_UINT_MESSAGE(shouldTune ? 1 : 0, tunes, "A new AUTO antenna needs one guarded memory tune");
        if (scenario == 14) {
            TEST_ASSERT_TRUE(tuner.getState().antennaAuto == ldg::AntennaAuto::AlreadySelected);
            TEST_ASSERT_TRUE(wire.empty());
            TEST_ASSERT_EQUAL_UINT(3, toggles);
            continue;
        }
        if (shouldTune) TEST_ASSERT_EQUAL_STRING("MATC", wire.c_str());
        else if (scenario == 11 || scenario == 13) TEST_ASSERT_EQUAL_STRING("A", wire.c_str());
        TEST_ASSERT_EQUAL_INT(scenario == 11 ? MODE_MANUAL : scenario == 13 ? MODE_BYPASS : MODE_AUTO, tuner.getMode());
        const char* results[] = {"succeeded", "succeeded", "failed", "failed", "no_rf", "power_limit",
            "invalid_power", "invalid_power", "band_changed", "manual_override", "succeeded", "not_required", "not_required", "not_required"};
        JsonDocument status;
        ldg::writeTunerDiagnostics(status.to<JsonObject>(), tuner.getDiagnostics(), millis());
        TEST_ASSERT_EQUAL_STRING(results[scenario], status["antenna_selection"]["memory_tune"].as<const char*>());
        const char* states[] = {"selected", "selected", "tune_failed", "tune_failed", "tune_no_rf", "tune_power_limit",
            "tune_invalid_power", "tune_invalid_power", "tune_band_changed", "tune_manual_override", "selected", "selected", "error", "selected"};
        TEST_ASSERT_EQUAL_STRING(states[scenario], ldg::antennaAutoName(tuner.getState().antennaAuto));
        JsonDocument state;
        ldg::writeState(state, tuner.getState(), false);
        TEST_ASSERT_TRUE(tuner.getState().antennaAuto == ldg::readWebState(state.as<JsonVariantConst>()).antennaAuto);
        if (shouldTune && scenario != 3)
            TEST_ASSERT_EQUAL_STRING(scenario == 1 ? "M" : scenario == 2 ? "F" : "T",
                                    status["antenna_selection"]["memory_tune_ack"].as<const char*>());
        else TEST_ASSERT_TRUE(status["antenna_selection"]["memory_tune_ack"].isNull());
        // Failure, timeout, RF pauses and further packets never repeat T/A.
        const unsigned done = toggles;
        for (int i = 0; i < 5; ++i) {
            feed(tuner, frame(i % 2 ? 0 : 150, 0, 1160));
            tuner.serviceAntennas();
        }
        TEST_ASSERT_EQUAL_UINT(done, toggles);
        TEST_ASSERT_EQUAL_UINT(shouldTune ? 1 : 0, tunes);
    }
}

void test_failed_auto_restore_retries_once_at_fresh_zero_and_yields_to_manual_commands() {
    for (int scenario = 0; scenario < 4; ++scenario) {
        const bool cancel = scenario & 1, corruptSync = scenario & 2;
        TunerProtocol tuner; begin(tuner);
        ldg::AntennaSettings settings;
        settings.enabled = true; settings.bands[BAND_20M] = ANT_B;
        tuner.configureAntennas(settings);
        bool operation = false, dropRestore = false;
        unsigned restores = 0, toggles = 0;
        HardwareSerial::written = [&](HardwareSerial& serial, uint8_t cmd) {
            if (cmd == 'Z' && operation && dropRestore && corruptSync) {
                // Actual failed RF recovery: bit 3 set in several sync bytes.
                for (char b : std::string("008008800880008IzAz")) serial.bytes.push_back(b);
            } else respondSync(serial, cmd);
            if (cmd == 'M') serial.bytes.push_back('M');
            if (cmd == 'T') serial.bytes.push_back('T');
            if (cmd == 'C') {
                if (operation) ++restores;
                if (!dropRestore) serial.bytes.push_back('A');
            }
            if (cmd == 'A') { ++toggles; serial.bytes.push_back(toggles % 2 ? '2' : '1'); }
            if (cmd == 'S' && operation)
                for (auto b : frame(150, 0, 1160)) serial.bytes.push_back(b);
        };
        feed(tuner, frame(0, 0, 2328)); tuner.serviceAntennas();
        TEST_ASSERT_TRUE(tuner.setAutoMode());
        for (int i = 0; i < 3; ++i) feed(tuner, frame(150, 0, 2328));
        tuner.serviceAntennas();
        for (int i = 0; i < 3; ++i) feed(tuner, frame(150, 0, 1160));
        operation = dropRestore = true;
        tuner.serviceAntennas();
        const unsigned failedWrites = corruptSync ? 1 : 2;
        TEST_ASSERT_EQUAL_UINT(failedWrites, restores);
        TEST_ASSERT_EQUAL_INT(MODE_UNKNOWN, tuner.getMode());
        TEST_ASSERT_EQUAL_UINT(3, toggles);
        feed(tuner, frame(150, 0, 1160)); tuner.serviceAntennas();
        TEST_ASSERT_EQUAL_UINT(failedWrites, restores); // No background writes during RF.
        testMillis += 501; tuner.serviceAntennas();
        TEST_ASSERT_EQUAL_UINT(failedWrites, restores); // Missing data is not RF off.
        dropRestore = false;
        if (cancel) {
            tuner.noteManualCommand(); tuner.manualCommandStarted();
            tuner.manualCommandComplete(TUNER_CMD_MANUAL_MODE, tuner.setManualMode());
        }
        feed(tuner, frame(0, 0, 1160)); tuner.serviceAntennas();
        TEST_ASSERT_EQUAL_UINT(failedWrites + !cancel, restores);
        TEST_ASSERT_EQUAL_INT(cancel ? MODE_MANUAL : MODE_AUTO, tuner.getMode());
        for (int i = 0; i < 4; ++i) {
            feed(tuner, frame(0, 0, 1160)); tuner.serviceAntennas();
        }
        TEST_ASSERT_EQUAL_UINT(failedWrites + !cancel, restores);
        TEST_ASSERT_EQUAL_UINT(3, toggles);
        const auto trace = tuner.getDiagnostics().antennaSelection;
        TEST_ASSERT_EQUAL(!cancel, trace.autoRestored);
        TEST_ASSERT_EQUAL_UINT(cancel ? 2 : 3, trace.restoreAttempts);
    }
}
void test_meter_watchdog_recovers_lost_start_with_backoff_and_respects_stop_and_manual_queue() {
    testMillis = 0xfffff000u;
    TunerProtocol tuner; begin(tuner);
    unsigned starts = 0, toggles = 0;
    bool reply = false;
    HardwareSerial::written = [&](HardwareSerial& serial, uint8_t cmd) {
        if (cmd == 'S') {
            ++starts;
            if (reply) for (auto b : frame(0, 0, 2328)) serial.bytes.push_back(b);
        }
        if (cmd == 'A') { ++toggles; serial.bytes.push_back(toggles % 2 ? '2' : '1'); }
    };
    const uint32_t initial = millis();
    testMillis = initial + 999; tuner.serviceAntennas();
    TEST_ASSERT_EQUAL_UINT(0, starts);
    ++testMillis; tuner.serviceAntennas(true);
    TEST_ASSERT_EQUAL_UINT(0, starts); // Queued manual control has priority.
    tuner.serviceAntennas();
    TEST_ASSERT_EQUAL_UINT(1, starts); TEST_ASSERT_EQUAL_UINT(0, toggles);
    const uint32_t attempted = millis();
    testMillis = attempted + 999; tuner.serviceAntennas();
    TEST_ASSERT_EQUAL_UINT(1, starts);
    reply = true; ++testMillis; tuner.serviceAntennas(); tuner.process();
    TEST_ASSERT_EQUAL_UINT(2, toggles); // Only a real zero packet allowed startup probing.
    TEST_ASSERT_EQUAL_UINT(2, tuner.getDiagnostics().meterRestarts);
    TEST_ASSERT_TRUE(tuner.getState().meterValid);
    const unsigned healthyStarts = starts;
    for (int i = 0; i < 5; ++i) {
        testMillis += 500; feed(tuner, frame(0, 0, 2328)); tuner.serviceAntennas();
    }
    TEST_ASSERT_EQUAL_UINT(healthyStarts, starts);
    TEST_ASSERT_TRUE(tuner.stopMeter());
    testMillis += 5000; tuner.serviceAntennas();
    TEST_ASSERT_EQUAL_UINT(healthyStarts, starts);
    HardwareSerial::latest->failCommand = 'S';
    TEST_ASSERT_FALSE(tuner.startMeter());
    tuner.serviceAntennas(); TEST_ASSERT_EQUAL_UINT(healthyStarts, starts);
    HardwareSerial::latest->failCommand = -1;
    testMillis += 1000; tuner.serviceAntennas();
    TEST_ASSERT_EQUAL_UINT(healthyStarts + 1, starts);
    TEST_ASSERT_EQUAL_UINT(3, tuner.getDiagnostics().meterRestarts);
    TEST_ASSERT_EQUAL_UINT(2, toggles); // Recovery never repeats the antenna probe.
}
void test_stalled_meter_after_selection_recovers_before_detecting_next_band_and_preserves_manual_choice() {
    TunerProtocol tuner; begin(tuner);
    ldg::AntennaSettings settings;
    settings.enabled = true; settings.bands[BAND_40M] = ANT_A; settings.bands[BAND_20M] = ANT_B;
    tuner.configureAntennas(settings);
    unsigned toggles = 0;
    uint16_t streamingBand = 0;
    HardwareSerial::written = [&](HardwareSerial& serial, uint8_t cmd) {
        respondSync(serial, cmd);
        if (cmd == 'M') serial.bytes.push_back('M');
        if (cmd == 'A') { ++toggles; serial.bytes.push_back(toggles % 2 ? '2' : '1'); }
        if (cmd == 'S' && streamingBand)
            for (int i = 0; i < 3; ++i)
                for (auto b : frame(150, 0, streamingBand)) serial.bytes.push_back(b);
    };
    TEST_ASSERT_TRUE(tuner.setManualMode());
    feed(tuner, frame(0, 0, 2328)); tuner.serviceAntennas();
    for (int i = 0; i < 3; ++i) feed(tuner, frame(150, 0, 2328));
    tuner.serviceAntennas();
    for (int i = 0; i < 3; ++i) feed(tuner, frame(150, 0, 1160));
    tuner.serviceAntennas(); // S was written after selecting ANT2, but no packets followed.
    TEST_ASSERT_EQUAL_UINT(3, toggles); TEST_ASSERT_EQUAL_INT(ANT_B, tuner.getAntenna());
    streamingBand = 2328;
    testMillis += 1000; tuner.process(); tuner.serviceAntennas(); tuner.process();
    TEST_ASSERT_EQUAL_UINT(1, tuner.getDiagnostics().meterRestarts);
    TEST_ASSERT_EQUAL_UINT(4, toggles); TEST_ASSERT_EQUAL_INT(ANT_A, tuner.getAntenna());
    TEST_ASSERT_TRUE(tuner.getState().antennaAuto == ldg::AntennaAuto::Selected);
    TEST_ASSERT_EQUAL_INT(BAND_40M, tuner.getState().antennaAutoBand);
    JsonDocument status; ldg::writeState(status, tuner.getState(), false);
    TEST_ASSERT_EQUAL_STRING("40m", status["antenna_auto_band"].as<const char*>());
    TEST_ASSERT_EQUAL_INT(BAND_40M, ldg::readWebState(status.as<JsonVariantConst>()).antennaAutoBand);
    tuner.manualCommandStarted();
    tuner.manualCommandComplete(TUNER_CMD_SELECT_ANT_B, tuner.selectAntenna(ANT_B)); tuner.process();
    testMillis += 1000; tuner.serviceAntennas();
    TEST_ASSERT_EQUAL_UINT(5, toggles); TEST_ASSERT_EQUAL_INT(ANT_B, tuner.getAntenna());
    TEST_ASSERT_TRUE(tuner.getState().antennaAuto == ldg::AntennaAuto::ManualSelection);
}
void test_enabled_automation_confirms_startup_auto_once_after_probe_and_respects_manual_priority() {
    for (int scenario = 0; scenario < 5; ++scenario) {
        TunerProtocol tuner; begin(tuner);
        ldg::AntennaSettings settings;
        settings.enabled = scenario != 3; settings.bands[BAND_20M] = ANT_B;
        tuner.configureAntennas(settings);
        unsigned toggles = 0, autos = 0;
        bool dropAuto = scenario == 2;
        HardwareSerial::written = [&](HardwareSerial& serial, uint8_t cmd) {
            respondSync(serial, cmd);
            if (cmd == 'A') { ++toggles; serial.bytes.push_back(toggles % 2 ? '2' : '1'); }
            if (cmd == 'C') {
                TEST_ASSERT_EQUAL_UINT(2, toggles); // Startup probe finishes first.
                ++autos;
                if (!dropAuto && !(scenario == 1 && autos == 1)) serial.bytes.push_back('A');
            }
            if (cmd == 'M') serial.bytes.push_back('M');
        };
        if (scenario == 4) tuner.manualCommandStarted();
        tuner.serviceAntennas(); TEST_ASSERT_EQUAL_UINT(0, autos);
        feed(tuner, frame(0, 0, 2328)); tuner.serviceAntennas();
        TEST_ASSERT_EQUAL_UINT(2, toggles); TEST_ASSERT_EQUAL_UINT(0, autos);
        tuner.serviceAntennas();
        TEST_ASSERT_EQUAL_UINT(scenario >= 3 ? 0 : scenario == 0 ? 1 : 2, autos);
        dropAuto = false;
        if (scenario == 2) {
            TEST_ASSERT_EQUAL_INT(MODE_UNKNOWN, tuner.getMode());
            feed(tuner, frame(0, 0, 2328)); tuner.serviceAntennas();
            TEST_ASSERT_EQUAL_UINT(3, autos);
        }
        TEST_ASSERT_EQUAL_INT(scenario < 3 ? MODE_AUTO : MODE_UNKNOWN, tuner.getMode());
        const unsigned completed = autos;
        for (int i = 0; i < 5; ++i) {
            feed(tuner, frame(0, 0, 2328)); tuner.serviceAntennas();
        }
        for (int i = 0; i < 3; ++i) feed(tuner, frame(150, 0, 2328));
        tuner.serviceAntennas();
        TEST_ASSERT_EQUAL_UINT(2, toggles); // Unassigned first band cannot change the antenna.
        TEST_ASSERT_EQUAL_UINT(completed, autos);
        tuner.manualCommandStarted();
        tuner.manualCommandComplete(TUNER_CMD_MANUAL_MODE, tuner.setManualMode());
        feed(tuner, frame(0, 0, 1160)); tuner.serviceAntennas();
        TEST_ASSERT_EQUAL_INT(MODE_MANUAL, tuner.getMode());
        TEST_ASSERT_EQUAL_UINT(completed, autos);
    }
}
void test_unknown_tuner_mode_preserves_mode_and_selects_only_after_rf_off() {
    TunerProtocol tuner; begin(tuner);
    ldg::AntennaSettings settings;
    settings.enabled = true; settings.bands[BAND_40M] = ANT_A; settings.bands[BAND_20M] = ANT_B;
    tuner.configureAntennas(settings);
    // An explicit user action suppresses startup AUTO negotiation, even if
    // the action itself left the actual tuner mode unknown.
    tuner.manualCommandStarted();
    unsigned toggles = 0, refreshes = 0;
    bool operation = false;
    HardwareSerial::written = [&](HardwareSerial& serial, uint8_t cmd) {
        TEST_ASSERT_TRUE(cmd != 'M' && cmd != 'C');
        if (cmd == 'A') { ++toggles; serial.bytes.push_back(toggles % 2 ? '2' : '1'); }
        if (cmd == 'S' && operation) { ++refreshes; serial.schedule(700, frame(0, 0, 1160)); }
    };
    feed(tuner, frame(0, 0, 2328)); tuner.serviceAntennas();
    for (int i = 0; i < 3; ++i) feed(tuner, frame(150, 0, 2328));
    tuner.serviceAntennas();
    for (int i = 0; i < 3; ++i) feed(tuner, frame(150, 0, 1160));
    tuner.serviceAntennas();
    TEST_ASSERT_EQUAL_UINT(2, toggles);
    TEST_ASSERT_TRUE(tuner.getState().antennaAuto == ldg::AntennaAuto::WaitingRfOff);
    JsonDocument status; ldg::writeState(status, tuner.getState(), false);
    TEST_ASSERT_TRUE(ldg::readWebState(status.as<JsonVariantConst>()).antennaAuto == ldg::AntennaAuto::WaitingRfOff);
    operation = true;
    testMillis += 501;
    tuner.serviceAntennas();
    TEST_ASSERT_TRUE(tuner.getState().antennaAuto == ldg::AntennaAuto::WaitingMeter);
    TEST_ASSERT_EQUAL_UINT(1, refreshes);
    TEST_ASSERT_EQUAL_UINT(2, toggles);
    testMillis += 450;
    tuner.process(); tuner.serviceAntennas();
    TEST_ASSERT_EQUAL_UINT(3, toggles);
    TEST_ASSERT_TRUE(tuner.getState().antennaAuto == ldg::AntennaAuto::Selected);
    TEST_ASSERT_EQUAL_INT(MODE_UNKNOWN, tuner.getMode());
    TEST_ASSERT_EQUAL_UINT(2, refreshes); // One recovery S and the normal post-A S.
    for (int i = 0; i < 5; ++i) {
        testMillis += 5000;
        tuner.serviceAntennas();
    }
    TEST_ASSERT_EQUAL_UINT(3, toggles);
}

static ldg::RadioState radioFixture;
static ldg::RadioState readRadioFixture() { return radioFixture; }

void test_tci_checks_the_final_relay_write_and_defers_tx_stale_power_and_source_loss() {
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        testMillis = 0;
        radioFixture = ldg::RadioState{};
        radioFixture.connection = ldg::RadioConnection::Ready;
        radioFixture.frequencyValid = radioFixture.rxKnown = radioFixture.tuneKnown = true;
        radioFixture.txFrequencyHz = 14074000; radioFixture.stableMs = 200;
        TunerProtocol tuner;
        tuner.setRadioStateGetter(readRadioFixture);
        unsigned toggles = 0, tunes = 0; bool selecting = false, injected = false;
        tuner_ant_t antenna = ANT_A;
        HardwareSerial::written = [&](HardwareSerial& serial, uint8_t cmd) {
            respondSync(serial,cmd);
            if (cmd == 'S') {
                if (!(scenario == 2 && selecting && injected))
                    for (auto byte : frame(0,0,0)) serial.bytes.push_back(byte);
                if (scenario == 3 && selecting && toggles == 3) { radioFixture.transmitting = true; injected = true; }
            }
            if (cmd == 'M') serial.bytes.push_back('M');
            if (cmd == 'C') serial.bytes.push_back('A');
            if (cmd == 'T' || cmd == 'F') { ++tunes; serial.bytes.push_back('T'); }
            if (cmd == 'X' && selecting && tuner.getMode() == MODE_MANUAL && !injected && scenario != 3) {
                injected = true;
                if (scenario == 0) radioFixture.transmitting = true;
                if (scenario == 1) radioFixture.connection = ldg::RadioConnection::Disconnected;
                if (scenario == 2) testMillis += 600; // Fresh before X, stale at the actual A byte.
            }
            if (cmd == 'A') {
                ++toggles;
                if (!(scenario == 3 && selecting && toggles == 3)) antenna = antenna == ANT_A ? ANT_B : ANT_A;
                serial.bytes.push_back(antenna == ANT_A ? '1' : '2');
            }
        };
        begin(tuner);
        ldg::AntennaSettings settings; settings.enabled = true; settings.bands[BAND_20M] = ANT_B;
        tuner.configureAntennas(settings);
        feed(tuner,frame(0,0,0)); tuner.serviceAntennas(); // Establish original port using two ACKs.
        TEST_ASSERT_EQUAL_UINT(2,toggles);
        TEST_ASSERT_EQUAL_INT(int(ldg::AntennaInit::Ready),int(tuner.getState().antennaInit));
        tuner.serviceAntennas(); // Confirm initial AUTO policy.
        selecting = true;
        feed(tuner,frame(0,0,0)); tuner.serviceAntennas();
        TEST_ASSERT_TRUE(injected);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(scenario == 3 ? 3 : 2,toggles,
            scenario == 0 ? "TX before A" : scenario == 1 ? "TCI loss before A" : scenario == 2 ? "Stale zero before A" : "TX before recovery A");
        TEST_ASSERT_EQUAL_UINT(0,tunes);
        TEST_ASSERT_EQUAL_INT(MODE_AUTO,tuner.getMode());
        selecting = false;
        if (scenario == 1) {
            for (unsigned i = 0; i < 3; ++i) { feed(tuner,frame(0,0,0)); tuner.serviceAntennas(); }
            TEST_ASSERT_FALSE(tuner.getState().antennaFromRadio);
            TEST_ASSERT_EQUAL_UINT(2,toggles); // Loss does not execute held TCI intent.
        } else {
            radioFixture.transmitting = false;
            feed(tuner,frame(0,0,0)); tuner.serviceAntennas();
            TEST_ASSERT_EQUAL_INT(ANT_B,tuner.getAntenna());
            TEST_ASSERT_EQUAL_UINT(scenario == 3 ? 4 : 3,toggles);
            TEST_ASSERT_EQUAL_UINT(0,tunes);
        }
    }
}

void test_tci_startup_probe_defers_restoring_original_port_when_tx_starts() {
    radioFixture = ldg::RadioState{};
    radioFixture.connection = ldg::RadioConnection::Ready;
    radioFixture.frequencyValid = radioFixture.rxKnown = radioFixture.tuneKnown = true;
    radioFixture.txFrequencyHz = 14074000; radioFixture.stableMs = 200;
    TunerProtocol tuner; tuner.setRadioStateGetter(readRadioFixture);
    unsigned toggles = 0;
    HardwareSerial::written = [&](HardwareSerial& serial, uint8_t cmd) {
        respondSync(serial,cmd);
        if (cmd == 'A') { ++toggles; serial.bytes.push_back(toggles % 2 ? '2' : '1'); }
        if (cmd == 'S') {
            for (auto byte : frame(0,0,0)) serial.bytes.push_back(byte);
            if (toggles == 1) radioFixture.transmitting = true;
        }
    };
    begin(tuner); feed(tuner,frame(0,0,0)); tuner.serviceAntennas();
    TEST_ASSERT_EQUAL_UINT(1,toggles);
    TEST_ASSERT_EQUAL_INT(int(ldg::AntennaInit::Restoring),int(tuner.getState().antennaInit));
    feed(tuner,frame(0,0,0)); tuner.serviceAntennas();
    TEST_ASSERT_EQUAL_UINT(1,toggles);
    radioFixture.transmitting = false;
    feed(tuner,frame(0,0,0)); tuner.serviceAntennas();
    TEST_ASSERT_EQUAL_UINT(2,toggles);
    TEST_ASSERT_EQUAL_INT(ANT_A,tuner.getAntenna());
    TEST_ASSERT_EQUAL_INT(int(ldg::AntennaInit::Ready),int(tuner.getState().antennaInit));
}

void test_managed_mode_tunes_every_tx_without_reselecting_same_band_or_restoring_native_auto() {
    for (bool fallback : {false, true}) {
        HardwareSerial::written = nullptr;
        TunerProtocol tuner; begin(tuner);
        uint16_t band = 2328, fwd = 0, reflected = 0;
        unsigned toggles = 0, memories = 0, autos = 0, manuals = 0;
        uint8_t antenna = 2;
        std::string wire;
        HardwareSerial::written = [&](HardwareSerial& serial, uint8_t cmd) {
            respondSync(serial,cmd);
            if (cmd == 'M') { ++manuals; serial.bytes.push_back('M'); wire += 'M'; }
            if (cmd == 'C') { ++autos; serial.bytes.push_back('A'); wire += 'C'; }
            if (cmd == 'A') { ++toggles; antenna = antenna == 1 ? 2 : 1; serial.bytes.push_back(antenna == 1 ? '1' : '2'); wire += 'A'; }
            if (cmd == 'T') { ++memories; serial.bytes.push_back('T'); wire += 'T'; }
            if (cmd == 'S') for (auto b : frame(fwd,reflected,band)) serial.bytes.push_back(b);
        };
        ldg::AntennaSettings antennas; antennas.enabled = 1;
        antennas.bands[BAND_40M] = ANT_A; antennas.bands[BAND_20M] = ANT_B;
        tuner.configureAntennas(antennas);
        ldg::TuningSettings profile; profile.managed = 1; profile.autoFallback = fallback;
        tuner.configureTuning(profile);
        feed(tuner,frame(0,0,band)); tuner.serviceAntennas(); // M confirmed first.
        tuner.serviceAntennas(); // Startup probe returns ANT2.
        TEST_ASSERT_EQUAL_INT(ANT_B,tuner.getAntenna());
        TEST_ASSERT_EQUAL_INT(MODE_MANUAL,tuner.getMode());
        TEST_ASSERT_EQUAL_UINT(2,toggles);
        wire.clear(); fwd = 150; reflected = fallback ? 60 : 0; // ~40W, SWR 2.33.
        for (int i = 0; i < 3; ++i) feed(tuner,frame(fwd,reflected,band));
        tuner.serviceAntennas(); // First 40m must select ANT1, then T exactly once.
        TEST_ASSERT_EQUAL_STRING("AT",wire.c_str());
        TEST_ASSERT_EQUAL_UINT(1,memories); TEST_ASSERT_EQUAL_UINT(0,autos);
        for (int i = 0; i < 4; ++i) { feed(tuner,frame(fwd,reflected,band)); tuner.serviceAntennas(); }
        TEST_ASSERT_EQUAL_UINT(fallback ? 1 : 0,autos);
        TEST_ASSERT_EQUAL_INT(fallback ? MODE_AUTO : MODE_MANUAL,tuner.getMode());
        const unsigned selected = toggles;
        fwd = reflected = 0;
        for (int i = 0; i <= 8; ++i) { testMillis += 200; feed(tuner,frame(0,0,band)); tuner.serviceAntennas(); }
        TEST_ASSERT_EQUAL_INT(MODE_MANUAL,tuner.getMode());
        fwd = 150; reflected = 0;
        for (int i = 0; i < 3; ++i) feed(tuner,frame(fwd,reflected,band));
        tuner.serviceAntennas();
        TEST_ASSERT_EQUAL_UINT(2,memories); TEST_ASSERT_EQUAL_UINT(selected,toggles);
        // A manual port selection stays selected on subsequent transmissions.
        tuner.noteManualCommand(); tuner.manualCommandStarted();
        TEST_ASSERT_TRUE(tuner.selectAntenna(ANT_B)); tuner.manualCommandComplete(TUNER_CMD_SELECT_ANT_B,true);
        fwd = 0;
        for (int i = 0; i <= 8; ++i) { testMillis += 200; feed(tuner,frame(0,0,band)); tuner.serviceAntennas(); }
        fwd = 150;
        for (int i = 0; i < 3; ++i) feed(tuner,frame(fwd,0,band));
        tuner.serviceAntennas();
        TEST_ASSERT_EQUAL_INT(ANT_B,tuner.getAntenna()); TEST_ASSERT_EQUAL_UINT(3,memories);
        TEST_ASSERT_EQUAL_UINT(selected+1,toggles);
        TEST_ASSERT_EQUAL_UINT(fallback ? 1 : 0,autos);
        TEST_ASSERT_EQUAL_UINT(fallback ? 2 : 1,manuals);
    }
}
void test_managed_mode_does_not_tune_without_confirmed_port_mode_or_safe_rf() {
    for (int scenario = 0; scenario < 5; ++scenario) {
        HardwareSerial::written = nullptr;
        TunerProtocol tuner; begin(tuner);
        unsigned memories = 0, manuals = 0;
        HardwareSerial::written = [&](HardwareSerial& serial,uint8_t cmd) {
            respondSync(serial,cmd);
            if (cmd == 'M') { ++manuals; if (scenario != 0) serial.bytes.push_back('M'); }
            if (cmd == 'A') serial.bytes.push_back('1');
            if (cmd == 'T') { ++memories; serial.bytes.push_back('F'); }
        };
        if (scenario != 1) {
            tuner.manualCommandStarted(); TEST_ASSERT_TRUE(tuner.selectAntenna(ANT_A));
            tuner.manualCommandComplete(TUNER_CMD_SELECT_ANT_A,true);
        }
        ldg::TuningSettings profile; profile.managed = 1; tuner.configureTuning(profile);
        const uint16_t power = scenario == 2 ? 375 : 150; // 100W+ is never a tuning authorizer.
        for (int i=0;i<3;++i) feed(tuner,frame(power,0,2328));
        if (scenario == 3) testMillis += 600;
        tuner.serviceAntennas(scenario == 4);
        for (int i=0;i<4;++i) tuner.serviceAntennas(scenario == 4);
        TEST_ASSERT_EQUAL_UINT(0,memories);
        TEST_ASSERT_LESS_OR_EQUAL_UINT(2,manuals);
    }
}

void test_managed_fallback_retries_once_but_not_after_rf_becomes_unsafe() {
    for (int scenario = 0; scenario < 3; ++scenario) {
        HardwareSerial::written = nullptr;
        TunerProtocol tuner; begin(tuner);
        unsigned memories = 0, autos = 0;
        uint16_t fwd = 150;
        HardwareSerial::written = [&](HardwareSerial& serial,uint8_t cmd) {
            respondSync(serial,cmd);
            if (cmd == 'M') serial.bytes.push_back('M');
            if (cmd == 'A') serial.bytes.push_back('1');
            if (cmd == 'T') { ++memories; serial.bytes.push_back('F'); }
            if (cmd == 'C') {
                ++autos;
                if (autos == 2) serial.bytes.push_back('A');
                if (scenario == 1) fwd = 375; // RF rises above 100 W after the first C loses its ACK.
                if (scenario == 2) fwd = 0; // Or transmission ends.
            }
            if (cmd == 'S') for (auto b : frame(fwd,60,2328)) serial.bytes.push_back(b);
        };
        tuner.manualCommandStarted(); TEST_ASSERT_TRUE(tuner.selectAntenna(ANT_A));
        tuner.manualCommandComplete(TUNER_CMD_SELECT_ANT_A,true);
        ldg::TuningSettings profile; profile.managed = profile.autoFallback = 1; tuner.configureTuning(profile);
        for (int i=0;i<3;++i) feed(tuner,frame(150,60,2328));
        tuner.serviceAntennas(); tuner.serviceAntennas();
        for (int i=0;i<3;++i) { feed(tuner,frame(150,60,2328)); tuner.serviceAntennas(); }
        TEST_ASSERT_EQUAL_UINT(1,memories);
        TEST_ASSERT_EQUAL_UINT(scenario == 0 ? 2 : 1,autos);
        for (int i=0;i<5;++i) tuner.serviceAntennas();
        TEST_ASSERT_EQUAL_UINT(scenario == 0 ? 2 : 1,autos);
        fwd = 0;
        for (int i=0;i<=8;++i) { testMillis += 200; feed(tuner,frame(0,0,2328)); tuner.serviceAntennas(); }
        TEST_ASSERT_EQUAL_INT(MODE_MANUAL,tuner.getMode());
    }
}

void test_manual_command_queued_during_antenna_selection_cancels_unstarted_memory_tune() {
    HardwareSerial::written = nullptr;
    TunerProtocol tuner; begin(tuner);
    unsigned toggles = 0, memories = 0;
    bool rf = false;
    HardwareSerial::written = [&](HardwareSerial& serial,uint8_t cmd) {
        respondSync(serial,cmd);
        if (cmd == 'M') serial.bytes.push_back('M');
        if (cmd == 'A') {
            ++toggles; serial.bytes.push_back(toggles % 2 ? '2' : '1');
            if (rf) tuner.noteManualCommand(); // ANT2 requested while the automatic A is in flight.
        }
        if (cmd == 'T') { ++memories; serial.bytes.push_back('T'); }
        if (cmd == 'S') for (auto b : frame(rf?150:0,0,1160)) serial.bytes.push_back(b);
    };
    ldg::AntennaSettings antennas; antennas.enabled = 1; antennas.bands[BAND_20M] = ANT_B;
    tuner.configureAntennas(antennas);
    ldg::TuningSettings profile; profile.managed = 1; tuner.configureTuning(profile);
    feed(tuner,frame(0,0,1160)); tuner.serviceAntennas(); tuner.serviceAntennas();
    rf = true;
    for (int i=0;i<3;++i) feed(tuner,frame(150,0,1160));
    tuner.serviceAntennas();
    TEST_ASSERT_EQUAL_UINT(3,toggles); TEST_ASSERT_EQUAL_INT(ANT_B,tuner.getAntenna());
    TEST_ASSERT_EQUAL_UINT(0,memories);
    TEST_ASSERT_TRUE(tuner.getState().tuningState==ldg::TuningState::ManualOverride);
}

struct ModeConfig {
    ldg::TuningSettings settings;
    bool writable = true;
    unsigned saves = 0;
    ldg::TuningSettings tuningSettings() const { return settings; }
    bool updateTuningMode(bool controlled) {
        if (!writable) return false;
        settings.managed = controlled; ++saves; return true;
    }
};
void test_three_operating_modes_are_exclusive_and_controlled_never_reaches_uart() {
    HardwareSerial::written = nullptr;
    TunerProtocol tuner; begin(tuner);
    ModeConfig config;
    std::string wire;
    uint16_t fwd = 0;
    HardwareSerial::written = [&](HardwareSerial& serial,uint8_t cmd) {
        TEST_ASSERT_NOT_EQUAL(TUNER_CMD_CONTROLLED_MODE,cmd);
        respondSync(serial,cmd);
        if (cmd == 'A') serial.bytes.push_back('1');
        if (cmd == 'M' || cmd == 'C' || cmd == 'T') wire += char(cmd);
        if (cmd == 'M') serial.bytes.push_back('M');
        if (cmd == 'C') serial.bytes.push_back('A');
        if (cmd == 'T') serial.bytes.push_back('T');
        if (cmd == 'S') for (auto b : frame(fwd,0,2328)) serial.bytes.push_back(b);
    };
    tuner.manualCommandStarted(); TEST_ASSERT_TRUE(tuner.selectAntenna(ANT_A));
    tuner.manualCommandComplete(TUNER_CMD_SELECT_ANT_A,true);
    for (uint8_t cmd : {uint8_t(TUNER_CMD_MANUAL_MODE),uint8_t(TUNER_CMD_AUTO_MODE)}) {
        TEST_ASSERT_TRUE(ldg::selectOperatingMode(config,tuner,cmd));
        TEST_ASSERT_FALSE(config.settings.managed);
        for (int i=0;i<3;++i) feed(tuner,frame(150,0,2328));
        tuner.serviceAntennas();
        TEST_ASSERT_TRUE(wire.find('T') == std::string::npos);
    }
    fwd = 150;
    TEST_ASSERT_TRUE(ldg::selectOperatingMode(config,tuner,TUNER_CMD_CONTROLLED_MODE));
    TEST_ASSERT_TRUE(config.settings.managed);
    TEST_ASSERT_EQUAL_INT(MODE_CONTROLLED,ldg::operatingMode(tuner.getState()));
    TEST_ASSERT_EQUAL_INT(MODE_MANUAL,tuner.getMode());
    for (int i=0;i<3;++i) feed(tuner,frame(fwd,0,2328));
    tuner.serviceAntennas(); TEST_ASSERT_EQUAL_STRING("MCMT",wire.c_str());
    TEST_ASSERT_TRUE(tuner.setAutoMode()); // Temporary high-SWR fallback is physical AUTO only.
    JsonDocument doc; ldg::writeState(doc,tuner.getState(),false);
    TEST_ASSERT_EQUAL_INT(MODE_CONTROLLED,doc["mode"].as<int>());
    TEST_ASSERT_EQUAL_INT(MODE_AUTO,doc["tuner_mode"].as<int>());
    const auto forwarded = ldg::readWebState(doc.as<JsonVariantConst>());
    TEST_ASSERT_EQUAL_INT(MODE_CONTROLLED,ldg::operatingMode(forwarded));
    TEST_ASSERT_EQUAL_INT(MODE_AUTO,forwarded.mode);
    ldg::writeState(doc,tuner.getState(),true);
    TEST_ASSERT_EQUAL_STRING("controlled",doc["mode"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("auto",doc["tuner_mode"].as<const char*>());
    TEST_ASSERT_TRUE(ldg::selectOperatingMode(config,tuner,TUNER_CMD_MANUAL_MODE));
    TEST_ASSERT_EQUAL_INT(MODE_MANUAL,ldg::operatingMode(tuner.getState()));
    fwd = 0;
    for (int i=0;i<=8;++i) { testMillis += 200; feed(tuner,frame(0,0,2328)); tuner.serviceAntennas(); }
    fwd = 150;
    for (int i=0;i<3;++i) feed(tuner,frame(fwd,0,2328));
    tuner.serviceAntennas();
    TEST_ASSERT_EQUAL_STRING("MCMTCM",wire.c_str());
}
void test_failed_mode_persistence_does_not_change_policy_or_send_a_command() {
    TunerProtocol tuner; begin(tuner); ModeConfig config; config.writable = false;
    unsigned writes = 0;
    HardwareSerial::written = [&](HardwareSerial&,uint8_t) { ++writes; };
    TEST_ASSERT_FALSE(ldg::selectOperatingMode(config,tuner,TUNER_CMD_CONTROLLED_MODE));
    TEST_ASSERT_FALSE(tuner.getState().tuningManaged); TEST_ASSERT_EQUAL_UINT(0,writes);
    TEST_ASSERT_FALSE(ldg::selectOperatingMode(config,tuner,TUNER_CMD_MEM_TUNE));
    TEST_ASSERT_EQUAL_UINT(0,writes);
    config.writable = true;
    TEST_ASSERT_FALSE(ldg::selectOperatingMode(config,tuner,TUNER_CMD_CONTROLLED_MODE)); // No M ACK.
    TEST_ASSERT_TRUE(tuner.getState().tuningManaged);
    TEST_ASSERT_EQUAL_INT(MODE_UNKNOWN,tuner.getMode());
    TEST_ASSERT_EQUAL_INT(MODE_CONTROLLED,ldg::operatingMode(tuner.getState()));
    TEST_ASSERT_TRUE(tuner.getState().tuningState == ldg::TuningState::Error);
}

void test_controlled_late_intraband_mismatch_repeats_memory_then_optional_auto_without_antenna_switches() {
    for (bool fallback : {false,true}) {
        HardwareSerial::written=nullptr;
        TunerProtocol tuner; begin(tuner);
        uint16_t forward=150, reflected=10, band=2328;
        unsigned memories=0, antennas=0, autos=0;
        std::string commands, expected;
        HardwareSerial::written=[&](HardwareSerial& serial,uint8_t cmd) {
            respondSync(serial,cmd);
            if (cmd=='A') { ++antennas; serial.bytes.push_back('1'); }
            if (cmd=='M') { commands+='M'; serial.bytes.push_back('M'); }
            if (cmd=='T') { commands+='T'; ++memories; serial.bytes.push_back('T'); }
            if (cmd=='C') { commands+='C'; ++autos; serial.bytes.push_back('A'); }
            if (cmd=='S') for (auto b:frame(forward,reflected,band)) serial.bytes.push_back(b);
        };
        tuner.manualCommandStarted(); TEST_ASSERT_TRUE(tuner.selectAntenna(ANT_A));
        tuner.manualCommandComplete(TUNER_CMD_SELECT_ANT_A,true);
        ldg::TuningSettings profile; profile.managed=1; profile.autoFallback=fallback;
        tuner.configureTuning(profile);
        for (int i=0;i<3;++i) feed(tuner,frame(forward,reflected,band));
        tuner.serviceAntennas(); tuner.serviceAntennas();
        TEST_ASSERT_EQUAL_UINT(1,memories); TEST_ASSERT_EQUAL_UINT(0,autos);
        TEST_ASSERT_EQUAL_STRING("MT",commands.c_str()); commands.clear();
        for (int cycle=0;cycle<4;++cycle) {
            // Good match arms a new rise even while the physical tuner is AUTO.
            reflected=10;
            for (int i=0;i<100;++i) {
                testMillis+=100; feed(tuner,frame(forward,reflected,band)); tuner.serviceAntennas();
            }
            TEST_ASSERT_TRUE(tuner.getState().tuningState==
                (cycle&&fallback?ldg::TuningState::AutoActive:ldg::TuningState::Matched));
            band=2330; reflected=60; // Different point within 40m; same RF session and antenna.
            for (int i=1;i<=3;++i) {
                testMillis+=100; feed(tuner,frame(forward,reflected,band)); tuner.serviceAntennas();
                TEST_ASSERT_EQUAL_UINT(fallback?cycle:0,autos); // M/T must precede C.
            }
            tuner.serviceAntennas(); // Complete pending T after acknowledged M in later cycles.
            TEST_ASSERT_EQUAL_UINT(cycle+2,memories); TEST_ASSERT_EQUAL_UINT(1,antennas);
            for (int i=0;i<3;++i) {
                testMillis+=100; feed(tuner,frame(forward,reflected,band)); tuner.serviceAntennas();
            }
            expected += fallback?(cycle?"MTC":"TC"):"T";
            TEST_ASSERT_EQUAL_STRING(expected.c_str(),commands.c_str());
            TEST_ASSERT_EQUAL_UINT(fallback?cycle+1:0,autos);
            TEST_ASSERT_EQUAL_INT(MODE_CONTROLLED,ldg::operatingMode(tuner.getState()));
            TEST_ASSERT_EQUAL_INT(fallback?MODE_AUTO:MODE_MANUAL,tuner.getMode());
            for (int i=0;i<40;++i) {
                testMillis+=100; feed(tuner,frame(forward,reflected,band)); tuner.serviceAntennas();
            }
            TEST_ASSERT_EQUAL_STRING(expected.c_str(),commands.c_str()); // Constant mismatch never loops.
        }
        forward=reflected=0;
        for (int i=0;i<3;++i) { testMillis+=1250; feed(tuner,frame(0,0,band)); tuner.serviceAntennas(); }
        if (fallback) expected+='M';
        TEST_ASSERT_EQUAL_STRING(expected.c_str(),commands.c_str());
        TEST_ASSERT_EQUAL_INT(MODE_MANUAL,tuner.getMode());
        forward=150; reflected=10;
        for (int i=0;i<3;++i) { testMillis+=100; feed(tuner,frame(forward,reflected,band)); tuner.serviceAntennas(); }
        expected+='T'; TEST_ASSERT_EQUAL_STRING(expected.c_str(),commands.c_str());
        TEST_ASSERT_EQUAL_UINT(6,memories); TEST_ASSERT_EQUAL_UINT(1,antennas);
    }
}

void test_controlled_20_to_160_waits_for_confirmed_band_and_selects_before_tuning_in_one_tx() {
    for (bool continuous : {false,true}) {
        HardwareSerial::written=nullptr;
        TunerProtocol tuner; begin(tuner);
        uint16_t forward=0, reflected=0, wireBand=1160;
        uint8_t antenna=2;
        unsigned memories=0, antennas=0, autos=0;
        std::string commands;
        bool delayedMeter=false;
        HardwareSerial::written=[&](HardwareSerial& serial,uint8_t cmd) {
            respondSync(serial,cmd);
            if (cmd=='M') { commands+='M'; serial.bytes.push_back('M'); }
            if (cmd=='A') {
                commands+='A'; ++antennas; antenna=antenna==1?2:1;
                serial.bytes.push_back(antenna==1?'1':'2');
            }
            if (cmd=='T') { commands+='T'; ++memories; serial.bytes.push_back('T'); }
            if (cmd=='C') { commands+='C'; ++autos; serial.bytes.push_back('A'); }
            if (cmd=='S') {
                if (delayedMeter) {
                    for (int i=0;i<6;++i) serial.schedule(700+i*50,frame(forward,reflected,wireBand));
                } else for (auto b:frame(forward,reflected,wireBand)) serial.bytes.push_back(b);
            }
        };
        tuner.manualCommandStarted(); TEST_ASSERT_TRUE(tuner.selectAntenna(ANT_B));
        tuner.manualCommandComplete(TUNER_CMD_SELECT_ANT_B,true);
        ldg::AntennaSettings table; table.enabled=1;
        table.bands[BAND_20M]=ANT_B; table.bands[BAND_160M]=ANT_A;
        tuner.configureAntennas(table);
        ldg::TuningSettings profile; profile.managed=1; profile.autoFallback=1;
        tuner.configureTuning(profile);
        feed(tuner,frame(0,0,wireBand)); tuner.serviceAntennas();
        forward=150;
        for (int i=0;i<6;++i) { testMillis+=50; feed(tuner,frame(forward,0,wireBand)); tuner.serviceAntennas(); }
        TEST_ASSERT_EQUAL_UINT(1,memories); TEST_ASSERT_EQUAL_INT(ANT_B,tuner.getAntenna());
        TEST_ASSERT_EQUAL_INT(BAND_20M,tuner.getState().antennaAutoBand);
        if (!continuous) {
            forward=0;
            for (int i=0;i<9;++i) { testMillis+=200; feed(tuner,frame(0,0,wireBand)); tuner.serviceAntennas(); }
        }
        const unsigned before=memories, toggles=antennas;
        commands.clear(); forward=150; reflected=60; wireBand=8964;
        // Early RF carries an unknown band. Positive power alone must not send T
        // while the band-table machine still retains 20m / ANT2.
        for (uint16_t band : {uint16_t(0),uint16_t(0),wireBand}) {
            testMillis+=50; feed(tuner,frame(forward,reflected,band)); tuner.serviceAntennas();
            TEST_ASSERT_EQUAL_UINT(before,memories);
            TEST_ASSERT_EQUAL_UINT(toggles,antennas);
        }
        delayedMeter=true;
        for (int i=0;i<2;++i) { testMillis+=50; feed(tuner,frame(forward,reflected,wireBand)); tuner.serviceAntennas(); }
        const uint32_t started=testMillis;
        while (testMillis-started<6000 && !autos) {
            testMillis+=25; tuner.process(); tuner.serviceAntennas();
        }
        TEST_ASSERT_EQUAL_INT(ANT_A,tuner.getAntenna());
        TEST_ASSERT_EQUAL_INT(BAND_160M,tuner.getState().antennaAutoBand);
        TEST_ASSERT_EQUAL_UINT(toggles+1,antennas);
        TEST_ASSERT_EQUAL_UINT(before+1,memories);
        TEST_ASSERT_EQUAL_UINT(1,autos);
        TEST_ASSERT_EQUAL_STRING("ATC",commands.c_str());
        TEST_ASSERT_EQUAL_INT(MODE_CONTROLLED,ldg::operatingMode(tuner.getState()));
        TEST_ASSERT_EQUAL_INT(MODE_AUTO,tuner.getMode());
    }
}

void test_controlled_pending_band_survives_delayed_post_manual_meter_without_another_rf_start() {
    HardwareSerial::written=nullptr;
    TunerProtocol tuner; begin(tuner);
    uint16_t forward=0, reflected=0, wireBand=1160;
    uint8_t antenna=2;
    bool delayAfterManual=false;
    unsigned memories=0, antennas=0, autos=0;
    std::string commands;
    HardwareSerial::written=[&](HardwareSerial& serial,uint8_t cmd) {
        respondSync(serial,cmd);
        if (cmd=='M') { commands+='M'; serial.bytes.push_back('M'); }
        if (cmd=='A') { commands+='A'; ++antennas; antenna=antenna==1?2:1; serial.bytes.push_back(antenna==1?'1':'2'); delayAfterManual=false; }
        if (cmd=='T') { commands+='T'; ++memories; serial.bytes.push_back('T'); }
        if (cmd=='C') { commands+='C'; ++autos; serial.bytes.push_back('A'); }
        if (cmd=='S') {
            if (delayAfterManual) {
                for (int i=0;i<6;++i) serial.schedule(2700+i*50,frame(forward,reflected,wireBand));
            } else for (auto b:frame(forward,reflected,wireBand)) serial.bytes.push_back(b);
        }
    };
    tuner.manualCommandStarted(); TEST_ASSERT_TRUE(tuner.selectAntenna(ANT_B));
    tuner.manualCommandComplete(TUNER_CMD_SELECT_ANT_B,true);
    ldg::AntennaSettings table; table.enabled=1; table.bands[BAND_20M]=ANT_B; table.bands[BAND_160M]=ANT_A;
    tuner.configureAntennas(table);
    ldg::TuningSettings profile; profile.managed=1; profile.autoFallback=1; tuner.configureTuning(profile);
    feed(tuner,frame(0,0,wireBand)); tuner.serviceAntennas();
    forward=150; reflected=60;
    for (int i=0;i<8;++i) { testMillis+=50; feed(tuner,frame(forward,reflected,wireBand)); tuner.serviceAntennas(); }
    TEST_ASSERT_EQUAL_UINT(1,memories); TEST_ASSERT_EQUAL_UINT(1,autos); TEST_ASSERT_EQUAL_INT(MODE_AUTO,tuner.getMode());
    const unsigned toggles=antennas;
    commands.clear(); wireBand=8964; delayAfterManual=true;
    for (int i=0;i<3;++i) { testMillis+=50; feed(tuner,frame(forward,reflected,wireBand)); tuner.serviceAntennas(); }
    TEST_ASSERT_EQUAL_STRING("M",commands.c_str());
    TEST_ASSERT_EQUAL_UINT(1,memories); TEST_ASSERT_EQUAL_UINT(toggles,antennas);
    const uint32_t started=testMillis;
    while (testMillis-started<7000 && autos<2) {
        testMillis+=25; tuner.process(); tuner.serviceAntennas();
    }
    TEST_ASSERT_EQUAL_STRING("MATC",commands.c_str());
    TEST_ASSERT_EQUAL_INT(ANT_A,tuner.getAntenna());
    TEST_ASSERT_EQUAL_UINT(toggles+1,antennas); TEST_ASSERT_EQUAL_UINT(2,memories); TEST_ASSERT_EQUAL_UINT(2,autos);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_controlled_pending_band_survives_delayed_post_manual_meter_without_another_rf_start);
    RUN_TEST(test_controlled_20_to_160_waits_for_confirmed_band_and_selects_before_tuning_in_one_tx);
    RUN_TEST(test_controlled_late_intraband_mismatch_repeats_memory_then_optional_auto_without_antenna_switches);

    RUN_TEST(test_three_operating_modes_are_exclusive_and_controlled_never_reaches_uart);
    RUN_TEST(test_failed_mode_persistence_does_not_change_policy_or_send_a_command);
    RUN_TEST(test_manual_command_queued_during_antenna_selection_cancels_unstarted_memory_tune);
    RUN_TEST(test_managed_fallback_retries_once_but_not_after_rf_becomes_unsafe);
    RUN_TEST(test_managed_mode_tunes_every_tx_without_reselecting_same_band_or_restoring_native_auto);
    RUN_TEST(test_managed_mode_does_not_tune_without_confirmed_port_mode_or_safe_rf);
    RUN_TEST(test_tci_checks_the_final_relay_write_and_defers_tx_stale_power_and_source_loss);
    RUN_TEST(test_tci_startup_probe_defers_restoring_original_port_when_tx_starts);
    RUN_TEST(test_auto_band_selection_guards_mode_and_restores_it_even_after_failures);
    RUN_TEST(test_failed_old_auto_tune_keeps_first_band_choice_and_recovers_without_a_successful_match);
    RUN_TEST(test_auto_antenna_change_retunes_once_after_ack_and_before_restoring_auto);
    RUN_TEST(test_unknown_tuner_mode_preserves_mode_and_selects_only_after_rf_off);
    RUN_TEST(test_enabled_automation_confirms_startup_auto_once_after_probe_and_respects_manual_priority);
    RUN_TEST(test_meter_watchdog_recovers_lost_start_with_backoff_and_respects_stop_and_manual_queue);
    RUN_TEST(test_stalled_meter_after_selection_recovers_before_detecting_next_band_and_preserves_manual_choice);
    RUN_TEST(test_failed_auto_restore_retries_once_at_fresh_zero_and_yields_to_manual_commands);
    RUN_TEST(test_antenna_timeout_preserves_last_port_without_claiming_current_confirmation);
    RUN_TEST(test_failed_recovery_before_antenna_command_preserves_the_confirmed_port);
    RUN_TEST(test_just_late_antenna_reply_is_quarantined_and_guards_meter_restart);
    RUN_TEST(test_antenna_and_recovery_sync_accept_delayed_replies_without_repeating_a_toggle);
    RUN_TEST(test_antenna_startup_probe_restores_both_ports_and_respects_ack_guard);
    RUN_TEST(test_startup_missing_either_ack_never_blindly_retries_and_manual_selection_recovers);
    RUN_TEST(test_real_wire_band_selection_is_one_shot_and_never_restores_a_manual_choice);
    RUN_TEST(test_tune_response_and_uart_failure_are_not_success);
    RUN_TEST(test_meter_timeout_and_recovery_survive_clock_wrap);
    RUN_TEST(test_last_band_survives_rf_silence_and_unknown_band_packets);
    RUN_TEST(test_absolute_selection_uses_numeric_ack_and_does_not_repeat_toggle);
    RUN_TEST(test_idle_telemetry_does_not_forget_confirmed_controls_or_toggle_the_same_antenna);
    RUN_TEST(test_startup_wait_preserves_a_full_rate_uart_burst);
    RUN_TEST(test_uart_fault_discards_the_damaged_batch_before_resynchronizing);
    RUN_TEST(test_uart_fault_during_an_ack_requires_recovery_instead_of_reporting_success);
    RUN_TEST(test_uart_diagnostics_are_bounded_copies_in_wire_order_for_any_read_size);
    RUN_TEST(test_invalid_trailers_are_counted_separately_from_driver_errors);
    RUN_TEST(test_command_specific_acknowledgements);
    RUN_TEST(test_mode_commands_do_not_inherit_a_fault_from_the_drained_meter_batch);
    RUN_TEST(test_command_trace_keeps_the_actual_reply_after_metering_overwrites_the_live_rx_tail);
    RUN_TEST(test_command_trace_distinguishes_no_reply_unexpected_reply_and_blocking_sync);
    RUN_TEST(test_command_trace_is_bounded_and_keeps_uart_faults_without_accepting_the_ack);
    RUN_TEST(test_command_trace_distinguishes_write_failure_and_meter_restart_failure);
    RUN_TEST(test_full_and_memory_tune_wait_for_complete_long_cycle);
    RUN_TEST(test_mode_commands_wait_for_delayed_ack_before_restarting_metering);
    RUN_TEST(test_mode_timeout_remains_bounded_and_rejects_late_ack_across_clock_wrap);
    RUN_TEST(test_post_ack_guard_includes_wake_and_survives_clock_wrap);
    RUN_TEST(test_sync_requires_complete_signature_and_rejects_wrong_case);
    RUN_TEST(test_sync_streamed_and_fragmented_replies_finish_before_meter_restart);
    RUN_TEST(test_sync_finds_signature_after_noise_and_overlapping_zero_prefix);
    RUN_TEST(test_unknown_responses_never_establish_connection);
    RUN_TEST(test_noise_before_expected_ack_is_ignored_but_failed_tune_proves_communication);
    RUN_TEST(test_timeout_requires_sync_before_next_command_and_discards_late_ack);
    RUN_TEST(test_failed_sync_blocks_new_command_until_explicit_recovery);
    RUN_TEST(test_failure_to_restore_metering_is_reported_and_uncertain_mode_is_cleared);
    RUN_TEST(test_cached_antenna_does_not_skip_recovery_after_meter_restart_failure);
    RUN_TEST(test_meter_all_chunk_partitions_and_consecutive_frames);
    RUN_TEST(test_meter_resynchronizes_after_single_byte_corruption);
    RUN_TEST(test_meter_payload_semicolons_and_band_boundaries);
    RUN_TEST(test_swr_valid_measurements_and_invalid_rf_stay_unknown_in_json);
    return UNITY_END();
}
