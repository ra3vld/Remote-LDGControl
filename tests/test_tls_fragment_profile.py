"""Verify client MFL negotiation and preservation of optional web TLS defaults."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
HARNESS=r'''
#include <cassert>
#include "mbedtls/ssl.h"
int result=0,defaults=0,fragments=0;
extern "C" int __real_mbedtls_ssl_config_defaults(mbedtls_ssl_config*,int,int,int){++defaults;return result;}
int mbedtls_ssl_conf_max_frag_len(mbedtls_ssl_config* config,unsigned char code){++fragments;config->mfl=code;return 0;}
#include "mqtt_tls_profile.cpp"
int main(){for(auto endpoint:{0,1})for(auto error:{0,-42}){
 mbedtls_ssl_config config{};result=error;defaults=fragments=0;
 assert(__wrap_mbedtls_ssl_config_defaults(&config,endpoint,0,0)==error);
 assert(defaults==1);
 bool expected=!WEB_TLS && endpoint==0 && error==0;
 assert(fragments==(expected?1:0));assert(config.mfl==(expected?4:0));
}}
'''
@unittest.skipUnless(shutil.which('c++'),'host C++ compiler required')
class FragmentProfileTests(unittest.TestCase):
 def test_mfl_is_requested_only_for_compact_client_profiles(self):
  with tempfile.TemporaryDirectory(prefix='ldg-tls-profile-') as directory:
   root=Path(directory);(root/'mbedtls').mkdir()
   (root/'mbedtls/ssl.h').write_text('#pragma once\n#include <initializer_list>\nstruct mbedtls_ssl_config{unsigned char mfl=0;};\n#define MBEDTLS_SSL_IS_CLIENT 0\n#define MBEDTLS_SSL_MAX_FRAG_LEN_4096 4\nint mbedtls_ssl_conf_max_frag_len(mbedtls_ssl_config*,unsigned char);\n')
   shutil.copyfile(ROOT/'src/mqtt_tls_profile.cpp',root/'mqtt_tls_profile.cpp');(root/'check.cpp').write_text(HARNESS)
   for web in (0,1):
    (root/'config.h').write_text('#define TLS_ENABLED '+str(web)+'\n')
    subprocess.run(['c++','-std=c++11','-DARDUINO_ARCH_ESP32','-DBOARD_ESP32_3248S035C','-DWEB_TLS='+str(web),'-I',str(root),str(root/'check.cpp'),'-o',str(root/'check')],check=True,capture_output=True)
    checked=subprocess.run([str(root/'check')],capture_output=True)
    self.assertEqual(checked.returncode,0,checked.stderr.decode())
