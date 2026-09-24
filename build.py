"""Build on Windows x64: python build.py --zig C:/path/to/zig.exe
Requires Python 3.10+, Zig 0.14.1, and OBS Studio 32.2.2 (x64).
Downloads the official matching OBS headers on the first run.
"""
import argparse, concurrent.futures, json, os, pathlib, shutil, struct, subprocess, urllib.request
ROOT=pathlib.Path(__file__).resolve().parent
def exports(path):
    b=path.read_bytes()
    u16=lambda o:struct.unpack_from('<H',b,o)[0]
    u32=lambda o:struct.unpack_from('<I',b,o)[0]
    pe=u32(0x3c); optional=pe+24
    sections=optional+u16(pe+20)
    def offset(rva):
        for i in range(u16(pe+6)):
            s=sections+i*40; va=u32(s+12)
            if va<=rva<va+max(u32(s+8),u32(s+16)):return u32(s+20)+rva-va
        raise ValueError('Unknown RVA')
    directory=offset(u32(optional+(112 if u16(optional)==0x20b else 96)))
    names=offset(u32(directory+32))
    result=[]
    for i in range(u32(directory+24)):
        p=offset(u32(names+4*i));result.append(b[p:b.index(b'\0',p)].decode('ascii'))
    return result
def run(*args):subprocess.run([str(a) for a in args],check=True,cwd=ROOT)
def main():
    a=argparse.ArgumentParser();a.add_argument('--zig',required=True)
    a.add_argument('--obs',default='C:/Program Files/obs-studio');a.add_argument('--test',action='store_true')
    a.add_argument('--build-dir',default=str(ROOT/'build'))
    args=a.parse_args();zig=pathlib.Path(args.zig).resolve();obs=pathlib.Path(args.obs)
    build=pathlib.Path(args.build_dir).resolve();build.mkdir(parents=True,exist_ok=True)
    os.environ['ZIG_GLOBAL_CACHE_DIR']=str(build/'zig-cache')
    headers=build/'obs-sdk';tag='32.2.2'
    if not (headers/'ready').exists() and (ROOT/'vendor/obs-sdk/ready').exists():
        shutil.copytree(ROOT/'vendor/obs-sdk',headers,dirs_exist_ok=True)
    def get(url):return urllib.request.urlopen(url,timeout=90).read()
    if not (headers/'ready').exists():
        tree=json.loads(get(f'https://api.github.com/repos/obsproject/obs-studio/git/trees/{tag}?recursive=1'))
        paths=[x['path'] for x in tree['tree'] if x['path'].startswith('libobs/') and x['path'].endswith('.h')]
        def fetch(s):
            p=headers/s;p.parent.mkdir(parents=True,exist_ok=True)
            p.write_bytes(get(f'https://raw.githubusercontent.com/obsproject/obs-studio/{tag}/{s}'))
        with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:list(pool.map(fetch,paths))
        (headers/'libobs/obsconfig.h').write_text('#pragma once\n#define OBS_DATA_PATH "data"\n#define OBS_PLUGIN_PATH "obs-plugins"\n#define OBS_PLUGIN_DESTINATION "obs-plugins"\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n')
        (headers/'ready').write_text(tag)
    definition=build/'obs.def'
    definition.write_text('LIBRARY obs.dll\nEXPORTS\n'+'\n'.join(exports(obs/'bin/64bit/obs.dll'))+'\n')
    run(zig,'dlltool','-m','i386:x86-64','-d',definition,'-l',build/'obs.lib')
    common=['-target','x86_64-windows-gnu','-std=c++17','-O2','-D_M_X64','-I'+str(headers/'libobs')]
    dest=ROOT/'package/voice-compressor';(dest/'bin/64bit').mkdir(parents=True,exist_ok=True)
    run(zig,'c++',*common,'-shared',ROOT/'src/plugin.cpp',build/'obs.lib','-luser32','-lgdi32',
        ROOT/'src/plugin.def','-o',dest/'bin/64bit/voice-compressor.dll')
    shutil.copytree(ROOT/'data',dest/'data',dirs_exist_ok=True)
    for extra in (dest/'bin/64bit').iterdir():
        if extra.suffix in ('.pdb','.lib'):shutil.move(str(extra),str(build/extra.name))
    print('Built:',dest/'bin/64bit/voice-compressor.dll',flush=True)
    if args.test:
        run(zig,'c++',*common,ROOT/'tests/dsp_test.cpp','-o',build/'dsp_test.exe')
        run(build/'dsp_test.exe')
        run(zig,'c++',*common,ROOT/'tests/obs_smoke.cpp',build/'obs.lib','-luser32','-lgdi32','-o',build/'obs_smoke.exe')
        os.environ['PATH']=str(obs/'bin/64bit')+os.pathsep+os.environ.get('PATH','')
        for locale in ['en-US','ja-JP','zh-CN','zh-TW']:
            os.environ['VOICE_TEST_LOCALE']=locale
            subprocess.run([str(build/'obs_smoke.exe'),str(dest/'bin/64bit/voice-compressor.dll'),str(dest/'data')],check=True,cwd=build)
            shutil.copyfile(build/'meter-test.bmp',build/f'meter-{locale}.bmp')
            shutil.copyfile(build/'help-test.bmp',build/f'help-{locale}.bmp')
            print('PASS: locale',locale,flush=True)
if __name__=='__main__':main()
