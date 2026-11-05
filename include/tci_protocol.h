#pragma once

#include "radio_band.h"
#include <cstring>
#include <cstdio>

namespace ldg {
// Streaming ASCII parser. A command may span text frames/continuations; no
// unbounded String, audio allocation, or network calls belong to this class.
class TciProtocol {
public:
    static constexpr unsigned MaxReceivers = 16;
    void connect(uint8_t receiver, uint32_t now) {
        *this = TciProtocol();
        m_state.receiver = receiver;
        m_state.connection = RadioConnection::Synchronizing;
        m_aliveAt = m_bandAt = now;
    }
    void disconnect(bool enabled, const char* error = "") {
        *this = TciProtocol();
        m_state.connection = enabled ? RadioConnection::Disconnected : RadioConnection::Disabled;
        copy(m_state.error, error);
    }
    void connecting() { m_state.connection = RadioConnection::Connecting; }
    void heartbeat(uint32_t now) { m_aliveAt = now; }
    bool initialized() const { return m_ready && m_state.receiverCount; }
    uint8_t receiverCount() const { return m_state.receiverCount; }

    bool feed(const uint8_t* data, size_t size, uint32_t now) {
        heartbeat(now);
        for (size_t i = 0; i < size; ++i) {
            const unsigned char c = data[i];
            if (!c || (c < 32 && c != '\r' && c != '\n' && c != '\t')) return fail("Invalid TCI text");
            if (c == ';') {
                if (m_overflow) return fail("TCI command exceeds 1024 bytes");
                m_command[m_used] = 0;
                const bool valid = command(now);
                m_used = 0;
                if (!valid) return fail("Malformed TCI state; resynchronizing");
            } else if (m_used < sizeof(m_command) - 1) m_command[m_used++] = char(c);
            else m_overflow = true;
        }
        return true;
    }
    bool feed(const char* text, uint32_t now) {
        return feed(reinterpret_cast<const uint8_t*>(text), strlen(text), now);
    }
    RadioState snapshot(uint32_t now) const {
        auto state = m_state;
        state.ageMs = now - m_aliveAt;
        state.stableMs = now - m_bandAt;
        if (state.ownsBand() && state.ageMs > RadioState::LivenessMs) {
            state.connection = RadioConnection::Disconnected;
            state.frequencyValid = false;
            copy(state.error, "TCI heartbeat timeout");
        }
        return state;
    }

private:
    enum Seen : unsigned { A = 1, B = 2, Split = 4, Xit = 8, Offset = 16 };
    RadioState m_state{};
    char m_command[1025]{};
    size_t m_used = 0;
    bool m_overflow = false, m_ready = false, m_reportedValid = false;
    unsigned m_seen = 0;
    uint32_t m_trxKnown = 0, m_trx = 0, m_tuneKnown = 0, m_tune = 0;
    uint64_t m_vfo[2]{}, m_reported = 0;
    uint32_t m_aliveAt = 0, m_bandAt = 0;
    tuner_band_t m_band = BAND_UNKNOWN;

    template<size_t N> static void copy(char (&to)[N], const char* from) {
        strncpy(to, from, N - 1); to[N - 1] = 0;
    }
    bool fail(const char* error) {
        m_state.connection = RadioConnection::Synchronizing;
        m_state.frequencyValid = false;
        m_state.rxKnown = m_state.tuneKnown = false;
        copy(m_state.error, error);
        return false;
    }
    static char* trim(char* p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        size_t length = strlen(p);
        while (length && (p[length-1] == ' ' || p[length-1] == '\t' || p[length-1] == '\r' || p[length-1] == '\n')) p[--length] = 0;
        return p;
    }
    static bool number(const char* p, uint64_t& value, uint64_t limit) {
        value = 0;
        if (!*p) return false;
        do {
            if (*p < '0' || *p > '9' || value > limit / 10) return false;
            value = value * 10 + unsigned(*p++ - '0');
            if (value > limit) return false;
        } while (*p);
        return true;
    }
    static bool boolean(const char* p, bool& value) {
        char folded[6]{};
        const size_t size = strlen(p);
        if (size >= sizeof(folded)) return false;
        for (size_t i = 0; i < size; ++i) folded[i] = p[i] >= 'A' && p[i] <= 'Z' ? p[i] + ('a' - 'A') : p[i];
        if (!strcmp(folded, "true")) { value = true; return true; }
        if (!strcmp(folded, "false")) { value = false; return true; }
        return false;
    }
    void recompute(uint32_t now) {
        const uint32_t all = m_state.receiverCount ? (uint32_t(1) << m_state.receiverCount) - 1 : 0;
        m_state.rxKnown = all && (m_trxKnown & all) == all;
        m_state.tuneKnown = all && (m_tuneKnown & all) == all;
        m_state.transmitting = m_trx != 0;
        m_state.tuning = m_tune != 0;
        const bool fields = (m_seen & (A | Split | Xit)) == (A | Split | Xit) &&
            (!m_state.split || (m_seen & B)) && (!m_state.xit || (m_seen & Offset));
        const int64_t derived = int64_t(m_vfo[m_state.split ? 1 : 0]) + (m_state.xit ? m_state.xitOffset : 0);
        const bool frequency = fields && derived > 0 && derived <= 1000000000000LL;
        m_state.rxFrequencyHz = m_vfo[0];
        m_state.txFrequencyHz = frequency ? uint64_t(derived) : 0;
        // TX_FREQUENCY is global (no receiver index). Reject a different-band
        // report rather than applying another receiver's antenna selection.
        const bool conflict = frequency && m_reportedValid &&
            frequencyBand(m_reported) != frequencyBand(uint64_t(derived));
        if (frequency && m_reportedValid && !conflict) m_state.txFrequencyHz = m_reported;
        m_state.frequencyValid = frequency && !conflict;
        const bool complete = m_ready && all && m_state.receiver < m_state.receiverCount &&
            m_state.frequencyValid && m_state.rxKnown && m_state.tuneKnown;
        m_state.connection = complete ? RadioConnection::Ready : RadioConnection::Synchronizing;
        copy(m_state.error, conflict ? "TX frequency conflicts with selected RX" : complete ? "" : "Waiting for complete TCI state");
        const auto band = m_state.frequencyValid ? frequencyBand(m_state.txFrequencyHz) : BAND_UNKNOWN;
        if (band != m_band) { m_band = band; m_bandAt = now; }
        ++m_state.revision;
    }
    bool command(uint32_t now) {
        char* name = trim(m_command);
        if (!*name) return true;
        char* arguments = strchr(name, ':');
        if (arguments) *arguments++ = 0;
        name = trim(name);
        for (char* p = name; *p; ++p) if (*p >= 'A' && *p <= 'Z') *p += 'a' - 'A';
        const bool recognized = !strcmp(name,"ready") || !strcmp(name,"trx_count") || !strcmp(name,"vfo") ||
            !strcmp(name,"split_enable") || !strcmp(name,"xit_enable") || !strcmp(name,"xit_offset") ||
            !strcmp(name,"trx") || !strcmp(name,"tune") || !strcmp(name,"tx_frequency") ||
            !strcmp(name,"device") || !strcmp(name,"protocol") || !strcmp(name,"modulation") ||
            !strcmp(name,"stop") || !strcmp(name,"start");
        if (!recognized) return true;
        char* args[4]{}; unsigned count = 0;
        while (arguments) {
            if (count == 4) return false;
            args[count++] = trim(arguments);
            arguments = strchr(arguments, ',');
            if (arguments) *arguments++ = 0;
            args[count-1] = trim(args[count-1]);
            if (!*args[count-1]) return false;
        }
        uint64_t index = 0, value = 0, channel = 0; bool flag = false;
        if (!strcmp(name,"ready")) { if (count) return false; m_ready = true; }
        else if (!strcmp(name,"stop")) { if (count) return false; m_ready = false; m_seen = 0; m_trxKnown = m_tuneKnown = 0; m_reportedValid = false; }
        else if (!strcmp(name,"start")) { if (count) return false; m_ready = true; }
        else if (!strcmp(name,"device")) { if (count != 1) return false; copy(m_state.device, args[0]); }
        else if (!strcmp(name,"protocol")) {
            if (count != 2) return false;
            snprintf(m_state.protocol, sizeof(m_state.protocol), "%.15s,%.7s", args[0], args[1]);
        }
        else if (!strcmp(name,"trx_count")) {
            if (count != 1 || !number(args[0],value,MaxReceivers) || !value || m_state.receiver >= value) return false;
            m_state.receiverCount = value;
        }
        else if (!strcmp(name,"tx_frequency")) {
            if (count != 1 || !number(args[0],value,1000000000000ULL) || !value) return false;
            m_reported = value; m_reportedValid = true;
        } else {
            if (count < 2 || !number(args[0],index,MaxReceivers-1) ||
                (m_state.receiverCount && index >= m_state.receiverCount)) return false;
            const bool selected = index == m_state.receiver;
            if (!strcmp(name,"vfo")) {
                if (count != 3 || !number(args[1],channel,1) || !number(args[2],value,1000000000000ULL) || !value) return false;
                if (selected) {
                    if (!(m_seen & (channel ? B : A)) || m_vfo[channel] != value) m_reportedValid = false;
                    m_vfo[channel] = value; m_seen |= channel ? B : A;
                }
            } else if (!strcmp(name,"modulation")) {
                if (count != 2) return false;
                if (selected) copy(m_state.modulation, args[1]);
            } else if (!strcmp(name,"xit_offset")) {
                const char* p = args[1]; const bool negative = *p == '-';
                if (*p == '-' || *p == '+') ++p;
                if (count != 2 || !number(p,value,1000000000)) return false;
                if (selected) {
                    const int32_t offset = negative ? -int32_t(value) : int32_t(value);
                    if (!(m_seen & Offset) || m_state.xitOffset != offset) m_reportedValid = false;
                    m_state.xitOffset = offset; m_seen |= Offset;
                }
            } else {
                if ((count != 2 && !(count == 3 && !strcmp(name,"trx"))) || !boolean(args[1],flag)) return false;
                const uint32_t bit = uint32_t(1) << index;
                if (!strcmp(name,"trx")) { m_trxKnown |= bit; if (flag) m_trx |= bit; else m_trx &= ~bit; }
                else if (!strcmp(name,"tune")) { m_tuneKnown |= bit; if (flag) m_tune |= bit; else m_tune &= ~bit; }
                else if (selected && !strcmp(name,"split_enable")) {
                    if (!(m_seen & Split) || m_state.split != flag) m_reportedValid = false;
                    m_state.splitKnown = true; m_state.split = flag; m_seen |= Split;
                } else if (selected && !strcmp(name,"xit_enable")) {
                    if (!(m_seen & Xit) || m_state.xit != flag) m_reportedValid = false;
                    m_state.xitKnown = true; m_state.xit = flag; m_seen |= Xit;
                }
            }
        }
        recompute(now); return true;
    }
};
}
