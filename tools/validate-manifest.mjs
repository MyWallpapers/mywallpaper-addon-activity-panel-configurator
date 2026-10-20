import { readFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL, fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';
const root = fileURLToPath(new URL('..', import.meta.url));
const sdk = resolve(process.argv[2] ?? resolve(root, '../MyWallpaper/packages/sdk-public'));
const lock = JSON.parse(await readFile(resolve(root, 'native/sdk-public.lock.json'), 'utf8'));
for (const [file, digest] of Object.entries(lock.files)) {
  const bytes = await readFile(resolve(sdk, file));
  if (createHash('sha256').update(bytes).digest('hex') !== digest) throw Error(`Public SDK identity changed: ${file}. Review explicit lock update.`);
}
const api = await import(pathToFileURL(resolve(sdk, 'dist/addon-schema/index.js')));
const manifest = JSON.parse(await readFile(resolve(root, 'manifest.json'), 'utf8'));
api.parseAddonManifest(manifest);
const contractRoot=resolve(root,'contracts/audio-system/v1');
const contractLock=JSON.parse(await readFile(resolve(contractRoot,'provenance.json'),'utf8'));
for(const [file,digest] of Object.entries(contractLock.files)) {
  if(createHash('sha256').update(await readFile(resolve(contractRoot,file))).digest('hex')!==digest) throw Error(`Pinned A18 contract changed: ${file}`);
}
if(JSON.stringify(manifest.services?.requires?.audio)!==JSON.stringify({contract:'a18/audio-system',version:1,optional:true})) throw Error('Exact optional A18 requirement expected');
const settings = manifest.settings;
const values = Object.fromEntries(settings.filter(s=>s.type !== 'section').map(s=>[s.id,s.default]));
const issues = api.validateSettingValues(manifest, values, 'device');
if (issues.length) throw Error(JSON.stringify(issues));
if (settings.length>100 || settings.some(s=>s.id==='profile'||s.showIf?.setting==='profile')) throw Error('Direct settings contract');
for (const setting of settings) if (setting.showIf && !settings.some(s=>s.id===setting.showIf.setting)) throw Error('Invalid visibility reference');
for (const dependency of JSON.parse(await readFile(resolve(root, 'native/dependencies.lock.json'), 'utf8')).dependencies) {
  for (const [file,digest] of [[dependency.path,dependency.sha256],[dependency.noticePath,dependency.noticeSha256]]) {
    if(createHash('sha256').update(await readFile(resolve(root,file))).digest('hex')!==digest)throw Error(`Dependency changed: ${file}`);
  }
}
console.log(`Exact public SDK accepted ${settings.length} definitions and defaults; dependency bytes verified.`);
