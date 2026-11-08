#include "mqtt_crypto_budget.h"
#if defined(LDG_MQTT_CRYPTO_BUDGET) && LDG_MQTT_CRYPTO_BUDGET
#include <sdkconfig.h>
#if !CONFIG_IDF_TARGET_ESP32
#error MQTT MPI budget requires the classic ESP32 accelerator release hook
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>

namespace ldg {
static MqttCryptoBudget budget;
void beginMqttCryptoBudget() {
    budget.begin(reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle()),
                 uint32_t(esp_timer_get_time()));
}
void endMqttCryptoBudget() { budget.end(); }
MqttCryptoStats mqttCryptoStats() { return budget.stats(); }
}

extern "C" void __real_esp_crypto_mpi_lock_release();
extern "C" void __wrap_esp_crypto_mpi_lock_release() {
    // Release hardware before sleeping. Only our active MQTT TLS worker yields;
    // Wi-Fi, HTTP TLS and other users keep the SDK's original behaviour.
    __real_esp_crypto_mpi_lock_release();
    ldg::budget.released(reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle()),
        [] { return uint32_t(esp_timer_get_time()); }, [] { vTaskDelay(1); });
}
#endif
