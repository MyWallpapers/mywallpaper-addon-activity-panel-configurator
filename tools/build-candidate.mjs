import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { readFileSync, writeFileSync, mkdirSync, copyFileSync, realpathSync, existsSync, chmodSync } from 'node:fs';
import { resolve, join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
const root=realpathSync(fileURLToPath(new URL('..',import.meta.url)));
const shared=resolve(root,'../MyWallpaper');
const task=join(shared,'tools/dev/task.mjs');
const {collectCheckoutSourceIdentity}=await import(pathToFileURL(join(shared,'tools/dev/source-identity.mjs')));
const hash=bytes=>createHash('sha256').update(bytes).digest('hex');
const identity=()=>collectCheckoutSourceIdentity(root);
const same=(a,b)=>a?.sourceComplete&&b?.sourceComplete&&a.head===b.head&&a.sourceFingerprint===b.sourceFingerprint&&a.checkoutFingerprint===b.checkoutFingerprint;
const sharedFiles=['tools/dev/task.mjs','tools/dev/native-addon.mjs','tools/dev/source-identity.mjs','tools/dev/native/NativeDevelopment.cmake','tools/dev/native/WindowsCompilerInputs.cmake','docs/decisions/decision-ledger.json'];
const sharedIdentity=()=>Object.fromEntries(sharedFiles.map(file=>[file,hash(readFileSync(join(shared,file)))]));
const privateRoot=join(root,'.mywallpaper');mkdirSync(privateRoot,{recursive:true,mode:0o700});
const marker=join(privateRoot,'candidate.json');
function run(command,args){const result=spawnSync(command,args,{cwd:root,encoding:'utf8',windowsHide:true,maxBuffer:8*1024*1024});if(result.status!==0)throw Error(`${command} failed: ${result.error?.message??result.stderr??result.status}`);return result;}
run(process.execPath,['tools/validate-manifest.mjs']);
const before=identity(),tooling=sharedIdentity();
if(existsSync(marker)){
  const previous=JSON.parse(readFileSync(marker,'utf8'));
  if(same(previous.source,before)&&JSON.stringify(previous.tooling)===JSON.stringify(tooling)&&Object.entries(previous.outputs).every(([file,digest])=>existsSync(join(root,file))&&hash(readFileSync(join(root,file)))===digest)){
    console.log('A24 DEV candidate reused: exact source, tooling and output hashes unchanged.');process.exit(0);
  }
}
const receiptsRoot=join(root,'docs/receipts');mkdirSync(receiptsRoot,{recursive:true});
const receipt=join(receiptsRoot,`components-${Date.now()}.json`),log=join(privateRoot,`components-${Date.now()}.log`);
const result=spawnSync(process.execPath,[task,'verify','native','--project',root,'--owner','A24','--intent','Build owned A24 DEV candidate','--proof','component','--output',receipt],{cwd:root,encoding:'utf8',windowsHide:true,maxBuffer:16*1024*1024});
writeFileSync(log,(result.stdout??'')+(result.stderr??''),{mode:0o600});
if(result.status!==0)throw Error(`Component verification failed (${result.status}); private log: ${log}`);
const proof=JSON.parse(readFileSync(receipt,'utf8'));
if(proof.status!=='complete'||proof.scope?.kind!=='native-component-tests'||!same(proof.source.before,proof.source.after)||!same(before,identity()))throw Error('Incomplete or changed component source proof');
const buildLines=(result.stderr??'').split(/\r?\n/).filter(line=>line.startsWith('{"nativeLane"'));
const build=JSON.parse(buildLines.at(-1)).build;
const local=process.platform==='win32'?build:run('wslpath',['-u',build]).stdout.trim();
const binary=join(local,'Debug/companion.exe');
const outputs={};
for(const [from,to] of [[binary,'native/out/windows-x86_64/companion.exe'],[join(root,'LICENSE'),'native/out/windows-x86_64/LICENSE'],[join(root,'THIRD_PARTY_NOTICES.md'),'native/out/windows-x86_64/THIRD_PARTY_NOTICES.md'],[join(root,'third_party/nlohmann/LICENSE.MIT'),'native/out/windows-x86_64/notices/nlohmann-json-LICENSE.MIT'],[join(root,'contracts/audio-system/v1/LICENSE.MIT'),'native/out/windows-x86_64/notices/a18-audio-system-LICENSE.MIT']]){
  const dest=join(root,to);mkdirSync(resolve(dest,'..'),{recursive:true});copyFileSync(from,dest);outputs[to]=hash(readFileSync(dest));
}
if(!same(before,identity())||JSON.stringify(tooling)!==JSON.stringify(sharedIdentity()))throw Error('Source/tooling changed while staging candidate');
writeFileSync(marker,JSON.stringify({schemaVersion:1,kind:'DEV-debug-non-distributable',source:before,tooling,outputs,receipt,log,build,acceptanceComplete:false},null,2)+'\n',{mode:0o600});chmodSync(marker,0o600);
console.log('A24 DEV candidate staged with exact component receipt; application acceptance remains open.');
