"""由 diagrams.json 生成可编辑 SVG 和同布局 PNG；运行需 requirements-dev.txt。"""
from pathlib import Path
import hashlib
import html
import os
import json
from PIL import Image, ImageDraw, ImageFont
ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / "docs/architecture"
def main():
    source = BASE / "diagrams.json"
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    exports = []
    for d in json.loads(source.read_text(encoding="utf-8"))["diagrams"]:
        im = Image.new("RGB", (1200, 560), "#f5f7fa")
        draw = ImageDraw.Draw(im)
        fonts = {}
        for size in (15, 17, 21, 32):
            fonts[size] = ImageFont.truetype(str(Path(os.environ["WINDIR"]) / "Fonts/segoeui.ttf"), size)
        svg = ['<svg xmlns="http://www.w3.org/2000/svg" width="1200" height="560" viewBox="0 0 1200 560">',f'<metadata>source-sha256:{digest}</metadata>','<rect width="1200" height="560" fill="#f5f7fa"/>']
        def text(x,y,value,size=17,color="#1d2939"):
            draw.text((x,y),value,font=fonts[size],fill=color)
            svg.append(f'<text x="{x}" y="{y+size}" font-family="Segoe UI, sans-serif" font-size="{size}" fill="{color}">{html.escape(value)}</text>')
        text(40,26,d["title"],32);text(40,76,d["subtitle"],17,"#475467")
        for i,(a,b,kind) in enumerate(d["nodes"]):
            x=40+(i%4)*290;y=155+(i//4)*200
            fill={"owned":"#dceaff","external":"#e9edf1","state":"#d9f2e8"}[kind]
            draw.rounded_rectangle((x,y,x+250,y+108),12,fill=fill,outline="#667085",width=2)
            svg.append(f'<rect x="{x}" y="{y}" width="250" height="108" rx="12" fill="{fill}" stroke="#667085" stroke-width="2"/>')
            text(x+15,y+21,a,21);text(x+15,y+60,b,15)
        for a,b in d["edges"]:
            ax=40+a%4*290;ay=155+a//4*200;bx=40+b%4*290;by=155+b//4*200
            if ay==by:
                if bx>ax:p=(ax+252,ay+54,bx-7,by+54);tri=[(bx-3,by+54),(bx-12,by+49),(bx-12,by+59)]
                else:p=(ax-2,ay+54,bx+257,by+54);tri=[(bx+253,by+54),(bx+262,by+49),(bx+262,by+59)]
            else:p=(ax+125,ay+110,bx+125,by-8);tri=[(bx+125,by-3),(bx+120,by-12),(bx+130,by-12)]
            draw.line(p,fill="#475467",width=2);draw.polygon(tri,fill="#475467")
            svg.append(f'<line x1="{p[0]}" y1="{p[1]}" x2="{p[2]}" y2="{p[3]}" stroke="#475467" stroke-width="2"/>')
            svg.append('<polygon points="'+" ".join(f"{x},{y}" for x,y in tri)+'" fill="#475467"/>')
        text(40,510,d["foot"],15,"#475467");svg.append("</svg>")
        for ext in ("svg","png"):
            path=BASE/(d["name"]+"."+ext)
            if ext=="svg":path.write_text("\n".join(svg)+"\n",encoding="utf-8")
            else:im.save(path)
            exports.append({"path":path.relative_to(ROOT).as_posix(),"sha256":hashlib.sha256(path.read_bytes()).hexdigest(),"sourceSha256":digest})
    (BASE/"exports.json").write_text(json.dumps(exports,indent=2)+"\n",encoding="utf-8")
if __name__=="__main__":main()

