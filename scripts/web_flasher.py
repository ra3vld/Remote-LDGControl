#!/usr/bin/env python3
"""Package real ESP32 build outputs without filling gaps over the NVS partition."""
import argparse,hashlib,json,os,re,shutil,struct,subprocess,zipfile
from pathlib import Path
from firmware_identity import ROOT,source_digest,source_inputs,git,version

def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def profiles():return json.loads((ROOT/'web-flasher/profiles.json').read_text())['profiles']
def partitions(path):
    result={}
    data=Path(path).read_bytes()
    for at in range(0,len(data)-31,32):
        magic,kind,subtype,offset,size,label,flags=struct.unpack('<HBBII16sI',data[at:at+32])
        if magic!=0x50aa:break
        name=label.split(b'\0')[0].decode('ascii')
        if not size or offset+size>0x400000:raise ValueError('Invalid partition '+name)
        for item in result.values():
            if offset<item['offset']+item['size'] and item['offset']<offset+size:raise ValueError('Overlapping partitions')
        result[name]=dict(type=kind,subtype=subtype,offset=offset,size=size)
    if not {'nvs','app0','spiffs'}<=result.keys():raise ValueError('Missing LDG partitions')
    return result

def embedded_metadata(elf):
    matches=re.findall(rb'LDG_METADATA_V1:(\{[^\n\0]+\})\n',Path(elf).read_bytes())
    if len(matches)!=1:raise ValueError('Missing or ambiguous compiled build metadata')
    return json.loads(matches[0])
def elf_memory(path):
    data=Path(path).read_bytes()
    if data[:6]!=b'\x7fELF\x01\x01' or len(data)<52:raise ValueError('Expected ESP32 ELF32 image')
    offset=struct.unpack_from('<I',data,32)[0];size,count,names=struct.unpack_from('<HHH',data,46)
    if size!=40 or not 0<names<count or offset+count*size>len(data):raise ValueError('Invalid ELF sections')
    sections=[struct.unpack_from('<IIIIIIIIII',data,offset+i*size) for i in range(count)]
    strings=sections[names];labels=data[strings[4]:strings[4]+strings[5]]
    found={labels[x[0]:].split(b'\0',1)[0].decode():x[5] for x in sections}
    if not {'.dram0.data','.dram0.bss'}<=found.keys():raise ValueError('Missing ESP32 RAM sections')
    return sum(found.get(name,0) for name in ('.dram0.data','.dram0.bss','.noinit'))

def check_parts(parts,table):
    nvs=table['nvs'];ranges=[]
    for part in parts:
        start,end=part['offset'],part['offset']+part['size']
        if start<0x1000 or end>0x400000 or start<nvs['offset']+nvs['size'] and nvs['offset']<end:raise ValueError('Part overlaps NVS or flash boundary')
        if any(start<b and a<end for a,b in ranges):raise ValueError('Overlapping image parts')
        ranges.append((start,end))

def assemble(args):
    output=Path(args.output).resolve();output.mkdir(parents=True,exist_ok=True)
    shutil.copytree(ROOT/'web-flasher/images',output/'images',dirs_exist_ok=True)
    digest=source_digest();head=git('rev-parse','HEAD')
    firmware_paths=[str(p.relative_to(ROOT)) for p in source_inputs()]+['web-flasher/profiles.json']
    dirty=bool(git('status','--porcelain','--',*firmware_paths))
    if dirty and not args.allow_dirty:raise ValueError('Commit firmware inputs before packaging a publishable catalog')
    catalog=dict(schema=1,firmware_commit=head,firmware_source_digest=digest,preview=dirty,profiles=[])
    for name in ('index.html','app.js','provisioning.js','serial-api.js','style.css','photos.json'):shutil.copy2(ROOT/'web-flasher'/name,output/name)
    for profile in profiles():
        folder=Path(args.build_root)/profile['id'];item=dict(profile)
        item['documentation']='https://github.com/ra3vld/Remote-LDGControl/blob/'+head+'/'+profile['docs']
        if not (folder/'firmware.elf').exists():
            if not args.allow_missing:raise ValueError('Missing firmware: '+profile['id'])
            item['build']=None;catalog['profiles'].append(item);continue
        try:meta=embedded_metadata(folder/'firmware.elf')
        except ValueError:
            if not args.allow_missing:raise
            item['build']=None;catalog['profiles'].append(item);continue
        if meta['profile']!=profile['id'] or meta['source_digest']!=digest or meta['commit']!=head or meta['version']!=version() or meta['family']!='tuner':raise ValueError('Build/source identity mismatch: '+profile['id'])
        table=partitions(folder/'partitions.bin');app=table['app0'];fs=table['spiffs']
        destination=output/'firmware'/profile['id'];destination.mkdir(parents=True,exist_ok=True)
        boot=Path(args.boot_app0) if args.boot_app0 else Path(os.environ.get('PLATFORMIO_CORE_DIR',Path.home()/'.platformio'))/'packages-pioarduino-55.03.312-1/framework-arduinoespressif32/tools/partitions/boot_app0.bin'
        specs=[('bootloader.bin',folder/'bootloader.bin',0x1000,0x7000),('partitions.bin',folder/'partitions.bin',0x8000,0x1000),('boot_app0.bin',boot,table['otadata']['offset'],table['otadata']['size']),('firmware.bin',folder/'firmware.bin',app['offset'],app['size']),('littlefs.bin',folder/'littlefs.bin',fs['offset'],fs['size'])]
        parts=[]
        for name,source,offset,limit in specs:
            if not source.exists() or not 0<source.stat().st_size<=limit:raise ValueError('Missing or oversized '+str(source))
            shutil.copy2(source,destination/name);parts.append(dict(path=name,offset=offset,size=source.stat().st_size,sha256=sha(source)))
        check_parts(parts,table)
        manifest=dict(name='LDG · '+profile['title'],version=meta['version'],new_install_prompt_erase=True,new_install_improv_wait_time=0,builds=[dict(chipFamily=meta['chipFamily'],parts=[dict(path=p['path'],offset=p['offset']) for p in parts])])
        (destination/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n')
        (destination/'build-metadata.json').write_text(json.dumps(dict(meta,parts=parts,partitions=table),ensure_ascii=False,indent=2)+'\n')
        (destination/'SHA256SUMS').write_text(''.join(p['sha256']+'  '+p['path']+'\n' for p in parts))
        shutil.copy2(folder/'firmware.elf',destination/'firmware.elf')
        with zipfile.ZipFile(destination/'usb-firmware.zip','w',zipfile.ZIP_DEFLATED) as archive:
            for name in [p['path'] for p in parts]+['manifest.json','SHA256SUMS','build-metadata.json']:archive.write(destination/name,name)
        item['build']=dict(meta,static_ram_bytes=elf_memory(folder/'firmware.elf'),app_bytes=(folder/'firmware.bin').stat().st_size,app_limit_bytes=app['size'],parts=parts)
        catalog['profiles'].append(item)
    # The source package contains exactly the fingerprinted firmware inputs and
    # their build manifests; private local notes and credentials are excluded.
    inputs=source_inputs()+[ROOT/'LICENSE',ROOT/'package.json']
    for directory in ('scripts','web-flasher','certs/mqtt'):
        inputs.extend(p for p in (ROOT/directory).rglob('*') if p.is_file() and '__pycache__' not in p.parts)
    with zipfile.ZipFile(output/'sources.zip','w',zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(set(inputs)):archive.write(path,path.relative_to(ROOT))
        archive.writestr('SOURCE_IDENTITY.json',json.dumps(dict(commit=head,source_digest=digest,version=version(),preview=dirty),indent=2))
    (output/'catalog.json').write_text(json.dumps(catalog,ensure_ascii=False,indent=2)+'\n')
    (output/'website-provenance.json').write_text(json.dumps(dict(commit=head,source_digest=digest),indent=2)+'\n')
    check_catalog(output)
    print('Catalog:',len(catalog['profiles']),'profiles;',sum(bool(p['build']) for p in catalog['profiles']),'built')

def check_catalog(root):
    root=Path(root);catalog=json.loads((root/'catalog.json').read_text())
    if catalog['schema']!=1 or not catalog['profiles']:raise ValueError('Invalid catalog')
    for p in catalog['profiles']:
        if not p['build']:continue
        folder=root/'firmware'/p['id'];meta=json.loads((folder/'build-metadata.json').read_text())
        if meta['source_digest']!=catalog['firmware_source_digest'] or meta['commit']!=catalog['firmware_commit']:raise ValueError('Catalog provenance mismatch')
        manifest=json.loads((folder/'manifest.json').read_text());listed=manifest['builds'][0]['parts']
        if manifest.get('new_install_prompt_erase') is not True or manifest.get('new_install_improv_wait_time')!=0:raise ValueError('Unsafe erase/USB setup policy')
        if len(listed)!=5 or listed!=[dict(path=x['path'],offset=x['offset']) for x in meta['parts']]:raise ValueError('Incorrect USB manifest')
        for x in meta['parts']:
            path=folder/x['path']
            if path.parent!=folder or sha(path)!=x['sha256'] or path.stat().st_size!=x['size']:raise ValueError('Firmware checksum mismatch')
        check_parts(meta['parts'],meta['partitions'])
    return catalog

def main():
    parser=argparse.ArgumentParser();parser.add_argument('command',choices=['assemble','check','release']);parser.add_argument('--output',default='dist/web-flasher');parser.add_argument('--build-root',default='.pio/build');parser.add_argument('--boot-app0');parser.add_argument('--allow-missing',action='store_true');parser.add_argument('--allow-dirty',action='store_true');args=parser.parse_args()
    if args.command=='assemble':assemble(args)
    elif args.command=='check':check_catalog(args.output);print('Catalog checksums and NVS ranges: PASS')
    else:
        catalog=check_catalog(args.output)
        if catalog.get('preview') or any(not p['build'] for p in catalog['profiles']):raise ValueError('A complete committed catalog is required for release')
        target=Path(args.output).parent/'release';target.mkdir(parents=True,exist_ok=True)
        with zipfile.ZipFile(target/'web-flasher-site.zip','w',zipfile.ZIP_DEFLATED) as archive:
            for p in sorted(Path(args.output).rglob('*')):
                if p.is_file():archive.write(p,p.relative_to(args.output))
        shutil.copy2(Path(args.output)/'sources.zip',target/'sources.zip')
        (target/'SHA256SUMS').write_text(''.join(sha(p)+'  '+p.name+'\n' for p in sorted(target.iterdir()) if p.is_file() and p.name!='SHA256SUMS'))
if __name__=='__main__':main()
