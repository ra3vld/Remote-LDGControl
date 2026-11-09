import argparse,hashlib,importlib.util,json,shutil,struct,sys,tempfile,unittest,zipfile
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import web_flasher as wf
import update_web_flasher_site as update

class WebFlasher(unittest.TestCase):
    def setUp(self):
        self.ram_patch=patch.object(wf,'elf_memory',return_value=1024);self.ram_patch.start();self.addCleanup(self.ram_patch.stop)
        self.version_patch=patch.object(wf,'version',return_value='1.2.3');self.version_patch.start();self.addCleanup(self.version_patch.stop)
        self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name);self.build=self.root/'build';self.folder=self.build/'esp32-3248s035c';self.folder.mkdir(parents=True)
        self.boot=self.root/'boot_app0.bin';self.boot.write_bytes(b'boot'+b'\xff'*128)
        self.meta=dict(schema=1,family='tuner',profile='esp32-3248s035c',chipFamily='ESP32',role='tuner_controller',display='ST7796/GT911',capabilities={'usb_provisioning':True,'mqtt':True},commit=wf.git('rev-parse','HEAD'),version='1.2.3',source_digest=wf.source_digest())
        self.folder.joinpath('firmware.elf').write_bytes(b'ELF\0LDG_METADATA_V1:'+json.dumps(self.meta).encode()+b'\n')
        self.folder.joinpath('firmware.bin').write_bytes(b'app'*64);self.folder.joinpath('bootloader.bin').write_bytes(b'bootloader');self.folder.joinpath('littlefs.bin').write_bytes(b'littlefs')
        table=b''
        for name,kind,subtype,offset,size in [('nvs',1,2,0x9000,0x5000),('otadata',1,0,0xe000,0x2000),('app0',0,16,0x10000,0x1c0000),('spiffs',1,130,0x390000,0x60000)]:table+=struct.pack('<HBBII16sI',0x50aa,kind,subtype,offset,size,name.encode(),0)
        self.folder.joinpath('partitions.bin').write_bytes(table)
        self.output=self.root/'site';self.args=argparse.Namespace(output=str(self.output),build_root=str(self.build),boot_app0=str(self.boot),allow_missing=True,allow_dirty=True)
    def tearDown(self):self.temp.cleanup()
    def test_real_parts_include_littlefs_and_preserve_nvs(self):
        wf.assemble(self.args);catalog=wf.check_catalog(self.output);parts=catalog['profiles'][0]['build']['parts']
        with zipfile.ZipFile(self.output/'sources.zip') as z:
            self.assertIn('certs/mqtt/ISRG_Root_X2.der',z.namelist());self.assertNotIn('certs/mqtt/ISRG_Root_X2.pem',z.namelist())
        self.assertEqual([p['offset'] for p in parts],[0x1000,0x8000,0xe000,0x10000,0x390000]);self.assertEqual(parts[-1]['path'],'littlefs.bin')
        manifest=json.loads((self.output/'firmware/esp32-3248s035c/manifest.json').read_text())
        self.assertIs(manifest['new_install_prompt_erase'],True);self.assertEqual(manifest['new_install_improv_wait_time'],0)
        self.output.joinpath('firmware/esp32-3248s035c/littlefs.bin').write_bytes(b'tampered')
        with self.assertRaisesRegex(ValueError,'checksum'):wf.check_catalog(self.output)
    def test_publishable_sources_must_be_tracked(self):
        self.args.allow_dirty=False
        original=wf.git
        def controlled(*args):
            if args[0]=='ls-files':return original(*args).replace('src/main.cpp\0','')
            if args[0]=='status':return ''
            return original(*args)
        with patch.object(wf,'git',side_effect=controlled):
            with self.assertRaisesRegex(ValueError,'Untracked firmware'):wf.assemble(self.args)

    def test_bin_size_not_only_platformio_summary_is_enforced(self):
        self.folder.joinpath('firmware.bin').write_bytes(b'x'*(0x1c0000+1))
        with self.assertRaisesRegex(ValueError,'oversized'):wf.assemble(self.args)
    def test_missing_littlefs_and_build_identity_cannot_publish(self):
        self.folder.joinpath('littlefs.bin').unlink()
        with self.assertRaisesRegex(ValueError,'Missing'):wf.assemble(self.args)
        self.folder.joinpath('littlefs.bin').write_bytes(b'fs');self.meta['source_digest']='0'*64
        self.folder.joinpath('firmware.elf').write_bytes(b'LDG_METADATA_V1:'+json.dumps(self.meta).encode()+b'\n')
        with self.assertRaisesRegex(ValueError,'identity mismatch'):wf.assemble(self.args)
    def test_elf_static_ram_comes_from_sections(self):
        import struct
        self.ram_patch.stop()
        labels=b'\0.shstrtab\0.dram0.data\0.dram0.bss\0.noinit\0'
        data=bytearray(52);data[:6]=b'\x7fELF\x01\x01';struct.pack_into('<I',data,32,52);struct.pack_into('<HHH',data,46,40,5,1)
        for name,kind,offset,size in [('',0,0,0),('.shstrtab',3,252,len(labels)),('.dram0.data',1,0,128),('.dram0.bss',8,0,256),('.noinit',8,0,1)]:
            data+=struct.pack('<IIIIIIIIII',labels.index(name.encode()+b'\0'),kind,0,0,offset,size,0,0,0,0)
        path=self.root/'ram.elf';path.write_bytes(data+labels);self.assertEqual(wf.elf_memory(path),385)

    def test_nvs_overlap_is_rejected(self):
        with self.assertRaisesRegex(ValueError,'NVS'):wf.check_parts([dict(offset=0x9000,size=1)],wf.partitions(self.folder/'partitions.bin'))
    def test_ci_artifact_packages_without_original_build_or_sdk(self):
        staged=self.root/'downloaded-artifacts'
        wf.stage_profile('esp32-3248s035c',self.build,staged,self.boot)
        expected={p.name:wf.sha(p) for p in (staged/'esp32-3248s035c').iterdir()}
        self.assertEqual(set(expected),{'firmware.elf','firmware.bin','littlefs.bin','bootloader.bin','partitions.bin','boot_app0.bin'})
        shutil.rmtree(self.build);self.boot.unlink()
        self.args.build_root=str(staged);self.args.boot_app0=None
        with patch.dict(wf.os.environ,{'PLATFORMIO_CORE_DIR':str(self.root/'no-sdk-installed')}):wf.assemble(self.args)
        wf.check_catalog(self.output)
        for name,digest in expected.items():self.assertEqual(wf.sha(self.output/'firmware/esp32-3248s035c'/name),digest)

    def test_ci_artifact_rejects_wrong_identity_and_missing_files(self):
        output=self.root/'artifacts'
        self.meta['profile']='esp32-display'
        (self.folder/'firmware.elf').write_bytes(b'LDG_METADATA_V1:'+json.dumps(self.meta).encode()+b'\n')
        with self.assertRaisesRegex(ValueError,'identity mismatch'):wf.stage_profile('esp32-3248s035c',self.build,output,self.boot)
        self.assertFalse(output.exists())
        self.meta['profile']='esp32-3248s035c'
        (self.folder/'firmware.elf').write_bytes(b'LDG_METADATA_V1:'+json.dumps(self.meta).encode()+b'\n')
        (self.folder/'littlefs.bin').unlink()
        with self.assertRaisesRegex(ValueError,'Missing'):wf.stage_profile('esp32-3248s035c',self.build,output,self.boot)
        self.assertFalse(output.exists())
        with self.assertRaisesRegex(ValueError,'Unknown'):wf.stage_profile('../unknown',self.build,output,self.boot)

    def test_ci_matrix_uses_catalog_profiles_and_rejects_unknown(self):
        expected=[dict(profile=p['id']) for p in wf.profiles()]
        self.assertEqual(wf.build_matrix(),dict(include=expected))
        for profile in expected:self.assertEqual(wf.build_matrix(profile['profile']),dict(include=[profile]))
        with self.assertRaisesRegex(ValueError,'Unknown'):wf.build_matrix('not-a-board')

    def test_release_tag_must_be_stable_and_point_to_current_build(self):
        for tag in ['v1.2.3-dev.1','1.2.3','v01.2.3']:
            with self.assertRaisesRegex(ValueError,'Stable'):wf.check_release_tag(tag)
        with patch.object(wf,'git',side_effect=['tag-commit','different-head']):
            with self.assertRaisesRegex(ValueError,'does not match'):wf.check_release_tag('v1.2.3')
        with patch.object(wf,'git',side_effect=['same-commit','same-commit']):wf.check_release_tag('v1.2.3')
        with patch.object(wf,'git',side_effect=['same-commit','same-commit']):
            with self.assertRaisesRegex(ValueError,'does not match'):wf.check_release_tag('v1.2.4')

    def test_ui_update_preserves_all_released_firmware_bytes(self):
        wf.assemble(self.args);catalog=json.loads((self.output/'catalog.json').read_text());catalog['preview']=False;catalog['profiles']=catalog['profiles'][:1];(self.output/'catalog.json').write_text(json.dumps(catalog))
        archive=self.root/'web-flasher-site.zip'
        with zipfile.ZipFile(archive,'w') as z:
            for p in self.output.rglob('*'):
                if p.is_file():z.write(p,p.relative_to(self.output))
        checks=self.root/'SHA256SUMS';checks.write_text(wf.sha(archive)+'  '+archive.name+'\n')
        before=update.update(archive,checks,'v1.2.3',self.root/'updated');self.assertIn('catalog.json',before);self.assertIn('sources.zip',before)
        for name,digest in before.items():self.assertEqual(wf.sha(self.root/'updated'/name),digest)
        with patch.object(update,'source_digest',return_value='0'*64):
            with self.assertRaisesRegex(ValueError,'source changed'):update.update(archive,checks,'v1.2.3',self.root/'wrong-source')
        with self.assertRaisesRegex(ValueError,'tag/version'):update.update(archive,checks,'v1.2.4',self.root/'wrong-version')
        checks.write_text('0'*64+'  '+archive.name+'\n')
        with self.assertRaisesRegex(ValueError,'SHA-256'):update.update(archive,checks,'v1.2.3',self.root/'rejected')
    def test_archive_path_escape_is_rejected(self):
        archive=self.root/'web-flasher-site.zip'
        with zipfile.ZipFile(archive,'w') as z:z.writestr('../escape','x')
        checks=self.root/'SHA256SUMS';checks.write_text(wf.sha(archive)+'  '+archive.name+'\n')
        with self.assertRaisesRegex(ValueError,'Unsafe'):update.update(archive,checks,'v1.2.3',self.root/'rejected')

class Version(unittest.TestCase):
    def test_semantic_version_uses_largest_change_and_resets_lower_components(self):
        import firmware_identity as identity
        for messages,expected in [('fix: minor repair\0feat(usb): provisioning\0','1.3.0-dev.2'),('fix!: protocol break\0','2.0.0-dev.1'),('fix: repair\0','1.2.4-dev.1'),('','1.2.3')]:
            with patch.object(identity,'git',side_effect=['v1.2.3',messages]):self.assertEqual(identity.version(),expected)
    def test_source_archive_builds_without_git_metadata(self):
        import firmware_identity as identity
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder);(path/'SOURCE_IDENTITY.json').write_text(json.dumps(dict(version='1.2.3',commit='abc')))
            self.assertEqual(identity.version(root=path),'1.2.3');self.assertEqual(identity.git('rev-parse','HEAD',root=path),'abc')

if __name__=='__main__':unittest.main()
