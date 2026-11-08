import importlib.util
from pathlib import Path
import struct
import unittest
from cryptography import x509
from cryptography.hazmat.primitives import serialization

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('build_mqtt_ca',ROOT/'scripts/build_mqtt_ca.py')
generator=importlib.util.module_from_spec(spec);spec.loader.exec_module(generator)

class CaBundleTests(unittest.TestCase):
    def test_managed_anchor_is_exactly_the_official_pinned_x2_root(self):
        header=(ROOT/'include/managed_mqtt_ca.h').read_text()
        pem=header.split('R"CA(',1)[1].split(')CA"',1)[0].encode()
        direct=x509.load_pem_x509_certificate(pem).public_bytes(serialization.Encoding.DER)
        self.assertEqual((ROOT/'certs/mqtt/ISRG_Root_X2.der').read_bytes(),direct)
        expected=x509.load_der_x509_certificate((ROOT/'certs/mqtt/ISRG_Root_X2.der').read_bytes()).public_bytes(serialization.Encoding.PEM)
        self.assertEqual(expected,pem)

    def test_sdk_offsets_lengths_and_keys_match_the_pinned_roots(self):
        paths=sorted((ROOT/'certs/mqtt').glob('*.der'))
        roots=sorted((x509.load_der_x509_certificate(p.read_bytes()) for p in paths),key=lambda c:c.subject.public_bytes())
        data=generator.build_bundle(paths)
        first=struct.unpack_from('<I',data)[0]
        self.assertEqual(8,first)
        self.assertEqual(2,len(roots))
        offsets=struct.unpack_from('<II',data)
        for i,root in enumerate(roots):
            offset=offsets[i];name_size,key_size=struct.unpack_from('<HH',data,offset)
            end=offset+4+name_size+key_size
            self.assertEqual(offsets[i+1] if i+1<len(roots) else len(data),end)
            self.assertEqual(root.subject.public_bytes(),data[offset+4:offset+4+name_size])
            expected=root.public_key().public_bytes(serialization.Encoding.DER,serialization.PublicFormat.SubjectPublicKeyInfo)
            self.assertEqual(expected,data[offset+4+name_size:end])
        self.assertEqual((ROOT/'src/mqtt_ca_bundle.S').read_text(),generator.assembly(data))

    def test_old_big_endian_count_is_not_a_valid_sdk_first_offset(self):
        # Regression: Arduino's shipped Python tool still writes this header.
        legacy=struct.pack('>HHH',2,81,550)
        first=struct.unpack_from('<I',legacy)[0]
        self.assertGreater(first,1024)
        self.assertNotEqual(first,8)

    def test_empty_trust_store_is_rejected(self):
        with self.assertRaises(ValueError):generator.build_bundle([])

if __name__=='__main__':unittest.main()
