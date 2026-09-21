"""逐级比较 public adapter 与保留的 concrete 固定输入路径。"""
from __future__ import annotations
import argparse, hashlib, json
from pathlib import Path
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--smoke-root",required=True,type=Path)
    p.add_argument("--legacy-root",required=True,type=Path)
    p.add_argument("--mode",choices=["hardware","warp"],default="hardware")
    p.add_argument("--output",required=True,type=Path)
    args=p.parse_args()
    report={"schema":"miniengine.m6-06.legacy-ab.v1","status":"FAIL","mode":args.mode,"cases":[]}
    suffix={1:"level1-clear-96x64",2:"level2-tone-4x4",3:"level3-depth-ev0-96x64",
            4:"level4-resize-ev0-113x75",5:"level4-resize-ev0-113x75",6:"level6-revision-ev1-113x75"}
    try:
        smoke=json.loads((args.smoke_root/"summary.json").read_text())
        assert smoke["status"]=="PASS"
        for level in range(1,7):
            for backend in ("d3d11","d3d12"):
                new_path=args.smoke_root/f"{backend}-{args.mode}-level{level}"/"screenshot.rgba"
                old_path=args.legacy_root/args.mode/f"{backend}-{suffix[level]}.rgba"
                new,old=new_path.read_bytes(),old_path.read_bytes()
                assert new and old
                if level==2:
                    # 旧 probe 的恒定 4x4 HDR 输入与新全屏使用同一值；先验证旧结果确实恒定。
                    assert all(old[i:i+4]==old[:4] for i in range(0,len(old),4))
                    reference=old[:4]*(len(new)//4)
                    comparison="constant HDR input; 4x4 reference expanded by identical pixel value"
                else:reference=old;comparison="same extent and fixed indexed mesh/clear input"
                assert len(new)==len(reference),(backend,level,len(new),len(reference))
                maximum=max(abs(a-b) for a,b in zip(new,reference))
                different=sum(a!=b for a,b in zip(new,reference))
                assert maximum<=1,(backend,level,maximum,different)
                report["cases"].append({"backend":backend,"level":level,"status":"PASS",
                    "maxChannelDifference":maximum,"differentChannels":different,
                    "comparison":comparison,"newPath":new_path.as_posix(),"legacyPath":old_path.as_posix(),
                    "newSha256":hashlib.sha256(new).hexdigest(),"legacySha256":hashlib.sha256(old).hexdigest()})
                print("PASS",backend,"level",level,"maxDiff",maximum,flush=True)
        report["status"]="PASS"
    except Exception as error:report["failure"]=str(error);print("FAIL",error,flush=True)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8")
    return 0 if report["status"]=="PASS" else 1
if __name__=="__main__":raise SystemExit(main())
