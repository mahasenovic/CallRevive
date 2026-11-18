#!/usr/bin/env python3
"""Create a cosmetic-only 1.0 package around the verified client2 core."""
import base64,hashlib,json,pathlib,plistlib,shutil,stat,subprocess,sys
P=pathlib.Path
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
base=P('vendor/client2.deb')
base.write_bytes(base64.b64decode(P('vendor/client2.deb.base64').read_text(),validate=False))
assert sha(base)=='cc6fb9d5987efd7581823ff8938eaa50c289a37f31df1cd0b36fe5f1ed16c27d'
icons=json.loads(P('icons.json').read_text())
for dirname in ['preferences/Resources','layout/Library/PreferenceLoader/Preferences']:
 for name,data in icons.items():P(dirname,name).write_bytes(base64.b64decode(data))
if len(sys.argv)==1:
 print('Verified base and restored icons. Build preferences next.');sys.exit(0)
out=P('dist');out.mkdir(exist_ok=True)
root=out/'root';subprocess.run(['dpkg-deb','-R',str(base),str(root)],check=True)
def snapshot():
 return {str(p.relative_to(root)):(sha(p),stat.S_IMODE(p.stat().st_mode)) for p in root.rglob('*') if p.is_file()}
old=snapshot()
core='var/jb/Library/MobileSubstrate/DynamicLibraries/MohsenTouchGuard.dylib'
prefs='var/jb/Library/PreferenceBundles/TouchGuardPrefs.bundle'
shutil.copyfile(sys.argv[1],root/prefs/'TouchGuardPrefs')
(root/prefs/'TouchGuardPrefs').chmod(0o755)
for name in ['Root.plist','Info.plist',*icons]:shutil.copyfile(P('preferences/Resources',name),root/prefs/name)
entry='var/jb/Library/PreferenceLoader/Preferences/com.mohsen.touchguard.plist'
shutil.copyfile(P('layout/Library/PreferenceLoader/Preferences/com.mohsen.touchguard.plist'),root/entry)
for name in icons:shutil.copyfile(P('layout/Library/PreferenceLoader/Preferences',name),root/P(entry).parent/name)
control=root/'DEBIAN/control'
text=control.read_text().splitlines();metadata=json.loads(P('package-metadata.json').read_text())
for key,value in metadata.items():
 text=[(key+': '+value) if s.startswith(key+': ') else s for s in text]
control.write_text('\n'.join(text)+'\n')
new=snapshot()
assert new[core]==old[core], 'Runtime core changed'
allowed={'DEBIAN/control',entry,*[prefs+'/'+n for n in ['TouchGuardPrefs','Root.plist','Info.plist',*icons]],*[str(P(entry).parent/n) for n in icons]}
changed={k for k in set(old)|set(new) if old.get(k)!=new.get(k)}
assert changed <= allowed,(changed-allowed)
assert {'DEBIAN/control',entry,prefs+'/TouchGuardPrefs',prefs+'/Root.plist'}<=changed
# All original functional switch dictionaries remain byte-for-value identical.
baseline=P('dist/base');subprocess.run(['dpkg-deb','-R',str(base),str(baseline)],check=True)
old_plist=plistlib.loads((baseline/prefs/'Root.plist').read_bytes())
new_plist=plistlib.loads((root/prefs/'Root.plist').read_bytes())
switches=lambda p:[x for x in p['items'] if x.get('cell')=='PSSwitchCell']
assert len(switches(new_plist))==9 and switches(new_plist)==switches(old_plist)
package=out/'com.mohsen.touchguard_1.0_iphoneos-arm64.deb'
subprocess.run(['dpkg-deb','--root-owner-group','-Zgzip','--build',str(root),str(package)],check=True)
audit={'version':'1.0','base_package_sha256':sha(base),'package_sha256':sha(package),'preserved_runtime_core_sha256':new[core][0],'changed_files':sorted(changed),'all_9_switches_unchanged':True,'device_ui_verified':False}
(out/'AUDIT.json').write_text(json.dumps(audit,indent=2)+'\n')
print(json.dumps(audit,indent=2))
