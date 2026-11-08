#!/usr/bin/env python3
"""Update the UI around a verified, byte-preserved stable firmware release."""
import argparse,hashlib,json,re,shutil,stat,zipfile
from pathlib import Path
from firmware_identity import ROOT,source_digest,git
from web_flasher import check_catalog,sha

def update(archive,checksums,tag,output):
    if not re.fullmatch(r'v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)',tag):raise ValueError('Stable release tag required')
    expected={name.strip():digest for digest,name in (line.split(maxsplit=1) for line in Path(checksums).read_text().splitlines() if line.strip())}
    if expected.get(Path(archive).name)!=sha(archive):raise ValueError('Release archive SHA-256 mismatch')
    output=Path(output);output.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(archive) as bundle:
        if sum(i.file_size for i in bundle.infolist())>256*1024*1024:raise ValueError('Release archive too large')
        for info in bundle.infolist():
            path=output/info.filename
            if not path.resolve().is_relative_to(output.resolve()) or stat.S_ISLNK(info.external_attr>>16):raise ValueError('Unsafe archive path')
        bundle.extractall(output)
    catalog=check_catalog(output)
    if catalog.get('preview') or any(not p['build'] for p in catalog['profiles']):raise ValueError('Incomplete firmware release')
    if any(p['build']['version']!=tag[1:] for p in catalog['profiles']):raise ValueError('Firmware release tag/version mismatch')
    if catalog['firmware_source_digest']!=source_digest():raise ValueError('Firmware source changed; publish a new firmware release before updating the installer')
    protected=[p for p in output.rglob('*') if p.is_file() and (p.is_relative_to(output/'firmware') or p.name in ('catalog.json','sources.zip'))]
    before={p.relative_to(output).as_posix():sha(p) for p in protected}
    for name in ('index.html','app.js','provisioning.js','serial-api.js','style.css','photos.json'):shutil.copy2(ROOT/'web-flasher'/name,output/name)
    shutil.copytree(ROOT/'web-flasher/images',output/'images',dirs_exist_ok=True)
    # The released self-hosted vendor distribution and all firmware stay intact.
    (output/'website-provenance.json').write_text(json.dumps(dict(commit=git('rev-parse','HEAD'),firmware_release=tag,firmware_commit=catalog['firmware_commit'],firmware_source_digest=catalog['firmware_source_digest']),indent=2)+'\n')
    assert before=={name:sha(output/name) for name in before}
    check_catalog(output)
    return before
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--archive',required=True);p.add_argument('--checksums',required=True);p.add_argument('--tag',required=True);p.add_argument('--output',default='dist/web-flasher');a=p.parse_args();update(a.archive,a.checksums,a.tag,a.output);print('Updated website; released firmware preserved byte-for-byte')
