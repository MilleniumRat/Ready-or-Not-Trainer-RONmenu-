// Made by MilleniumRat (github.com/MilleniumRat)
// Stretched resolution, CS style: copy the finished frame, then draw its centre part
// (1/factor of the width) back over the whole back buffer. Everything gets wider, you see
// less to the sides, exactly like playing 4:3 stretched on a 16:9 screen.
//
// D3D11 only. Every bit of pipeline state we touch is saved and restored, because Unreal's
// D3D11 RHI caches state and would otherwise render the next frame with ours.
#include "stretch.h"
#include "common.h"

#include <d3d11.h>
#include <d3dcompiler.h>

namespace {

const char* kShader = R"(
cbuffer C : register(b0) { float2 uvRange; float2 pad; };
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    V o;
    float2 t = float2((id << 1) & 2, id & 2);          // fullscreen triangle: (0,0) (2,0) (0,2)
    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = float2(uvRange.x + (uvRange.y - uvRange.x) * t.x, t.y);
    return o;
}
Texture2D tex : register(t0);
SamplerState smp : register(s0);
float4 ps(V i) : SV_Target { return tex.Sample(smp, i.uv); }
)";

typedef HRESULT(WINAPI* PFN_Compile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                     UINT, UINT, ID3DBlob**, ID3DBlob**);

struct Res11 {
    ID3D11Device* dev = nullptr;          // not owned
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11SamplerState* samp = nullptr;
    ID3D11RasterizerState* rs = nullptr;
    ID3D11BlendState* bs = nullptr;
    ID3D11DepthStencilState* dss = nullptr;
    ID3D11Texture2D* copy = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    UINT w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    bool failed = false;
} r;

float g_applied = 1.f;
std::string g_status = "off";

template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

DXGI_FORMAT Typed(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default: return f;
    }
}

bool Compile(const char* entry, const char* target, ID3DBlob** out) {
    static PFN_Compile compile = nullptr;
    if (!compile) {
        HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
        if (m) compile = reinterpret_cast<PFN_Compile>(GetProcAddress(m, "D3DCompile"));
    }
    if (!compile) return false;
    ID3DBlob* err = nullptr;
    HRESULT hr = compile(kShader, strlen(kShader), "stretch", nullptr, nullptr, entry, target, 0, 0, out, &err);
    if (err) { Log("stretch shader %s: %s", entry, (const char*)err->GetBufferPointer()); err->Release(); }
    return SUCCEEDED(hr);
}

bool CreatePipeline(ID3D11Device* dev) {
    ID3DBlob *vsb = nullptr, *psb = nullptr;
    if (!Compile("vs", "vs_5_0", &vsb) || !Compile("ps", "ps_5_0", &psb)) { Rel(vsb); Rel(psb); return false; }
    bool ok = SUCCEEDED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &r.vs)) &&
              SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &r.ps));
    Rel(vsb); Rel(psb);
    if (!ok) return false;

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    D3D11_BLEND_DESC bld{};
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = FALSE;
    dsd.StencilEnable = FALSE;
    return SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &r.cb)) &&
           SUCCEEDED(dev->CreateSamplerState(&sd, &r.samp)) &&
           SUCCEEDED(dev->CreateRasterizerState(&rd, &r.rs)) &&
           SUCCEEDED(dev->CreateBlendState(&bld, &r.bs)) &&
           SUCCEEDED(dev->CreateDepthStencilState(&dsd, &r.dss));
}

bool EnsureCopy(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& bbd) {
    if (r.copy && r.w == bbd.Width && r.h == bbd.Height && r.fmt == bbd.Format) return true;
    Rel(r.srv); Rel(r.copy);
    D3D11_TEXTURE2D_DESC d = bbd;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = 0;
    d.MiscFlags = 0;
    d.MipLevels = 1;
    d.ArraySize = 1;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &r.copy))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = Typed(bbd.Format);
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    if (FAILED(dev->CreateShaderResourceView(r.copy, &sv, &r.srv))) return false;
    r.w = bbd.Width; r.h = bbd.Height; r.fmt = bbd.Format;
    return true;
}

// Everything the draw below changes.
struct Saved {
    UINT nVp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    ID3D11RasterizerState* rs; ID3D11BlendState* bs; FLOAT bf[4]; UINT mask;
    ID3D11DepthStencilState* dss; UINT sref;
    ID3D11RenderTargetView* rtv; ID3D11DepthStencilView* dsv;
    ID3D11ShaderResourceView* srv; ID3D11SamplerState* samp; ID3D11Buffer* cb;
    ID3D11VertexShader* vs; ID3D11PixelShader* ps; ID3D11GeometryShader* gs; ID3D11HullShader* hs; ID3D11DomainShader* ds;
    ID3D11ClassInstance *vsI[256], *psI[256], *gsI[256], *hsI[256], *dsI[256];
    UINT vsN = 256, psN = 256, gsN = 256, hsN = 256, dsN = 256;
    D3D11_PRIMITIVE_TOPOLOGY topo; ID3D11InputLayout* il;
    ID3D11Buffer* vb; UINT vbStride, vbOffset;
    ID3D11Buffer* ib; DXGI_FORMAT ibFmt; UINT ibOffset;
};

void Save(ID3D11DeviceContext* c, Saved& s) {
    c->RSGetViewports(&s.nVp, s.vp);
    c->RSGetState(&s.rs);
    c->OMGetBlendState(&s.bs, s.bf, &s.mask);
    c->OMGetDepthStencilState(&s.dss, &s.sref);
    c->OMGetRenderTargets(1, &s.rtv, &s.dsv);
    c->PSGetShaderResources(0, 1, &s.srv);
    c->PSGetSamplers(0, 1, &s.samp);
    c->VSGetConstantBuffers(0, 1, &s.cb);
    c->VSGetShader(&s.vs, s.vsI, &s.vsN);
    c->PSGetShader(&s.ps, s.psI, &s.psN);
    c->GSGetShader(&s.gs, s.gsI, &s.gsN);
    c->HSGetShader(&s.hs, s.hsI, &s.hsN);
    c->DSGetShader(&s.ds, s.dsI, &s.dsN);
    c->IAGetPrimitiveTopology(&s.topo);
    c->IAGetInputLayout(&s.il);
    c->IAGetVertexBuffers(0, 1, &s.vb, &s.vbStride, &s.vbOffset);
    c->IAGetIndexBuffer(&s.ib, &s.ibFmt, &s.ibOffset);
}

void Restore(ID3D11DeviceContext* c, Saved& s) {
    c->RSSetViewports(s.nVp, s.vp);
    c->RSSetState(s.rs);
    c->OMSetBlendState(s.bs, s.bf, s.mask);
    c->OMSetDepthStencilState(s.dss, s.sref);
    c->OMSetRenderTargets(1, &s.rtv, s.dsv);
    c->PSSetShaderResources(0, 1, &s.srv);
    c->PSSetSamplers(0, 1, &s.samp);
    c->VSSetConstantBuffers(0, 1, &s.cb);
    c->VSSetShader(s.vs, s.vsI, s.vsN);
    c->PSSetShader(s.ps, s.psI, s.psN);
    c->GSSetShader(s.gs, s.gsI, s.gsN);
    c->HSSetShader(s.hs, s.hsI, s.hsN);
    c->DSSetShader(s.ds, s.dsI, s.dsN);
    c->IASetPrimitiveTopology(s.topo);
    c->IASetInputLayout(s.il);
    c->IASetVertexBuffers(0, 1, &s.vb, &s.vbStride, &s.vbOffset);
    c->IASetIndexBuffer(s.ib, s.ibFmt, s.ibOffset);
    // drop the references the Get* calls took
    Rel(s.rs); Rel(s.bs); Rel(s.dss); Rel(s.rtv); Rel(s.dsv); Rel(s.srv); Rel(s.samp); Rel(s.cb);
    Rel(s.vs); Rel(s.ps); Rel(s.gs); Rel(s.hs); Rel(s.ds); Rel(s.il); Rel(s.vb); Rel(s.ib);
    for (UINT i = 0; i < s.vsN; ++i) Rel(s.vsI[i]);
    for (UINT i = 0; i < s.psN; ++i) Rel(s.psI[i]);
    for (UINT i = 0; i < s.gsN; ++i) Rel(s.gsI[i]);
    for (UINT i = 0; i < s.hsN; ++i) Rel(s.hsI[i]);
    for (UINT i = 0; i < s.dsN; ++i) Rel(s.dsI[i]);
}

}  // namespace

namespace stretch {

float Applied() { return g_applied; }
const std::string& Status() { return g_status; }

void SetUnsupported(const char* why) {
    g_applied = 1.f;
    g_status = why;
}

void Apply11(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* bb, ID3D11RenderTargetView* rtv, float factor) {
    g_applied = 1.f;
    if (factor <= 1.001f) { g_status = "off"; return; }
    if (r.failed) return;
    if (r.dev != dev) { Release11(); r.dev = dev; }
    if (!r.vs && !CreatePipeline(dev)) {
        r.failed = true;
        g_status = "none: could not build the stretch shader (d3dcompiler_47.dll missing?)";
        Log("stretch: pipeline creation failed");
        return;
    }
    D3D11_TEXTURE2D_DESC bbd{};
    bb->GetDesc(&bbd);
    if (bbd.SampleDesc.Count > 1) { g_status = "none: multisampled back buffer is not supported"; return; }
    if (!EnsureCopy(dev, bbd)) { g_status = "none: could not create the frame copy"; return; }

    // which part of the frame to show: the middle 1/factor of the width
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(r.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    float half = 0.5f / factor;
    float data[4] = { 0.5f - half, 0.5f + half, 0.f, 0.f };
    memcpy(m.pData, data, sizeof data);
    ctx->Unmap(r.cb, 0);

    Saved s;
    Save(ctx, s);

    ctx->CopyResource(r.copy, bb);
    D3D11_VIEWPORT vp{ 0, 0, (float)bbd.Width, (float)bbd.Height, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(r.rs);
    const float bf[4] = {};
    ctx->OMSetBlendState(r.bs, bf, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(r.dss, 0);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer* nullVb = nullptr; UINT zero = 0;
    ctx->IASetVertexBuffers(0, 1, &nullVb, &zero, &zero);
    ctx->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
    ctx->VSSetShader(r.vs, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &r.cb);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(r.ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &r.srv);
    ctx->PSSetSamplers(0, 1, &r.samp);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);

    Restore(ctx, s);
    g_applied = factor;
    char b[96];
    snprintf(b, sizeof b, "ok: x%.2f (%ux%u shown as %ux%u)", factor, (unsigned)(bbd.Width / factor), bbd.Height,
             bbd.Width, bbd.Height);
    g_status = b;
}

void Release11() {
    Rel(r.vs); Rel(r.ps); Rel(r.cb); Rel(r.samp); Rel(r.rs); Rel(r.bs); Rel(r.dss); Rel(r.srv); Rel(r.copy);
    r.w = r.h = 0;
    r.fmt = DXGI_FORMAT_UNKNOWN;
    r.dev = nullptr;
    r.failed = false;
}

}  // namespace stretch
