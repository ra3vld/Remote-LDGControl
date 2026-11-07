"""Check the public Wi-Fi override preserves unrelated SDK configuration."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
HARNESS=r'''
#include <cassert>
#include <initializer_list>
#include "esp_wifi.h"
wifi_init_config_t observed{};int calls=0;
extern "C" esp_err_t __real_esp_wifi_init(const wifi_init_config_t* p){++calls;if(p)observed=*p;return p?17:19;}
#include "wifi_memory.cpp"
int main(){
 assert(__wrap_esp_wifi_init(nullptr)==19 && calls==1);
 for(int dynamic:{0,1})for(int count:{2,32}){
  wifi_init_config_t config{dynamic,count,count,1,1,123};auto original=config;
  assert(__wrap_esp_wifi_init(&config)==17);
  assert(config.dynamic_tx_buf_num==original.dynamic_tx_buf_num && config.ampdu_rx_enable==1);
  assert(observed.dynamic_tx_buf_num==(dynamic && count>8?8:count));
  assert(observed.dynamic_rx_buf_num==(count>8?8:count));
  assert(observed.sentinel==123 && !observed.ampdu_rx_enable && !observed.ampdu_tx_enable);
 }
}
'''
@unittest.skipUnless(shutil.which('c++'),'host C++ compiler required')
class NetworkBudgetTests(unittest.TestCase):
 def test_public_buffer_limits_preserve_original_config_and_driver_errors(self):
  with tempfile.TemporaryDirectory(prefix='ldg-network-budget-') as directory:
   root=Path(directory)
   (root/'esp_wifi.h').write_text('#pragma once\n#include <cstddef>\nusing esp_err_t=int;struct wifi_init_config_t{int tx_buf_type,dynamic_tx_buf_num,dynamic_rx_buf_num,ampdu_rx_enable,ampdu_tx_enable,sentinel;};\n')
   shutil.copyfile(ROOT/'src/wifi_memory.cpp',root/'wifi_memory.cpp');(root/'check.cpp').write_text(HARNESS)
   subprocess.run(['c++','-std=c++11','-DARDUINO_ARCH_ESP32','-DBOARD_ESP32_3248S035C','-I',str(root),str(root/'check.cpp'),'-o',str(root/'check')],check=True,capture_output=True)
   result=subprocess.run([str(root/'check')],capture_output=True)
   self.assertEqual(result.returncode,0,result.stderr.decode())
