// Keep the pinned ESP Web Tools callback observable and allow USB handoff.
import {readdir, readFile, writeFile} from 'node:fs/promises';
import path from 'node:path';

export async function patchVendor(output) {
  const directory = path.join(output, 'vendor');
  const files = (await readdir(directory)).filter((name) => /^install-dialog-.*\.js$/.test(name));
  if (files.length !== 1) throw Error('Unexpected ESP Web Tools dialog distribution');
  const file = path.join(directory, files[0]);
  let script = await readFile(file, 'utf8');
  const original = 'this._installState=e,"finished"===e.state?';
  const previous = 'this._installState=e,this.dispatchEvent(new CustomEvent("ldg-flash-state",{detail:e,bubbles:true,composed:true})),"finished"===e.state?';
  const patched = 'this._installState=e;if(!this.dispatchEvent(new CustomEvent("ldg-flash-state",{detail:e,bubbles:true,composed:true,cancelable:true}))){this._handleClose();return}"finished"===e.state?';
  if (script.includes(patched)) return 'vendor/' + files[0];
  const input = script.includes(previous) ? previous : original;
  if (script.split(input).length !== 2) throw Error('ESP Web Tools flash callback changed');
  script = script.replace(input, patched);
  await writeFile(file, script);
  return 'vendor/' + files[0];
}

if (process.argv[1]?.endsWith('patch_web_flasher_vendor.mjs')) console.log(await patchVendor(process.argv[2]));
