#pragma once
namespace edvr {
constexpr char kEnginePrimaryCopyScatterCsHlsl[] = R"HLSL(
struct Patch { uint slot; uint marker; uint native[84]; uint previous[5]; };
struct Record { uint4 data[21]; };
StructuredBuffer<Patch> P : register(t0);
RWStructuredBuffer<Record> Pool : register(u0);
cbuffer Count : register(b0) { uint rows; uint slots; uint2 unused; };
[numthreads(64,1,1)] void main(uint3 id:SV_DispatchThreadID) {
 if(id.x>=rows)return;
 Patch p=P[id.x];if(p.slot>=slots)return;
 Record r=Pool[p.slot];
 [loop] for(uint i=0;i<84;++i)if(r.data[i/4][i%4]!=p.native[i])return;
 r.data[18]=uint4(p.marker,p.previous[0],p.previous[1],p.previous[2]);
 r.data[19].zw=uint2(p.previous[3],p.previous[4]);
 Pool[p.slot]=r;
}
)HLSL";
}
