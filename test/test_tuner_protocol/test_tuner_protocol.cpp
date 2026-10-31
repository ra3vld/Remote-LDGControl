#include <unity.h>
#include <vector>
#include <string>
#include <utility>
#include "tuner_protocol.h"
#include "voltage_sensor.h"
#include "mqtt_protocol.h"
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
        TEST_ASSERT_EQUAL_UINT(0, doc["last_command"]["rx_bytes"].as<unsigned>());
        TEST_ASSERT_TRUE(doc["last_command"]["ack"].isNull());
        TEST_ASSERT_FALSE(cmd == 'P' ? tuner.bypass() : tuner.setAutoMode());
        TEST_ASSERT_EQUAL_UINT(1, writes); // Recovery Z must succeed before any retry.
        commandStatus(doc, tuner);
        TEST_ASSERT_EQUAL_STRING("Z", doc["last_command"]["exchange"].as<const char*>());
        TEST_ASSERT_EQUAL_UINT(1000, doc["last_command"]["timeout_ms"].as<unsigned>());
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

int main() {
    UNITY_BEGIN();
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
