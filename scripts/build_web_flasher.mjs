// Self-host the pinned ESP Web Tools distribution and preserve its licenses.
import {cp,readFile,writeFile,mkdir} from 'node:fs/promises';
import {spawnSync} from 'node:child_process';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {patchVendor} from './patch_web_flasher_vendor.mjs';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const args=process.argv.slice(2),at=args.indexOf('--output'),output=path.resolve(at<0?'dist/web-flasher':args[at+1]);
const result=spawnSync(process.env.PYTHON||'python3',['scripts/web_flasher.py','assemble',...args],{cwd:root,stdio:'inherit'});if(result.status!==0)process.exit(result.status||1);
await cp(path.join(root,'node_modules/esp-web-tools/dist/web'),path.join(output,'vendor'),{recursive:true});
await patchVendor(output);
const licenses=['ESP Web Tools 10.4.0: local cancellable ldg-flash-state callback and released-port handoff, adapted from G5500-ESP32S3-Controller (GPL-3.0).'];
for(const name of ['esp-web-tools','esptool-js','lit','lit-element','lit-html','@lit/reactive-element','@material/web','tslib','improv-wifi-serial-sdk','pako','atob-lite']){const folder=path.join(root,'node_modules',name),pkg=JSON.parse(await readFile(path.join(folder,'package.json'),'utf8'));let license='';for(const name of ['LICENSE','LICENSE.txt','LICENSE.md','CopyrightNotice.txt'])try{license=await readFile(path.join(folder,name),'utf8');break}catch{}if(!license)throw Error('Missing license for '+name);licenses.push(pkg.name+' '+pkg.version+' ('+pkg.license+')\n'+license)}
await writeFile(path.join(output,'THIRD_PARTY_LICENSES.txt'),licenses.join('\n\n--------------------\n\n'));
console.log('Web flasher: '+output);
