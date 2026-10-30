#ifndef SECURITY_H
#define SECURITY_H

#include <Arduino.h>
#include <WiFi.h>
#include "config.h"

// Rate limiter for web requests
class RateLimiter {
public:
    RateLimiter(uint16_t maxRequests, uint32_t windowMs);
    bool allowRequest(const IPAddress& clientIP);
    void reset();

private:
    struct ClientRecord {
        IPAddress ip;
        uint16_t count;
        uint32_t windowStart;
        bool used;
    };

    static const uint8_t MAX_CLIENTS = 10;
    ClientRecord m_clients[MAX_CLIENTS];
    uint16_t m_maxRequests;
    uint32_t m_windowMs;

    ClientRecord* findClient(const IPAddress& ip);
    ClientRecord* getFreeSlot();
};

// Login attempt tracker
class LoginTracker {
public:
    LoginTracker(uint8_t maxAttempts, uint32_t lockoutMs);
    bool canAttempt(const IPAddress& clientIP);
    void recordSuccess(const IPAddress& clientIP);
    void recordFailure(const IPAddress& clientIP);
    void reset();

private:
    struct LoginRecord {
        IPAddress ip;
        uint8_t failures;
        uint32_t lockoutUntil;
        bool used;
        bool locked;
    };

    static const uint8_t MAX_RECORDS = 10;
    LoginRecord m_records[MAX_RECORDS];
    uint8_t m_maxAttempts;
    uint32_t m_lockoutMs;

    LoginRecord* findRecord(const IPAddress& ip);
    LoginRecord* getFreeSlot();
};

// Security utilities
class Security {
public:
    enum class Authorization { Allowed, RateLimited, Locked, Unauthorized };
    static Authorization authorize(const IPAddress& clientIP, const String& username,
                                   const String& password);
    // Initialize security subsystem
    static void begin();

    // Verify HTTP basic auth credentials
    static bool verifyAuth(const String& username, const String& password);

    // Generate a random session token.
    static String generateToken();

    // Validate CSRF token
    static bool validateToken(const String& token);

    // Get rate limiter instance
    static RateLimiter& getRateLimiter();

    // Get login tracker instance
    static LoginTracker& getLoginTracker();

    // Check if client is authorized (rate limit + login + CSRF)
    static bool isAuthorized(const IPAddress& clientIP, const String& username,
                            const String& password, const String& csrfToken = "");

private:
    static RateLimiter* s_rateLimiter;
    static LoginTracker* s_loginTracker;
    static String s_currentToken;
    static uint32_t s_tokenExpiry;
};

#endif // SECURITY_H
