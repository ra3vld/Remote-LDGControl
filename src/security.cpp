#include "security.h"
#include "config_manager.h"
#include <cstring>

// Static member initialization
RateLimiter* Security::s_rateLimiter = nullptr;
LoginTracker* Security::s_loginTracker = nullptr;
String Security::s_currentToken = "";
uint32_t Security::s_tokenExpiry = 0;

// ============================================================
// RateLimiter Implementation
// ============================================================

RateLimiter::RateLimiter(uint16_t maxRequests, uint32_t windowMs)
    : m_maxRequests(maxRequests), m_windowMs(windowMs) {
    reset();
}

RateLimiter::ClientRecord* RateLimiter::findClient(const IPAddress& ip) {
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (m_clients[i].ip == ip && m_clients[i].used) {
            return &m_clients[i];
        }
    }
    return nullptr;
}

RateLimiter::ClientRecord* RateLimiter::getFreeSlot() {
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (!m_clients[i].used) {
            return &m_clients[i];
        }
    }
    uint8_t oldest = 0;
    for (uint8_t i = 1; i < MAX_CLIENTS; ++i)
        if (uint32_t(millis() - m_clients[i].windowStart) > uint32_t(millis() - m_clients[oldest].windowStart)) oldest = i;
    return &m_clients[oldest];
}

bool RateLimiter::allowRequest(const IPAddress& clientIP) {
    uint32_t now = millis();
    ClientRecord* client = findClient(clientIP);

    if (!client) {
        client = getFreeSlot();
        client->used = true;
        client->ip = clientIP;
        client->count = 0;
        client->windowStart = now;
    }

    // Reset window if expired
    if (now - client->windowStart >= m_windowMs) {
        client->count = 0;
        client->windowStart = now;
    }

    if (client->count >= m_maxRequests) {
        return false;
    }

    client->count++;
    return true;
}

void RateLimiter::reset() {
    for (auto& client : m_clients) client = ClientRecord{};
}

// ============================================================
// LoginTracker Implementation
// ============================================================

LoginTracker::LoginTracker(uint8_t maxAttempts, uint32_t lockoutMs)
    : m_maxAttempts(maxAttempts), m_lockoutMs(lockoutMs) {
    reset();
}

LoginTracker::LoginRecord* LoginTracker::findRecord(const IPAddress& ip) {
    for (uint8_t i = 0; i < MAX_RECORDS; i++) {
        if (m_records[i].used && m_records[i].ip == ip) {
            return &m_records[i];
        }
    }
    return nullptr;
}

LoginTracker::LoginRecord* LoginTracker::getFreeSlot() {
    for (uint8_t i = 0; i < MAX_RECORDS; i++) {
        if (!m_records[i].used || (!m_records[i].locked && m_records[i].failures == 0)) {
            return &m_records[i];
        }
    }
    return &m_records[0];
}

bool LoginTracker::canAttempt(const IPAddress& clientIP) {
    uint32_t now = millis();
    LoginRecord* record = findRecord(clientIP);

    if (!record) {
        return true;
    }

    // Check if lockout has expired
    if (record->locked && int32_t(now - record->lockoutUntil) < 0) {
        return false;
    }

    // Lockout expired, reset
    if (record->locked && int32_t(now - record->lockoutUntil) >= 0) {
        record->failures = 0;
        record->lockoutUntil = 0;
        record->locked = false;
    }

    return true;
}

void LoginTracker::recordSuccess(const IPAddress& clientIP) {
    LoginRecord* record = findRecord(clientIP);
    if (record) {
        record->failures = 0;
        record->lockoutUntil = 0;
        record->locked = false;
    }
}

void LoginTracker::recordFailure(const IPAddress& clientIP) {
    uint32_t now = millis();
    LoginRecord* record = findRecord(clientIP);

    if (!record) {
        record = getFreeSlot();
        record->used = true;
        record->ip = clientIP;
        record->failures = 0;
        record->lockoutUntil = 0;
        record->locked = false;
    }

    record->failures++;

    if (record->failures >= m_maxAttempts) {
        record->lockoutUntil = now + m_lockoutMs;
        record->locked = true;
    }
}

void LoginTracker::reset() {
    for (auto& record : m_records) record = LoginRecord{};
}

// ============================================================
// Security Implementation
// ============================================================

void Security::begin() {
    if (s_rateLimiter) return;
    s_rateLimiter = new RateLimiter(WEB_RATE_LIMIT_REQUESTS, WEB_RATE_LIMIT_WINDOW_MS);
    s_loginTracker = new LoginTracker(MAX_LOGIN_ATTEMPTS, LOGIN_LOCKOUT_MS);
    generateToken();
}

bool Security::verifyAuth(const String& username, const String& password) {
    const DeviceConfig& cfg = configManager.get();
    return (username == cfg.webUsername && password == cfg.webPassword);
}

String Security::generateToken() {
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    char hex[33];
    for (int i = 0; i < 16; ++i) snprintf(hex + 2 * i, 3, "%02x", bytes[i]);
    s_currentToken = String(hex);
    s_tokenExpiry = millis() + 3600000;
    return s_currentToken;
}

bool Security::validateToken(const String& token) {
    if (int32_t(millis() - s_tokenExpiry) >= 0) {
        generateToken();
        return false;
    }
    return (token == s_currentToken);
}

RateLimiter& Security::getRateLimiter() {
    return *s_rateLimiter;
}

LoginTracker& Security::getLoginTracker() {
    return *s_loginTracker;
}

Security::Authorization Security::authorize(const IPAddress& clientIP, const String& username,
                                            const String& password) {
    if (!s_rateLimiter) begin();
    if (!s_rateLimiter->allowRequest(clientIP)) return Authorization::RateLimited;
    if (!s_loginTracker->canAttempt(clientIP)) return Authorization::Locked;
    if (!verifyAuth(username, password)) {
        s_loginTracker->recordFailure(clientIP);
        return Authorization::Unauthorized;
    }
    s_loginTracker->recordSuccess(clientIP);
    return Authorization::Allowed;
}

bool Security::isAuthorized(const IPAddress& clientIP, const String& username,
                           const String& password, const String& csrfToken) {
    return authorize(clientIP, username, password) == Authorization::Allowed &&
        (csrfToken.length() == 0 || validateToken(csrfToken));
}
