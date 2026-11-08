"""Identity shared by compiled USB hello and the downloadable firmware catalog."""
from pathlib import Path
import hashlib,json,re,subprocess
ROOT=Path(globals().get('__file__',str(Path.cwd()/'scripts/firmware_identity.py'))).resolve().parents[1]
BASE='0.1.0'
def git(*args,root=ROOT):
    frozen=Path(root)/'SOURCE_IDENTITY.json'
    if not (Path(root)/'.git').exists() and frozen.exists():
        data=json.loads(frozen.read_text())
        if args==('rev-parse','HEAD'):return data['commit']
        if args and args[0]=='status':return ''
    return subprocess.check_output(['git',*args],cwd=root,text=True,stderr=subprocess.DEVNULL).strip()
def version(root=ROOT):
    frozen=Path(root)/'SOURCE_IDENTITY.json'
    if not (Path(root)/'.git').exists() and frozen.exists():return json.loads(frozen.read_text())['version']
    try:tag=git('describe','--tags','--match','v[0-9]*','--abbrev=0',root=root)
    except subprocess.CalledProcessError:tag=''
    if tag and not re.fullmatch(r'v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)',tag):raise ValueError('Invalid release version')
    base=tag[1:] if tag else BASE
    messages=git('log','--format=%B%x00',*( [tag+'..HEAD'] if tag else []),root=root).split('\0')
    messages=[m.strip() for m in messages if m.strip()]
    if not messages:return base
    parts=list(map(int,base.split('.')))
    level=2
    for message in messages:
        if re.match(r'\w+(\([^\n]*\))?!:',message) or re.search(r'^BREAKING CHANGE:',message,re.M):level=0;break
        if re.match(r'feat(\([^\n]*\))?:',message):level=min(level,1)
    parts[level]+=1
    for i in range(level+1,3):parts[i]=0
    return '.'.join(map(str,parts))+'-dev.'+str(len(messages))
def source_inputs():
    paths=[]
    for directory in ('src','include','patches','web','data'):
        paths.extend(p for p in (ROOT/directory).rglob('*') if p.is_file() and '__pycache__' not in p.parts)
    paths.extend(ROOT/'scripts'/p for p in ('firmware_identity.py','apply_patches.py','build_web.py','build_web.cjs','build_mqtt_ca.py','build_compact_tls.py'))
    paths.extend(ROOT/p for p in ('platformio.ini','partitions_custom.csv','requirements-platformio.txt','package-lock.json'))
    return sorted(paths)
def source_digest():
    digest=hashlib.sha256()
    for p in source_inputs():digest.update(p.relative_to(ROOT).as_posix().encode()+b'\0'+p.read_bytes()+b'\0')
    return digest.hexdigest()
def install(env):
    # SCons does not reliably track a command-line -include as a dependency
    # of managed library objects. Change the command signature with the board
    # header so a cached TFT_eSPI can never keep an older pin/font setup.
    hardware=hashlib.sha256((ROOT/'include/User_Setup.h').read_bytes()).hexdigest()[:16]
    env.Append(CPPDEFINES=[('LDG_HARDWARE_CONFIG_DIGEST','0x'+hardware)])
    profile=env['PIOENV'];display=profile!='esp32-remote'
    data=dict(schema=1,family='tuner',profile=profile,chipFamily='ESP32',role='tuner_controller',
        display='ST7796/GT911' if profile=='esp32-3248s035c' else 'BTT TFT35-SPI' if display else None,
        capabilities=dict(usb_provisioning=True,wifi=True,mqtt=True,mqtt_tls=True,home_assistant_discovery=True,tci=True,web=True,display=display),
        commit=git('rev-parse','HEAD'),version=version(),source_digest=source_digest())
    folder=Path(env.subst('$BUILD_DIR'))/'generated';folder.mkdir(parents=True,exist_ok=True)
    header='#pragma once\n#define LDG_BUILD_METADATA '+json.dumps(json.dumps(data,separators=(',',':')))+'\n'
    target=folder/'build_identity.h'
    if not target.exists() or target.read_text()!=header:target.write_text(header)
    env.Append(CPPPATH=[str(folder)])
    def save_metadata(source,target,env):
        (folder/'build-metadata.json').write_text(json.dumps(data,indent=2)+'\n')
    env.AddPostAction('$BUILD_DIR/${PROGNAME}.elf',save_metadata)
if 'Import' in globals():
    Import('env')
    install(env)
