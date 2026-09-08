#pragma once
// Shared by the player and the GPU pixel-output regression tests.
inline constexpr char MotionResolveHlsl[]=R"(
Texture2D<int2> F:register(t0);
Texture2D<uint> C:register(t1);
Texture2D<float4> Now:register(t2);
Texture2D<float4> Prev:register(t3);
StructuredBuffer<float4> Camera:register(t4);
SamplerState S:register(s0);
cbuffer Params:register(b0){float4 P;};
struct V{float4 pos:SV_Position;float2 uv:TEXCOORD0;};
struct O{float2 mv:SV_Target0;float bias:SV_Target1;};
float Luma(float3 c){return dot(c,float3(0.2126,0.7152,0.0722));}
O Raw(V i){O o;o.mv=0;o.bias=0;if(P.z<0.5)return o;
uint w,h;F.GetDimensions(w,h);float2 q=clamp(i.uv*float2(w,h)-0.5,0,float2(w-1,h-1));
int2 a=int2(floor(q));int2 b=min(a+1,int2(w-1,h-1));float2 t=frac(q);
float2 m=lerp(lerp(float2(F.Load(int3(a,0))),float2(F.Load(int3(b.x,a.y,0))),t.x),lerp(float2(F.Load(int3(a.x,b.y,0))),float2(F.Load(int3(b,0))),t.x),t.y);
float2 mv=m/32.0;uint sw,sh;Now.GetDimensions(sw,sh);float2 texel=1.0/float2(sw,sh);
float now=Luma(Now.SampleLevel(S,i.uv,0).rgb);
float same=Luma(Prev.SampleLevel(S,i.uv,0).rgb);
float warped=Luma(Prev.SampleLevel(S,i.uv+mv*texel,0).rgb);
float errZero=abs(now-same),errFlow=abs(now-warped);
float local=max(max(abs(now-Luma(Now.SampleLevel(S,i.uv+float2(texel.x,0),0).rgb)),abs(now-Luma(Now.SampleLevel(S,i.uv-float2(texel.x,0),0).rgb))),max(abs(now-Luma(Now.SampleLevel(S,i.uv+float2(0,texel.y),0).rgb)),abs(now-Luma(Now.SampleLevel(S,i.uv-float2(0,texel.y),0).rgb))));
float cost=lerp(lerp(C.Load(int3(a,0)),C.Load(int3(b.x,a.y,0)),t.x),lerp(C.Load(int3(a.x,b.y,0)),C.Load(int3(b,0)),t.x),t.y)/255.0;
float mag=length(mv);float evidence=errZero-errFlow;
int2 al=max(a-int2(1,0),0),ar=min(a+int2(1,0),int2(w-1,h-1)),au=max(a-int2(0,1),0),ad=min(a+int2(0,1),int2(w-1,h-1));
float coherent=max(max(length(float2(F.Load(int3(al,0)))/32.0-mv),length(float2(F.Load(int3(ar,0)))/32.0-mv)),max(length(float2(F.Load(int3(au,0)))/32.0-mv),length(float2(F.Load(int3(ad,0)))/32.0-mv)))<0.20;
// A clear photometric win always preserves the vector. In untextured regions,
// reject only weak/high-cost estimates; a coherent reliable field is retained.
// Preserve real subpixel motion: no minimum magnitude threshold. Textured
// regions retain optical flow; the fallback is confined to ambiguous regions.
float keep=(evidence>.003 || (local>.025 && errFlow<errZero+.012) || (local>.010 && coherent && cost<.25 && errFlow<errZero+.003))?1:0;
float2 result=keep*mv;
if(keep==0 && Camera[1].x>.5 && local<.025){
    float2 pos=(i.uv-.5)*float2(sw,sh)/max(sw,sh);
    float4 model=Camera[0];float2 global=model.xy+model.z*pos+model.w*float2(-pos.y,pos.x);
    float modelError=abs(now-Luma(Prev.SampleLevel(S,i.uv+global*texel,0).rgb));
    if(modelError<.025 && modelError<errZero+.008)result=global;
}
// Optical flow gives a reprojection address, not a visibility answer.  A
// vector can be perfectly coherent at an object boundary while its previous
// location contains the background (or vice versa).  DLSS must be told to
// favour the current sample in those disoccluded / unreliable regions.
//
// This is deliberately a single-frame, conservative mask: it has no CPU
// history and no blur/release phase.  `warpedResidual` finds genuine current
// versus reprojected disagreement; `flowBoundary` catches a moving silhouette
// even when a dark, low-contrast edge makes that residual weak.  Isolated film
// grain is rejected because it has neither a local flow boundary nor motion.
float flowBoundary=max(max(length(float2(F.Load(int3(al,0)))/32.0-mv),length(float2(F.Load(int3(ar,0)))/32.0-mv)),max(length(float2(F.Load(int3(au,0)))/32.0-mv),length(float2(F.Load(int3(ad,0)))/32.0-mv)));
// Use the raw optical-flow magnitude too.  The resolver may correctly reject
// an MV because its previous sample disagrees with the current image; that
// rejection is itself a strong reason to suppress temporal history.
float moving=saturate((max(length(result),length(mv))-.08)/.85);
float structural=saturate((flowBoundary-.18)/.82);
float residual=saturate((errFlow-.018)/.090);
float lumaStructure=saturate((local-.006)/.050);
float unreliable=saturate((cost-.45)/.45)*moving;
float mismatch=residual*(.25+.75*max(structural,moving*lumaStructure));
float silhouette=structural*moving*(.20+.80*lumaStructure);
// White means prefer the current colour.  It is bound both as the legacy
// BiasCurrentColor resource and as NGX disocclusion/responsivity hints.
o.bias=P.w<-.5?0:(P.w>.5?1:saturate(max(max(mismatch,silhouette),unreliable)));
o.mv=result*P.xy;return o;}
)";
