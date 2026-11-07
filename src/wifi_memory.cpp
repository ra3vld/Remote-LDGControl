#if defined(ARDUINO_ARCH_ESP32) && defined(BOARD_ESP32_3248S035C)
#include <esp_wifi.h>

// Bound transient Wi-Fi heap use through the public ESP-IDF configuration.
// Aggregation is disabled so the RX budget need not cover an AMPDU BA window.
extern "C" esp_err_t __real_esp_wifi_init(const wifi_init_config_t* config);
extern "C" esp_err_t __wrap_esp_wifi_init(const wifi_init_config_t* config) {
    if (!config) return __real_esp_wifi_init(config);
    auto bounded = *config;
    if (bounded.tx_buf_type == 1 && bounded.dynamic_tx_buf_num > 8)
        bounded.dynamic_tx_buf_num = 8;
    if (bounded.dynamic_rx_buf_num > 8) bounded.dynamic_rx_buf_num = 8;
    bounded.ampdu_rx_enable = 0;
    bounded.ampdu_tx_enable = 0;
    return __real_esp_wifi_init(&bounded);
}
#endif
