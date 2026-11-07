#!/usr/bin/env python3
"""Regenerate/check the compact default bundle with the pinned Espressif tool."""
from pathlib import Path
import argparse,os,subprocess,sys,tempfile
p=argparse.ArgumentParser();p.add_argument('--check',action='store_true');args=p.parse_args()
r=Path(__file__).resolve().parents[1]
core=Path(os.environ.get('PLATFORMIO_CORE_DIR',Path.home()/'.platformio'))
generator=core/'packages-pioarduino-55.03.312-1/framework-arduinoespressif32/tools/gen_crt_bundle.py'
with tempfile.TemporaryDirectory(prefix='ldg-mqtt-ca-') as d:
    subprocess.run([sys.executable,str(generator),'--input',*[str(p) for p in sorted((r/'certs/mqtt').glob('*.der'))]],cwd=d,check=True)
    b=(Path(d)/'x509_crt_bundle').read_bytes()
header='''/* ISRG Root X1/X2, generated with Espressif gen_crt_bundle.py from certs/mqtt.
 * Compact default bundle for verified MQTT TLS; preserves the existing OTA map.
 * Certificate public data is distributed under Mozilla Public License 2.0.
 */
.section .rodata.mqtt_ca_bundle,"a"
.balign 4
.global _binary_x509_crt_bundle_start
.global _binary_x509_crt_bundle_end
_binary_x509_crt_bundle_start:
'''
s=header+''.join('.byte '+','.join(str(v) for v in b[i:i+16])+'\n' for i in range(0,len(b),16))+'_binary_x509_crt_bundle_end:\n'
p=r/'src/mqtt_ca_bundle.S'
if args.check:
    if p.read_text()!=s:raise SystemExit('MQTT CA bundle is stale; run scripts/build_mqtt_ca.py')
else:p.write_text(s)
