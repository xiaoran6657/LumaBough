"""将真实双后端 reflection 映射为公共契约 fixture；失败不发布可消费的新头。"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path

def canonical(value):
    return json.dumps(value,sort_keys=True,separators=(',',':'),ensure_ascii=True)

def fnv(data):
    value=14695981039346656037
    for byte in data:value=((value^byte)*1099511628211)&((1<<64)-1)
    return value

def build(manifest_path:Path,contract_path:Path):
    raw=json.loads(manifest_path.read_text(encoding='utf-8-sig'))
    contract=json.loads(contract_path.read_text(encoding='utf-8-sig'))
    if raw.get('formatVersion')!=1 or raw.get('status')!='ready':raise ValueError('dual compiler package is not ready')
    mappings={}
    slots=set()
    for entry in contract['bindings']:
        slot=(entry['set'],entry['binding'])
        if slot in slots:raise ValueError('duplicate canonical logical binding')
        slots.add(slot)
        # aliases 只引用同一 canonical entry，不能覆盖 register/type/dimension/comparison 等属性。
        for name in [entry['name'],*entry.get('aliases',[])]:
            if not isinstance(name,str) or not name or name in mappings:raise ValueError('duplicate or invalid native binding name/alias')
            mappings[name]=entry
    packages=[]
    seen=set()
    for asset in raw['assets']:
        identity=(asset['assetId'],asset['stage'])
        if not asset['assetId'] or asset['stage'] not in ('vs','ps') or not asset['entry'] or identity in seen:raise ValueError('invalid or duplicate shader asset stage')
        seen.add(identity)
        if set(asset['variants'])!={'d3d11','d3d12'}:raise ValueError('both backend variants required')
        variants=[]
        for backend,v in sorted(asset['variants'].items()):
            logical=[]
            for binding in v['reflection']['bindings']:
                mapping=mappings.get(binding['name'])
                if mapping is None:raise ValueError('unmapped reflected binding '+binding['name'])
                for key in ('kind','register','space','count','uniformBytes','dimension','comparisonSampler'):
                    if binding[key]!=mapping[key]:raise ValueError('native binding contract drift: '+binding['name']+' '+key)
                dimension=binding['dimension']
                if binding['kind']=='texture' and dimension not in ('texture2d','texturecube'):raise ValueError('unsupported texture dimension '+dimension)
                logical.append({'set':mapping['set'],'binding':mapping['binding'],'type':{'cb':'UniformBuffer','texture':'SampledTexture','sampler':'Sampler'}[binding['kind']],
                                'count':binding['count'],'uniformBytes':binding['uniformBytes'],'dimension':'TextureCube' if dimension=='texturecube' else 'Texture2D',
                                'comparisonSampler':binding['comparisonSampler'],'dynamicOffset':mapping['dynamicOffset']})
            unique={}
            for binding in logical:
                slot=(binding['set'],binding['binding'])
                if slot in unique and unique[slot]!=binding:raise ValueError('incompatible reflected aliases')
                unique[slot]=binding
            logical=sorted(unique.values(),key=lambda x:(x['set'],x['binding']))
            inputs=[]
            for i in v['reflection']['vertexInputs']:
                semantic={'POSITION':'Position','NORMAL':'Normal','TANGENT':'Tangent','TEXCOORD':'TexCoord','COLOR':'Color'}.get(i['semantic'].upper())
                if semantic is None or i['components'] not in (2,3,4):raise ValueError('unsupported vertex input '+str(i))
                inputs.append({'semantic':semantic,'index':i['index'],'format':'Float'+str(i['components'])})
            inputs.sort(key=lambda x:(x['semantic'],x['index']))
            interface={'bindings':logical,'vertexInputs':inputs,'colorOutputMask':v['reflection']['colorOutputMask'],'writesDepth':v['reflection']['writesDepth']}
            code=(manifest_path.parent/v['bytecode']['path']).resolve()
            if not code.is_relative_to(manifest_path.parent.resolve()):raise ValueError('bytecode outside package output')
            data=code.read_bytes()
            if len(data)!=v['bytecode']['bytes'] or hashlib.sha256(data).hexdigest()!=v['bytecode']['sha256']:raise ValueError('bytecode SHA mismatch')
            semantic_hash=hashlib.sha256((asset['semanticHash']+'\n'+canonical(interface)).encode()).hexdigest()
            variants.append({'backend':backend,'sourceHash':v['sourceHash'],'semanticHash':semantic_hash,'bytecodePath':str(code).replace('\\','/'),
                             'bytecodeSha256':v['bytecode']['sha256'],'bytecodeHash':fnv(data),'interface':interface})
        if variants[0]['semanticHash']!=variants[1]['semanticHash']:raise ValueError('logical package parity blocked')
        packages.append({'assetId':asset['assetId'],'sourceStem':asset['sourceStem'],'entry':asset['entry'],'stage':asset['stage'],
                         'compilerSemanticHash':asset['semanticHash'],'variants':variants})
    return {'schemaVersion':1,'status':'ready','compilerRevision':raw['revision'],'bindingContractSha256':hashlib.sha256(contract_path.read_bytes()).hexdigest(),'packages':packages}

def header(package):
    lines=['#pragma once','#include <MiniEngine/Rhi/RhiShaderPackage.h>','#include <filesystem>','#include <fstream>','#include <stdexcept>',
           'namespace MiniEngine::Rhi::M604 {',
           'inline std::vector<std::byte> ReadPackageBytes(const std::filesystem::path& path) { std::ifstream f(path,std::ios::binary|std::ios::ate); if(!f)throw std::runtime_error("shader artifact missing"); auto size=f.tellg(); if(size<=0)throw std::runtime_error("empty shader artifact"); std::vector<std::byte> bytes(static_cast<std::size_t>(size)); f.seekg(0); if(!f.read(reinterpret_cast<char*>(bytes.data()),size))throw std::runtime_error("shader artifact truncated"); return bytes; }',
           'inline std::vector<ShaderPackage> LoadM604Packages() { std::vector<ShaderPackage> packages;']
    q=lambda x:json.dumps(x,ensure_ascii=True)
    for p in package['packages']:
        lines+=['{ ShaderPackage package; package.assetId='+q(p['assetId'])+';']
        for i,v in enumerate(p['variants']):
            lines+=['{ auto& v=package.variants['+str(i)+'];',
                    'v.backend=RhiBackend::'+('D3D11' if v['backend']=='d3d11' else 'D3D12')+';',
                    'v.stage=ShaderStage::'+('Vertex' if p['stage']=='vs' else 'Pixel')+';',
                    'v.bytecode=ReadPackageBytes('+q(v['bytecodePath'])+');',
                    'v.bytecodeHash='+str(v['bytecodeHash'])+'ULL;',
                    'v.sourceHash='+q(v['sourceHash'])+'; v.semanticHash='+q(v['semanticHash'])+'; v.entryPoint='+q(p['entry'])+';']
            for b in v['interface']['bindings']:
                lines+=['v.manifest.bindings.push_back({'+f"{b['set']},{b['binding']},BindingType::{b['type']},{b['count']},{b['uniformBytes']},TextureDimension::{b['dimension']},{str(b['comparisonSampler']).lower()}"+'});']
            for x in v['interface']['vertexInputs']:lines+=['v.manifest.vertexInputs.push_back({'+f"VertexSemantic::{x['semantic']},VertexFormat::{x['format']},{x['index']}"+'});']
            lines+=['v.manifest.colorOutputMask='+str(v['interface']['colorOutputMask'])+'; v.manifest.writesDepth='+str(v['interface']['writesDepth']).lower()+'; }']
        lines+=['ValidateShaderPackage(package); packages.push_back(std::move(package)); }']
    lines+=['return packages; }','} // namespace MiniEngine::Rhi::M604']
    return '\n'.join(lines)+'\n'

def publish(path,text):
    path.parent.mkdir(parents=True,exist_ok=True)
    temporary=path.with_suffix(path.suffix+'.tmp')
    temporary.write_text(text,encoding='utf-8');temporary.replace(path)

def main():
    p=argparse.ArgumentParser();p.add_argument('--manifest',type=Path,required=True);p.add_argument('--bindings',type=Path,required=True)
    p.add_argument('--header',type=Path,required=True);p.add_argument('--summary',type=Path,required=True);args=p.parse_args()
    package=build(args.manifest,args.bindings);text=header(package)
    publish(args.summary,json.dumps(package,ensure_ascii=False,indent=2)+'\n');publish(args.header,text)
    print(f"M6-04 logical packages: {len(package['packages'])}; both variants ready")
if __name__=='__main__':main()
