#!/usr/bin/env python3
"""Import original stage geometry and event records.

Commercial game content is not included; this script downloads public
community-reconstructed level layout exports.
"""
from __future__ import annotations
import argparse
import struct
import urllib.request
from pathlib import Path
from io import BytesIO
import zipfile

HERE = Path(__file__).resolve().parent
OUT = HERE / "romfs" / "levels"
BASE = "https://raw.githubusercontent.com/MysticAx0lotl/impossible-game-levels/main/output"
REMOTE = {0:"fireaura.lvl",1:"fireauraxbox.lvl",2:"chaozfantasy.lvl",3:"heaven.lvl"}
ARCHIVE = "https://codeload.github.com/MysticAx0lotl/impossible-game-levels/zip/refs/heads/main"

def unbox(dat):
    mv=memoryview(dat)
    cur=0
    def get(fmt):
        nonlocal cur
        size=struct.calcsize(fmt)
        if cur+size>len(dat): raise ValueError(f"Truncated level data at {cur}")
        value=struct.unpack_from(fmt,mv,cur)
        cur+=size
        return value[0] if len(value)==1 else value
    def count(limit):
        n=get(">i")
        if not 0<=n<=limit: raise ValueError("Invalid event count")
        return n
    if get(">I")!=0: raise ValueError("Only version 0 supported")
    if get(">B")!=0: raise ValueError("Custom graphics flag unsupported")
    n=get(">H")
    if not 1<=n<=10000: raise ValueError("Invalid obstacle count")
    objs=[get(">Bii") for _ in range(n)]
    if any(o[0]>2 for o in objs): raise ValueError("Unknown obstacle")
    end=get(">i")
    if end<=0: raise ValueError("Invalid end position")
    background=[]
    for _ in range(count(256)):
        x=get(">i"); custom=get(">B"); color=get(">i")
        if custom or not 0<=color<=5: raise ValueError("Unsupported background event")
        background.append((x,color))
    grav=[get(">i") for _ in range(count(128))]
    rise=[get(">ii") for _ in range(count(128))]
    fall=[get(">ii") for _ in range(count(128))]
    return objs,end,sorted(background),sorted(grav),sorted(rise),sorted(fall)

def install(stage,dat):
    objs,end,bgs,grav,rise,fall=unbox(dat)
    def s(x):return round(x*4/3)
    output=bytearray(struct.pack(">H",len(objs)))
    for typ,x,y in objs:output+=struct.pack(">Bii",typ,s(x),s(y))
    output+=struct.pack(">i",s(end))+b"EVT1"+struct.pack(">H",len(bgs))
    for x,c in bgs:output+=struct.pack(">iB",s(x),c)
    output+=struct.pack(">H",len(grav))
    for x in grav:output+=struct.pack(">i",s(x))
    for events in (rise,fall):
        output+=struct.pack(">H",len(events))
        for x,y in events:output+=struct.pack(">ii",s(x),s(y))
    OUT.mkdir(parents=True,exist_ok=True)
    (OUT/f"stage{stage}.bin").write_bytes(output)
    print(f"Stage {stage}: {len(objs)} obstacles, {len(bgs)} background events")

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("game_zip",type=Path,nargs="?")
    parser.add_argument("--download",action="store_true")
    args=parser.parse_args()
    if args.game_zip:
        with zipfile.ZipFile(args.game_zip) as z:
            matches=[n for n in z.namelist() if n.endswith("/res/phazed/level.dat")]
            if len(matches)!=1:raise SystemExit("Original Phazd level not found")
            install(4,z.read(matches[0]))
    elif not (OUT/"stage4.bin").is_file():
        raise SystemExit("Missing Phazd stage; supply your original game ZIP")
    if args.download:
        missing=[]
        for i,folder in REMOTE.items():
            if (OUT/f"stage{i}.bin").exists():continue
            try:
                req=urllib.request.Request(f"{BASE}/{folder}/level.dat",headers={"User-Agent":"ImpossibleGame3DS/0.7"})
                with urllib.request.urlopen(req,timeout=25) as response:install(i,response.read(500000))
            except Exception as e:
                print("Download failed",i,e);missing.append((i,folder))
        if missing:
            try:
                req=urllib.request.Request(ARCHIVE,headers={"User-Agent":"ImpossibleGame3DS/0.7"})
                with urllib.request.urlopen(req,timeout=40) as response: data=response.read(10_000_000)
                with zipfile.ZipFile(BytesIO(data)) as z:
                    for i,folder in missing:
                        matches=[n for n in z.namelist() if n.lower().endswith("/output/"+folder+"/level.dat")]
                        if matches:install(i,z.read(matches[0]))
            except Exception as e:print("Archive fallback failed",e)
    remaining=[i for i in range(5) if not (OUT/f"stage{i}.bin").is_file()]
    if remaining:raise SystemExit(f"Missing original layouts: {remaining}")
    print("All five stage layouts installed")

if __name__=="__main__":main()
