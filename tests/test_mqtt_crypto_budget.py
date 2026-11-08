"""Exercise the production MPI wrapper: task ownership, unlock order and clock."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <cassert>
#include <cstdint>
#include "mqtt_crypto_budget.h"
uint32_t now = 0, pauses = 0, releases = 0;
uintptr_t task = 7;
bool locked = false;
#include "freertos/task.h"
TaskHandle_t xTaskGetCurrentTaskHandle() { return reinterpret_cast<void*>(task); }
int64_t esp_timer_get_time() { return now; }
void vTaskDelay(unsigned ticks) {
    assert(!locked && ticks == 1);
    ++pauses; now += 1000;
}
extern "C" void __real_esp_crypto_mpi_lock_release() {
    assert(locked); locked = false; ++releases;
}
#include "mqtt_crypto_budget.cpp"
static void release() { locked = true; __wrap_esp_crypto_mpi_lock_release(); }
int main() {
    // No registered TLS task: SDK unlock still runs, without delay/counters.
    release(); assert(releases == 1 && !pauses);
    assert(!ldg::mqttCryptoStats().checkpoints);
    {
        ldg::MqttCryptoScope plain(false);
        release(); assert(!ldg::mqttCryptoStats().active && !pauses);
    }
    {
        ldg::MqttCryptoScope tls(true);
        assert(ldg::mqttCryptoStats().active);
        now = 1999; release(); assert(!pauses);
        task = 8; now = 9000; release(); assert(!pauses);
        assert(ldg::mqttCryptoStats().checkpoints == 1);
        task = 7; release(); assert(pauses == 1);
        assert(ldg::mqttCryptoStats().maxGapUs == 9000);
        release(); assert(pauses == 1); // pause time is excluded from next slice
        now += 1999; release(); assert(pauses == 1);
        ++now; release(); assert(pauses == 2);
        assert(ldg::mqttCryptoStats().checkpoints == 5);
    } // Success/error/early return all unregister through the same destructor.
    assert(!ldg::mqttCryptoStats().active);
    auto previous = ldg::mqttCryptoStats();
    now += 10000; release(); assert(pauses == 2);
    assert(ldg::mqttCryptoStats().checkpoints == previous.checkpoints);
    // A fresh connection resets its clock and handles uint32 microsecond wrap.
    now = UINT32_MAX - 999;
    {
        ldg::MqttCryptoScope tls(true);
        now += 2000; release(); assert(pauses == 3);
        assert(ldg::mqttCryptoStats().yields == 3);
    }
    task = 0;
    ldg::beginMqttCryptoBudget(); release(); assert(pauses == 3);
    ldg::endMqttCryptoBudget();
}
'''


@unittest.skipUnless(shutil.which("c++"), "host C++ compiler required")
class MqttCryptoBudgetTests(unittest.TestCase):
    def test_production_wrapper_unlocks_before_yield_and_only_for_tls_owner(self):
        with tempfile.TemporaryDirectory(prefix="ldg-crypto-budget-") as directory:
            root = Path(directory)
            (root / "freertos").mkdir()
            (root / "sdkconfig.h").write_text("#define CONFIG_IDF_TARGET_ESP32 1\n")
            (root / "freertos/FreeRTOS.h").write_text("#pragma once\n")
            (root / "freertos/task.h").write_text(
                "#pragma once\nusing TaskHandle_t=void*;\n"
                "TaskHandle_t xTaskGetCurrentTaskHandle();\nvoid vTaskDelay(unsigned);\n"
            )
            (root / "esp_timer.h").write_text(
                "#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time();\n"
            )
            (root / "check.cpp").write_text(HARNESS)
            subprocess.run(
                ["c++", "-std=c++11", "-DLDG_MQTT_CRYPTO_BUDGET=1",
                 "-I", str(root), "-I", str(ROOT / "include"),
                 "-I", str(ROOT / "src"), str(root / "check.cpp"),
                 "-o", str(root / "check")],
                check=True, capture_output=True,
            )
            result = subprocess.run([str(root / "check")], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr.decode())


if __name__ == "__main__":
    unittest.main()
