// WINDART game pane — Direct3D 11 engine implementation. The D3D11 port of
// gp_engine.mm (Metal). GPU objects are D3D11; all CPU logic (drawing, sprite
// parse/animation, text raster, blitter mirror op) ports VERBATIM from the .mm.
// gamepane-design/GP_ENGINE_D3D_DESIGN.md is the spec; shaders.hlsl is the
// already-translated HLSL, split per-pipeline into the source strings below
// (one cbuffer per register(b0), so each pipeline compiles independently).
#define WIN32_LEAN_AND_MEAN
#include "gp_engine_d3d.h"
#include "gp_audio_win.h"   // GpSfx (T3 audio)

#include <objbase.h>
#include <d3dcompiler.h>
#include <wincodec.h>

#include <math.h>
#include <string.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace windart_gamepane {

// ============================================================================
// HLSL sources (shaders.hlsl, split per pipeline — see file header).
// ============================================================================

static const char* kIndexedHlsl =
    "struct VOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "cbuffer IndexedUniforms : register(b0) {\n"
    "  float scroll_x; float scroll_y; float viewport_w; float viewport_h;\n"
    "};\n"
    "Texture2D<uint>          indexTex : register(t0);\n"
    "StructuredBuffer<float4> palette  : register(t1);\n"
    "VOut vs_indexed(uint vid : SV_VertexID) {\n"
    "  float2 positions[3] = { float2(-1.0,-1.0), float2(3.0,-1.0), float2(-1.0,3.0) };\n"
    "  float2 pos = positions[vid];\n"
    "  VOut o; o.pos = float4(pos, 0.0, 1.0);\n"
    "  o.uv = float2((pos.x+1.0)*0.5, 1.0-(pos.y+1.0)*0.5); return o;\n"
    "}\n"
    "float4 ps_indexed(VOut input) : SV_Target {\n"
    "  uint screenX = uint(input.uv.x * viewport_w);\n"
    "  uint screenY = uint(input.uv.y * viewport_h);\n"
    "  uint worldX = uint(int(screenX) + int(scroll_x));\n"
    "  uint worldY = uint(int(screenY) + int(scroll_y));\n"
    "  uint ci = indexTex.Load(int3(worldX, worldY, 0));\n"
    "  if (ci == 0u) { discard; }\n"
    "  uint k;\n"
    "  if (ci < 16u) { k = screenY*16u + ci; }\n"
    "  else { k = uint(viewport_h)*16u + (ci - 16u); }\n"
    "  return palette[k];\n"
    "}\n";

// Tile layers (RASM layers 1 & 2): a screen pixel -> world pixel (+ scroll, torus
// wrap) -> tile cell -> tile id (mapTex) -> pixel in the atlas -> palette resolve.
// Tile id 0 = empty cell; index 0 within a tile = transparent. Shares the pane's
// palette (per-line 0..15 keyed by screen scanline, global 16..255).
static const char* kTileHlsl =
    "struct VOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "cbuffer TileUniforms : register(b0) {\n"
    "  float scroll_x; float scroll_y; float viewport_w; float viewport_h;\n"
    "  float tile_w; float tile_h; float map_cols; float map_rows;\n"
    "};\n"
    "Texture2D<uint>          atlasTex : register(t0);\n"
    "Texture2D<uint>          mapTex   : register(t1);\n"
    "StructuredBuffer<float4> palette  : register(t2);\n"
    "VOut vs_tile(uint vid : SV_VertexID) {\n"
    "  float2 positions[3] = { float2(-1.0,-1.0), float2(3.0,-1.0), float2(-1.0,3.0) };\n"
    "  float2 pos = positions[vid];\n"
    "  VOut o; o.pos = float4(pos, 0.0, 1.0);\n"
    "  o.uv = float2((pos.x+1.0)*0.5, 1.0-(pos.y+1.0)*0.5); return o;\n"
    "}\n"
    "float4 ps_tile(VOut input) : SV_Target {\n"
    "  int screenX = int(input.uv.x * viewport_w);\n"
    "  int screenY = int(input.uv.y * viewport_h);\n"
    "  int tw = int(tile_w), th = int(tile_h);\n"
    "  int worldW = int(map_cols) * tw;\n"
    "  int worldH = int(map_rows) * th;\n"
    "  int wx = screenX + int(scroll_x);\n"
    "  int wy = screenY + int(scroll_y);\n"
    "  wx = ((wx % worldW) + worldW) % worldW;\n"
    "  wy = ((wy % worldH) + worldH) % worldH;\n"
    "  int cx = wx / tw, cy = wy / th;\n"
    "  uint tid = mapTex.Load(int3(cx, cy, 0));\n"
    "  if (tid == 0u) { discard; }\n"
    "  int px = wx - cx*tw, py = wy - cy*th;\n"
    "  uint ci = atlasTex.Load(int3(px, int(tid)*th + py, 0));\n"
    "  if (ci == 0u) { discard; }\n"
    "  uint k;\n"
    "  if (ci < 16u) { k = uint(screenY)*16u + ci; }\n"
    "  else { k = uint(viewport_h)*16u + (ci - 16u); }\n"
    "  return palette[k];\n"
    "}\n";

static const char* kSpriteHlsl =
    "struct SVIn  { float2 pos : POSITION; float2 uv : TEXCOORD0; };\n"
    "struct SVOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "cbuffer SpriteUniforms : register(b0) { float alpha; float3 _pad; };\n"
    "Texture2D<uint>          spriteTex : register(t0);\n"
    "StructuredBuffer<float4> spritePal : register(t1);\n"
    "SVOut vs_sprite(SVIn v) { SVOut o; o.pos = float4(v.pos,0.0,1.0); o.uv = v.uv; return o; }\n"
    "float4 ps_sprite(SVOut input) : SV_Target {\n"
    "  uint w,h; spriteTex.GetDimensions(w,h);\n"
    "  uint2 texel = uint2(input.uv.x*float(w), input.uv.y*float(h));\n"
    "  uint ci = spriteTex.Load(int3(texel,0));\n"
    "  if (ci == 0u) { discard; }\n"
    "  float4 c = spritePal[ci]; c.a *= alpha; return c;\n"
    "}\n";

static const char* kTextHlsl =
    "struct TVOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "Texture2D    textTex : register(t0);\n"
    "SamplerState textSmp : register(s0);\n"
    "TVOut vs_text(uint vid : SV_VertexID) {\n"
    "  float2 positions[3] = { float2(-1.0,-1.0), float2(3.0,-1.0), float2(-1.0,3.0) };\n"
    "  float2 pos = positions[vid];\n"
    "  TVOut o; o.pos = float4(pos,0.0,1.0);\n"
    "  o.uv = float2((pos.x+1.0)*0.5, 1.0-(pos.y+1.0)*0.5); return o;\n"
    "}\n"
    "float4 ps_text(TVOut input) : SV_Target { return textTex.Sample(textSmp, input.uv); }\n";

// The present blit: a fullscreen triangle sampling the offscreen BGRA render
// target into the swapchain backbuffer. Letterboxing is done by the D3D viewport
// (the backbuffer is cleared to black first, then a viewport fitted to the
// logical aspect is set, and the triangle samples UV 0..1 into it) — no per-pixel
// letterbox math needed. Screen line 0 stays at the top (the UV flip mirrors the
// engine's other passes; no vertical flip is introduced).
static const char* kPresentHlsl =
    "struct PVOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "Texture2D    srcTex : register(t0);\n"
    "SamplerState srcSmp : register(s0);\n"
    "PVOut vs_present(uint vid : SV_VertexID) {\n"
    "  float2 positions[3] = { float2(-1.0,-1.0), float2(3.0,-1.0), float2(-1.0,3.0) };\n"
    "  float2 pos = positions[vid];\n"
    "  PVOut o; o.pos = float4(pos,0.0,1.0);\n"
    "  o.uv = float2((pos.x+1.0)*0.5, 1.0-(pos.y+1.0)*0.5); return o;\n"
    "}\n"
    "float4 ps_present(PVOut input) : SV_Target { return srcTex.Sample(srcSmp, input.uv); }\n";

// The runtime shader background: this header + the game's fmain body.
static const char* kShaderHeaderHlsl =
    "cbuffer ShaderUniforms : register(b0) {\n"
    "  float time; float aspect; float2 _pad_shader; float p[8];\n"
    "};\n"
    "struct GVOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "GVOut vs_shader(uint vid : SV_VertexID) {\n"
    "  float2 positions[3] = { float2(-1.0,-1.0), float2(3.0,-1.0), float2(-1.0,3.0) };\n"
    "  float2 pos = positions[vid];\n"
    "  GVOut o; o.pos = float4(pos,0.0,1.0);\n"
    "  o.uv = float2((pos.x+1.0)*0.5, 1.0-(pos.y+1.0)*0.5); return o;\n"
    "}\n";

// ============================================================================
// Small GPU helpers
// ============================================================================

static bool CompileBlob(const char* src, const char* entry, const char* target,
                        ID3DBlob** blob, std::string* err) {
  ID3DBlob* errB = nullptr;
  HRESULT hr = D3DCompile(src, strlen(src), "gp", nullptr, nullptr, entry, target,
                          D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, blob, &errB);
  if (FAILED(hr)) {
    if (err) *err = errB ? std::string((const char*)errB->GetBufferPointer())
                         : "D3DCompile failed";
    if (errB) errB->Release();
    return false;
  }
  if (errB) errB->Release();
  return true;
}

static bool MakeVS(ID3D11Device* dev, const char* src, const char* entry,
                   ComPtr<ID3D11VertexShader>& vs, ComPtr<ID3DBlob>* blobOut,
                   std::string* err) {
  ComPtr<ID3DBlob> b;
  if (!CompileBlob(src, entry, "vs_5_0", &b, err)) return false;
  if (FAILED(dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(),
                                     nullptr, &vs))) {
    if (err) *err = "CreateVertexShader failed";
    return false;
  }
  if (blobOut) *blobOut = b;
  return true;
}

static bool MakePS(ID3D11Device* dev, const char* src, const char* entry,
                   ComPtr<ID3D11PixelShader>& ps, std::string* err) {
  ComPtr<ID3DBlob> b;
  if (!CompileBlob(src, entry, "ps_5_0", &b, err)) return false;
  if (FAILED(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(),
                                    nullptr, &ps))) {
    if (err) *err = "CreatePixelShader failed";
    return false;
  }
  return true;
}

static ComPtr<ID3D11Buffer> MakeCB(ID3D11Device* dev, UINT bytes) {
  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = bytes;
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  ComPtr<ID3D11Buffer> b;
  dev->CreateBuffer(&bd, nullptr, &b);
  return b;
}

static bool MakeStructured(ID3D11Device* dev, UINT numElems, UINT stride,
                           ComPtr<ID3D11Buffer>& buf,
                           ComPtr<ID3D11ShaderResourceView>& srv) {
  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = numElems * stride;
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  bd.StructureByteStride = stride;
  if (FAILED(dev->CreateBuffer(&bd, nullptr, &buf))) return false;
  D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
  sd.Format = DXGI_FORMAT_UNKNOWN;
  sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
  sd.Buffer.FirstElement = 0;
  sd.Buffer.NumElements = numElems;
  return SUCCEEDED(dev->CreateShaderResourceView(buf.Get(), &sd, &srv));
}

static void UploadDynamic(ID3D11DeviceContext* ctx, ID3D11Buffer* b,
                          const void* data, size_t bytes) {
  D3D11_MAPPED_SUBRESOURCE m;
  if (SUCCEEDED(ctx->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
    memcpy(m.pData, data, bytes);
    ctx->Unmap(b, 0);
  }
}

// R8_UINT texture. initData != NULL -> IMMUTABLE (sprite frame); NULL ->
// DEFAULT (index slot, later UpdateSubresource'd).
static bool MakeIndexTex(ID3D11Device* dev, int w, int h, const uint8_t* initData,
                         ComPtr<ID3D11Texture2D>& tex,
                         ComPtr<ID3D11ShaderResourceView>& srv) {
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8_UINT;
  td.SampleDesc.Count = 1;
  td.Usage = initData ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA srd = {};
  srd.pSysMem = initData;
  srd.SysMemPitch = (UINT)w;
  if (FAILED(dev->CreateTexture2D(&td, initData ? &srd : nullptr, &tex))) return false;
  return SUCCEEDED(dev->CreateShaderResourceView(tex.Get(), nullptr, &srv));
}

static const float kBlack[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

// ============================================================================
// GpIndexedPane
// ============================================================================

GpIndexedPane::GpIndexedPane(GpGfx* gfx, int world_w, int world_h,
                             int viewport_w, int viewport_h, std::string* err)
    : gfx_(gfx), world_w_(world_w), world_h_(world_h),
      viewport_w_(viewport_w), viewport_h_(viewport_h),
      active_(kFront), scroll_x_(0), scroll_y_(0), palette_dirty_(true) {
  if (world_w_ < viewport_w_ || world_h_ < viewport_h_) {
    if (err) *err = "gamepane: world smaller than viewport";
    return;
  }
  size_t n = (size_t)world_w_ * (size_t)world_h_;
  for (int i = 0; i < kNumBuffers; i++) {
    buffers_[i].assign(n, 0);
    if (!MakeIndexTex(gfx_->dev, world_w_, world_h_, nullptr, tex_[i], srv_[i])) {
      if (err) *err = "gamepane: index texture create failed";
      return;
    }
    dirty_[i] = true;
  }
  size_t entries = (size_t)viewport_h_ * 16 + 240;
  palette_.assign(entries * 4, 0.0f);
  if (!MakeStructured(gfx_->dev, (UINT)entries, 16, palette_buf_, palette_srv_)) {
    if (err) *err = "gamepane: palette buffer create failed";
    return;
  }
  cb_ = MakeCB(gfx_->dev, 16);
  ComPtr<ID3DBlob> vsb;
  if (!MakeVS(gfx_->dev, kIndexedHlsl, "vs_indexed", vs_, &vsb, err)) return;
  if (!MakePS(gfx_->dev, kIndexedHlsl, "ps_indexed", ps_, err)) return;
  load_default_palette();
}

void GpIndexedPane::set_active(int slot) { active_ = slot; }

void GpIndexedPane::swap_buffers() {
  buffers_[kFront].swap(buffers_[kBack]);
  tex_[kFront].Swap(tex_[kBack]);
  srv_[kFront].Swap(srv_[kBack]);
  bool d = dirty_[kFront];
  dirty_[kFront] = dirty_[kBack];
  dirty_[kBack] = d;
  if (active_ == kFront) active_ = kBack;
  else if (active_ == kBack) active_ = kFront;
}

void GpIndexedPane::set_scroll(int64_t x, int64_t y) {
  int64_t mx = world_w_ - viewport_w_, my = world_h_ - viewport_h_;
  scroll_x_ = x < 0 ? 0 : (x > mx ? mx : x);
  scroll_y_ = y < 0 ? 0 : (y > my ? my : y);
}

void GpIndexedPane::set_rgb(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
  if (index < 16) return;
  size_t k = ((size_t)viewport_h_ * 16 + (index - 16)) * 4;
  palette_[k] = r / 255.0f; palette_[k+1] = g / 255.0f;
  palette_[k+2] = b / 255.0f; palette_[k+3] = 1.0f;
  palette_dirty_ = true;
}

void GpIndexedPane::set_line_rgb(int line, uint8_t index,
                                 uint8_t r, uint8_t g, uint8_t b) {
  if (index < 1 || index > 15 || line < 0 || line >= viewport_h_) return;
  size_t k = ((size_t)line * 16 + index) * 4;
  palette_[k] = r / 255.0f; palette_[k+1] = g / 255.0f;
  palette_[k+2] = b / 255.0f; palette_[k+3] = 1.0f;
  palette_dirty_ = true;
}

static void HsvToRgb(double h, double s, double v,
                     uint8_t* r, uint8_t* g, uint8_t* b) {
  double i = floor(h * 6.0);
  double f = h * 6.0 - i;
  double p = v * (1.0 - s);
  double q = v * (1.0 - f * s);
  double t = v * (1.0 - (1.0 - f) * s);
  double rr = 0, gg = 0, bb = 0;
  switch (((int)i) % 6) {
    case 0: rr = v; gg = t; bb = p; break;
    case 1: rr = q; gg = v; bb = p; break;
    case 2: rr = p; gg = v; bb = t; break;
    case 3: rr = p; gg = q; bb = v; break;
    case 4: rr = t; gg = p; bb = v; break;
    case 5: rr = v; gg = p; bb = q; break;
  }
  *r = (uint8_t)(rr * 255.0); *g = (uint8_t)(gg * 255.0); *b = (uint8_t)(bb * 255.0);
}

void GpIndexedPane::load_default_palette() {
  for (int line = 0; line < viewport_h_; line++) {
    for (int i = 1; i < 16; i++) {
      uint8_t v = (uint8_t)(i * 255 / 15);
      set_line_rgb(line, (uint8_t)i, v, v, v);
    }
  }
  for (int i = 16; i < 256; i++) {
    uint8_t r, g, b;
    HsvToRgb((i - 16) / 240.0, 1.0, 1.0, &r, &g, &b);
    set_rgb((uint8_t)i, r, g, b);
  }
}

void GpIndexedPane::cls(uint8_t index) {
  memset(buffers_[active_].data(), index, buffers_[active_].size());
  dirty_[active_] = true;
}

void GpIndexedPane::pset(int64_t x, int64_t y, uint8_t index) {
  if (x < 0 || y < 0 || x >= world_w_ || y >= world_h_) return;
  buffers_[active_][(size_t)y * world_w_ + x] = index;
  dirty_[active_] = true;
}

uint8_t GpIndexedPane::pget(int64_t x, int64_t y) {
  if (x < 0 || y < 0 || x >= world_w_ || y >= world_h_) return 0;
  return buffers_[active_][(size_t)y * world_w_ + x];
}

void GpIndexedPane::load(const uint8_t* data, size_t len) {
  size_t n = buffers_[active_].size();
  if (len > n) len = n;
  memcpy(buffers_[active_].data(), data, len);
  dirty_[active_] = true;
}

void GpIndexedPane::fill_rect(int64_t x, int64_t y, int64_t w, int64_t h,
                              uint8_t index) {
  for (int64_t yy = y; yy < y + h; yy++) {
    if (yy < 0 || yy >= world_h_) continue;
    int64_t x0 = x < 0 ? 0 : x;
    int64_t x1 = x + w > world_w_ ? world_w_ : x + w;
    if (x0 >= x1) continue;
    memset(&buffers_[active_][(size_t)yy * world_w_ + x0], index, (size_t)(x1 - x0));
  }
  dirty_[active_] = true;
}

void GpIndexedPane::line(int64_t x0, int64_t y0, int64_t x1, int64_t y1,
                         uint8_t index) {
  int64_t dx = llabs(x1 - x0), dy = -llabs(y1 - y0);
  int64_t sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
  int64_t e = dx + dy;
  for (;;) {
    pset(x0, y0, index);
    if (x0 == x1 && y0 == y1) break;
    int64_t e2 = 2 * e;
    if (e2 >= dy) { e += dy; x0 += sx; }
    if (e2 <= dx) { e += dx; y0 += sy; }
  }
}

void GpIndexedPane::circle(int64_t cx, int64_t cy, int64_t r, uint8_t index) {
  if (r < 0) return;
  int64_t x = r, y = 0, e = 1 - r;
  while (x >= y) {
    pset(cx + x, cy + y, index); pset(cx - x, cy + y, index);
    pset(cx + x, cy - y, index); pset(cx - x, cy - y, index);
    pset(cx + y, cy + x, index); pset(cx - y, cy + x, index);
    pset(cx + y, cy - x, index); pset(cx - y, cy - x, index);
    y++;
    if (e < 0) { e += 2 * y + 1; }
    else { x--; e += 2 * (y - x) + 1; }
  }
}

void GpIndexedPane::disc(int64_t cx, int64_t cy, int64_t r, uint8_t index) {
  if (r < 0) return;
  int64_t x = r, y = 0, e = 1 - r;
  while (x >= y) {
    fill_rect(cx - x, cy + y, 2 * x + 1, 1, index);
    fill_rect(cx - x, cy - y, 2 * x + 1, 1, index);
    fill_rect(cx - y, cy + x, 2 * y + 1, 1, index);
    fill_rect(cx - y, cy - x, 2 * y + 1, 1, index);
    y++;
    if (e < 0) { e += 2 * y + 1; }
    else { x--; e += 2 * (y - x) + 1; }
  }
}

void GpIndexedPane::upload() {
  if (palette_dirty_) {
    UploadDynamic(gfx_->ctx, palette_buf_.Get(), palette_.data(),
                  palette_.size() * sizeof(float));
    palette_dirty_ = false;
  }
  for (int i = 0; i < kNumBuffers; i++) {
    if (!dirty_[i]) continue;
    gfx_->ctx->UpdateSubresource(tex_[i].Get(), 0, nullptr, buffers_[i].data(),
                                 (UINT)world_w_, 0);
    dirty_[i] = false;
  }
}

void GpIndexedPane::render(ID3D11RenderTargetView* rtv, bool clear) {
  if (!ps_) return;
  ID3D11DeviceContext* ctx = gfx_->ctx;
  ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
  if (clear) ctx->ClearRenderTargetView(rtv, kBlack);
  float u[4] = { (float)scroll_x_, (float)scroll_y_,
                 (float)viewport_w_, (float)viewport_h_ };
  ctx->UpdateSubresource(cb_.Get(), 0, nullptr, u, 0, 0);
  ctx->IASetInputLayout(nullptr);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(vs_.Get(), nullptr, 0);
  ctx->PSSetShader(ps_.Get(), nullptr, 0);
  ctx->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
  ID3D11ShaderResourceView* srvs[2] = { srv_[kFront].Get(), palette_srv_.Get() };
  ctx->PSSetShaderResources(0, 2, srvs);
  ctx->Draw(3, 0);
}

// ============================================================================
// GpTileLayer — indexed tilemap layers (RASM layers 1 & 2)
// ============================================================================

GpTileLayer::GpTileLayer(GpGfx* gfx, int viewport_w, int viewport_h,
                         std::string* err)
    : gfx_(gfx), viewport_w_(viewport_w), viewport_h_(viewport_h),
      tile_w_(0), tile_h_(0), tile_count_(0), cols_(0), rows_(0),
      scroll_x_(0.0), scroll_y_(0.0) {
  cb_ = MakeCB(gfx_->dev, 32);                 // 8 floats
  ComPtr<ID3DBlob> vsb;
  if (!MakeVS(gfx_->dev, kTileHlsl, "vs_tile", vs_, &vsb, err)) return;
  if (!MakePS(gfx_->dev, kTileHlsl, "ps_tile", ps_, err)) return;
}

void GpTileLayer::set_tileset(const uint8_t* atlas, int tile_w, int tile_h,
                              int count) {
  if (!atlas || tile_w <= 0 || tile_h <= 0 || count <= 0) return;
  tile_w_ = tile_w; tile_h_ = tile_h; tile_count_ = count;
  atlas_srv_.Reset(); atlas_tex_.Reset();
  MakeIndexTex(gfx_->dev, tile_w, tile_h * count, atlas, atlas_tex_, atlas_srv_);
}

void GpTileLayer::set_map(const uint8_t* map, int cols, int rows) {
  if (!map || cols <= 0 || rows <= 0) { cols_ = 0; rows_ = 0; return; }
  cols_ = cols; rows_ = rows;
  map_srv_.Reset(); map_tex_.Reset();
  MakeIndexTex(gfx_->dev, cols, rows, map, map_tex_, map_srv_);
}

void GpTileLayer::render(ID3D11RenderTargetView* rtv,
                         ID3D11ShaderResourceView* palette) {
  (void)rtv;                                   // RTV is bound once by render_present
  if (!ready() || !ps_) return;
  ID3D11DeviceContext* ctx = gfx_->ctx;
  ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);   // discard-composite (index 0)
  float u[8] = { (float)scroll_x_, (float)scroll_y_,
                 (float)viewport_w_, (float)viewport_h_,
                 (float)tile_w_, (float)tile_h_, (float)cols_, (float)rows_ };
  ctx->UpdateSubresource(cb_.Get(), 0, nullptr, u, 0, 0);
  ctx->IASetInputLayout(nullptr);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(vs_.Get(), nullptr, 0);
  ctx->PSSetShader(ps_.Get(), nullptr, 0);
  ctx->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
  ID3D11ShaderResourceView* srvs[3] = { atlas_srv_.Get(), map_srv_.Get(), palette };
  ctx->PSSetShaderResources(0, 3, srvs);
  ctx->Draw(3, 0);
}

// ============================================================================
// GpSprites
// ============================================================================

GpSprites::GpSprites(GpGfx* gfx, std::string* err) : gfx_(gfx) {
  ComPtr<ID3DBlob> vsb;
  if (!MakeVS(gfx_->dev, kSpriteHlsl, "vs_sprite", vs_, &vsb, err)) return;
  if (!MakePS(gfx_->dev, kSpriteHlsl, "ps_sprite", ps_, err)) return;
  D3D11_INPUT_ELEMENT_DESC ied[2] = {
    { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
  };
  if (FAILED(gfx_->dev->CreateInputLayout(ied, 2, vsb->GetBufferPointer(),
                                          vsb->GetBufferSize(), &layout_))) {
    if (err) *err = "gamepane: sprite input layout failed";
    return;
  }
  D3D11_BUFFER_DESC vbd = {};
  vbd.ByteWidth = 4 * 4 * sizeof(float);   // 4 verts * {x,y,u,v}
  vbd.Usage = D3D11_USAGE_DYNAMIC;
  vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  gfx_->dev->CreateBuffer(&vbd, nullptr, &vb_);
  cb_ = MakeCB(gfx_->dev, 16);
}

static bool ParseSpriteRows(const char* rows, int* w, int* h,
                            std::vector<uint8_t>* px) {
  if (rows == NULL || rows[0] == '\0') return false;
  std::vector<std::string> lines;
  std::string cur;
  for (const char* p = rows; ; p++) {
    if (*p == '/' || *p == '\0') {
      lines.push_back(cur); cur.clear();
      if (*p == '\0') break;
    } else {
      cur.push_back(*p);
    }
  }
  if (lines.empty() || lines[0].empty()) return false;
  size_t width = lines[0].size();
  px->clear();
  for (size_t i = 0; i < lines.size(); i++) {
    if (lines[i].size() != width) return false;
    for (size_t j = 0; j < width; j++) {
      char c = lines[i][j];
      uint8_t v;
      if (c == '.') v = 0;
      else if (c >= '0' && c <= '9') v = (uint8_t)(c - '0');
      else if (c >= 'a' && c <= 'f') v = (uint8_t)(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v = (uint8_t)(c - 'A' + 10);
      else return false;
      px->push_back(v);
    }
  }
  *w = (int)width; *h = (int)lines.size();
  return true;
}

int GpSprites::define(const char* rows) {
  int w = 0, h = 0;
  std::vector<uint8_t> px;
  if (!ParseSpriteRows(rows, &w, &h, &px)) return -1;
  GpSpriteDef def;
  def.w = w; def.h = h;
  ComPtr<ID3D11Texture2D> tex;
  ComPtr<ID3D11ShaderResourceView> srv;
  if (!MakeIndexTex(gfx_->dev, w, h, px.data(), tex, srv)) return -1;
  def.frames.push_back(tex);
  def.frame_srv.push_back(srv);
  memset(def.palette, 0, sizeof(def.palette));
  for (int i = 0; i < 16; i++) def.palette[i][3] = 1.0f;
  if (!MakeStructured(gfx_->dev, 16, 16, def.palette_buf, def.palette_srv)) return -1;
  def.palette_dirty = true;
  defs_.push_back(def);
  return (int)defs_.size() - 1;
}

bool GpSprites::add_frame(int def, const char* rows) {
  if (def < 0 || def >= (int)defs_.size()) return false;
  int w = 0, h = 0;
  std::vector<uint8_t> px;
  if (!ParseSpriteRows(rows, &w, &h, &px)) return false;
  if (w != defs_[def].w || h != defs_[def].h) return false;
  ComPtr<ID3D11Texture2D> tex;
  ComPtr<ID3D11ShaderResourceView> srv;
  if (!MakeIndexTex(gfx_->dev, w, h, px.data(), tex, srv)) return false;
  defs_[def].frames.push_back(tex);
  defs_[def].frame_srv.push_back(srv);
  return true;
}

void GpSprites::set_rgb(int def, int index, uint8_t r, uint8_t g, uint8_t b) {
  if (def < 0 || def >= (int)defs_.size() || index < 0 || index > 15) return;
  defs_[def].palette[index][0] = r / 255.0f;
  defs_[def].palette[index][1] = g / 255.0f;
  defs_[def].palette[index][2] = b / 255.0f;
  defs_[def].palette[index][3] = 1.0f;
  defs_[def].palette_dirty = true;
}

int GpSprites::place(int def, double x, double y) {
  if (def < 0 || def >= (int)defs_.size()) return -1;
  GpSpriteInstance inst;
  inst.def = def; inst.x = x; inst.y = y;
  inst.scale = 1.0; inst.rot_deg = 0.0; inst.alpha = 1.0;
  inst.frame = 0; inst.visible = true; inst.fps = 0.0; inst.accum = 0.0;
  instances_.push_back(inst);
  return (int)instances_.size() - 1;
}

GpSpriteInstance* GpSprites::instance(int id) {
  if (id < 0 || id >= (int)instances_.size()) return NULL;
  return &instances_[id];
}

bool GpSprites::hit(int a, int b) {
  GpSpriteInstance* ia = instance(a);
  GpSpriteInstance* ib = instance(b);
  if (ia == NULL || ib == NULL) return false;
  const GpSpriteDef& da = defs_[ia->def];
  const GpSpriteDef& db = defs_[ib->def];
  double ahw = da.w * ia->scale / 2.0, ahh = da.h * ia->scale / 2.0;
  double bhw = db.w * ib->scale / 2.0, bhh = db.h * ib->scale / 2.0;
  return ia->x - ahw < ib->x + bhw && ia->x + ahw > ib->x - bhw &&
         ia->y - ahh < ib->y + bhh && ia->y + ahh > ib->y - bhh;
}

void GpSprites::tick(double dt) {
  for (size_t i = 0; i < instances_.size(); i++) {
    GpSpriteInstance& inst = instances_[i];
    if (inst.fps <= 0.0) continue;
    size_t frames = defs_[inst.def].frames.size();
    if (frames <= 1) continue;
    inst.accum += dt;
    double period = 1.0 / inst.fps;
    while (inst.accum >= period) {
      inst.accum -= period;
      inst.frame = (int)((inst.frame + 1) % (int)frames);
    }
  }
}

void GpSprites::render(ID3D11RenderTargetView* rtv, double scroll_x,
                       double scroll_y, double vw, double vh) {
  (void)rtv;
  if (!ps_ || instances_.empty()) return;
  ID3D11DeviceContext* ctx = gfx_->ctx;
  for (size_t i = 0; i < defs_.size(); i++) {
    if (defs_[i].palette_dirty) {
      UploadDynamic(ctx, defs_[i].palette_buf.Get(), defs_[i].palette, 256);
      defs_[i].palette_dirty = false;
    }
  }
  ctx->OMSetBlendState(gfx_->blendAlpha, nullptr, 0xffffffff);
  ctx->IASetInputLayout(layout_.Get());
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  ctx->VSSetShader(vs_.Get(), nullptr, 0);
  ctx->PSSetShader(ps_.Get(), nullptr, 0);
  UINT stride = 4 * sizeof(float), offset = 0;
  for (size_t i = 0; i < instances_.size(); i++) {
    const GpSpriteInstance& inst = instances_[i];
    if (!inst.visible) continue;
    const GpSpriteDef& def = defs_[inst.def];
    double hw = def.w * inst.scale / 2.0, hh = def.h * inst.scale / 2.0;
    double theta = inst.rot_deg * (3.14159265358979323846 / 180.0);
    double c = cos(theta), s = sin(theta);
    double cx = inst.x - scroll_x, cy = inst.y - scroll_y;
    double lx[4] = { -hw, hw, -hw, hw };
    double ly[4] = { -hh, -hh, hh, hh };
    float uv[4][2] = { {0, 0}, {1, 0}, {0, 1}, {1, 1} };
    float verts[16];
    for (int v = 0; v < 4; v++) {
      double rx = lx[v] * c - ly[v] * s;
      double ry = lx[v] * s + ly[v] * c;
      double sxp = cx + rx, syp = cy + ry;
      verts[v * 4] = (float)((sxp / vw) * 2.0 - 1.0);
      verts[v * 4 + 1] = (float)(1.0 - (syp / vh) * 2.0);
      verts[v * 4 + 2] = uv[v][0];
      verts[v * 4 + 3] = uv[v][1];
    }
    float alpha = (float)inst.alpha;
    float cbuf[4] = { alpha, 0, 0, 0 };
    int frame = inst.frame;
    if (frame >= (int)def.frames.size()) frame = (int)def.frames.size() - 1;
    if (frame < 0) frame = 0;
    UploadDynamic(ctx, vb_.Get(), verts, sizeof(verts));
    ctx->UpdateSubresource(cb_.Get(), 0, nullptr, cbuf, 0, 0);
    ctx->IASetVertexBuffers(0, 1, vb_.GetAddressOf(), &stride, &offset);
    ctx->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
    ID3D11ShaderResourceView* srvs[2] = { def.frame_srv[frame].Get(),
                                          def.palette_srv.Get() };
    ctx->PSSetShaderResources(0, 2, srvs);
    ctx->Draw(4, 0);
  }
}

// ============================================================================
// GpBlitter (CPU mirror + reupload, all modes)
// ============================================================================

void GpBlitter::blit(GpIndexedPane* pane, int mode, int src, int dst,
                     int64_t sx, int64_t sy, int64_t dx, int64_t dy,
                     int64_t w, int64_t h, uint8_t value) {
  if (pane == NULL) return;
  int ww = pane->world_w(), wh = pane->world_h();
  if (sx < 0) { dx -= sx; w += sx; sx = 0; }
  if (sy < 0) { dy -= sy; h += sy; sy = 0; }
  if (dx < 0) { sx -= dx; w += dx; dx = 0; }
  if (dy < 0) { sy -= dy; h += dy; dy = 0; }
  if (mode == 5) { sx = 0; sy = 0; }
  if (w > ww - sx) w = ww - sx;
  if (h > wh - sy) h = wh - sy;
  if (w > ww - dx) w = ww - dx;
  if (h > wh - dy) h = wh - dy;
  if (w <= 0 || h <= 0) return;

  std::vector<uint8_t>& sbuf = pane->buffer(mode == 5 ? dst : src);
  std::vector<uint8_t>& dbuf = pane->buffer(dst);
  for (int64_t yy = 0; yy < h; yy++) {
    for (int64_t xx = 0; xx < w; xx++) {
      size_t si = (size_t)(sy + yy) * ww + (sx + xx);
      size_t di = (size_t)(dy + yy) * ww + (dx + xx);
      uint8_t sv = sbuf[si];
      switch (mode) {
        case 0: dbuf[di] = sv; break;
        case 1: if (sv != 0) dbuf[di] = sv; break;
        case 2: dbuf[di] = (uint8_t)(sv & dbuf[di]); break;
        case 3: dbuf[di] = (uint8_t)(sv | dbuf[di]); break;
        case 4: dbuf[di] = (uint8_t)(sv ^ dbuf[di]); break;
        case 5: dbuf[di] = value; break;
      }
    }
  }
  pane->set_dirty(dst, true);   // reupload the corrected slot on next upload()
}

// ============================================================================
// GpTextOverlay
// ============================================================================

static const int kCellW = 8, kCellH = 12;   // text cell: 8px advance, 12px tall

// --- font atlas: real ASCII glyphs (32..126), rasterized ONCE from a fixed-pitch
// system font via GDI — no hand-transcribed bitmap, the OS supplies the glyphs.
// kFontGlyphs[c-32][row] is an 8-wide column bitmask: bit `col` set = the glyph
// inks pixel (col,row). Replaces the old 7-segment raster (digits ok, letters as
// hollow boxes) with the full printable set. Built lazily on first draw_text.
static const int kFontW = 8, kFontH = kCellH;
static uint16_t kFontGlyphs[95][kFontH];
static bool g_font_ready = false;

static void EnsureFont() {
  if (g_font_ready) return;
  g_font_ready = true;                       // build once (on failure: all-blank)
  HDC dc = CreateCompatibleDC(nullptr);
  if (!dc) return;
  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = kFontW;
  bi.bmiHeader.biHeight = -kFontH;           // top-down (row 0 = top)
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;              // BGRA; width 8 -> 32B rows, already aligned
  void* bits = nullptr;
  HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!dib) { DeleteDC(dc); return; }
  HGDIOBJ oldbmp = SelectObject(dc, dib);
  // A crisp non-antialiased fixed-pitch face sized just under the cell so
  // ascenders/descenders stay inside the 8x12 box. OUT_TT_PRECIS forces the
  // TrueType face (a raster font would ignore the requested pixel size).
  HFONT font = CreateFontW(-(kFontH - 1), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
  HGDIOBJ oldfont = SelectObject(dc, font);
  SetBkColor(dc, RGB(0, 0, 0));
  SetTextColor(dc, RGB(255, 255, 255));
  SetBkMode(dc, OPAQUE);
  const uint32_t* px = static_cast<const uint32_t*>(bits);
  for (int ci = 0; ci < 95; ci++) {
    RECT rc = {0, 0, kFontW, kFontH};
    wchar_t ch = static_cast<wchar_t>(32 + ci);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &rc, &ch, 1, nullptr);   // fill black + draw white
    GdiFlush();
    for (int row = 0; row < kFontH; row++) {
      uint16_t m = 0;
      for (int col = 0; col < kFontW; col++)
        if ((px[row * kFontW + col] & 0xFF) > 110)             // B channel: white -> ink
          m |= static_cast<uint16_t>(1u << col);
      kFontGlyphs[ci][row] = m;
    }
  }
  SelectObject(dc, oldfont); DeleteObject(font);
  SelectObject(dc, oldbmp);  DeleteObject(dib);
  DeleteDC(dc);
}

GpTextOverlay::GpTextOverlay(GpGfx* gfx, int w, int h, std::string* err)
    : gfx_(gfx), w_(w), h_(h), dirty_(true) {
  rgba_.assign((size_t)w * h * 4, 0);
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DYNAMIC;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(gfx_->dev->CreateTexture2D(&td, nullptr, &tex_))) {
    if (err) *err = "gamepane: text texture create failed";
    return;
  }
  gfx_->dev->CreateShaderResourceView(tex_.Get(), nullptr, &srv_);
  ComPtr<ID3DBlob> vsb;
  if (!MakeVS(gfx_->dev, kTextHlsl, "vs_text", vs_, &vsb, err)) return;
  if (!MakePS(gfx_->dev, kTextHlsl, "ps_text", ps_, err)) return;
}

void GpTextOverlay::clear() {
  memset(rgba_.data(), 0, rgba_.size());
  dirty_ = true;
}

void GpTextOverlay::set_px(int64_t x, int64_t y, uint8_t r, uint8_t g, uint8_t b) {
  if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
  size_t i = ((size_t)y * w_ + x) * 4;
  rgba_[i] = r; rgba_[i + 1] = g; rgba_[i + 2] = b; rgba_[i + 3] = 255;
}

void GpTextOverlay::fill_px(int64_t x, int64_t y, int64_t w, int64_t h,
                            uint8_t r, uint8_t g, uint8_t b) {
  for (int64_t yy = y; yy < y + h; yy++)
    for (int64_t xx = x; xx < x + w; xx++) set_px(xx, yy, r, g, b);
}

void GpTextOverlay::draw_text(int64_t x, int64_t y, const char* text,
                              uint8_t r, uint8_t g, uint8_t b) {
  EnsureFont();
  for (const char* p = text; *p != '\0'; p++, x += kCellW) {
    unsigned char c = static_cast<unsigned char>(*p);
    if (c == ' ') continue;
    if (c < 32 || c > 126) c = '?';                 // map non-printables to a box
    const uint16_t* gl = kFontGlyphs[c - 32];
    for (int row = 0; row < kFontH; row++) {
      uint16_t m = gl[row];
      if (!m) continue;
      for (int col = 0; col < kFontW; col++)
        if (m & (1u << col)) set_px(x + col, y + row, r, g, b);
    }
  }
  dirty_ = true;
}

void GpTextOverlay::upload() {
  if (!dirty_) return;
  D3D11_MAPPED_SUBRESOURCE m;
  if (SUCCEEDED(gfx_->ctx->Map(tex_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
    for (int y = 0; y < h_; y++) {
      memcpy((uint8_t*)m.pData + (size_t)y * m.RowPitch,
             rgba_.data() + (size_t)y * w_ * 4, (size_t)w_ * 4);
    }
    gfx_->ctx->Unmap(tex_.Get(), 0);
  }
  dirty_ = false;
}

void GpTextOverlay::render(ID3D11RenderTargetView* rtv) {
  (void)rtv;
  if (!ps_) return;
  ID3D11DeviceContext* ctx = gfx_->ctx;
  ctx->OMSetBlendState(gfx_->blendAlpha, nullptr, 0xffffffff);
  ctx->IASetInputLayout(nullptr);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(vs_.Get(), nullptr, 0);
  ctx->PSSetShader(ps_.Get(), nullptr, 0);
  ctx->PSSetShaderResources(0, 1, srv_.GetAddressOf());
  ctx->PSSetSamplers(0, 1, &gfx_->sampler);
  ctx->Draw(3, 0);
}

// ============================================================================
// GpShaderPane
// ============================================================================

GpShaderPane::GpShaderPane(GpGfx* gfx)
    : gfx_(gfx), aspect_(1.0f) {
  for (int i = 0; i < 8; i++) params_[i] = 0.0f;
  LARGE_INTEGER c; QueryPerformanceCounter(&c);
  start_time_ = (double)c.QuadPart;
  cb_ = MakeCB(gfx_->dev, 144);
  std::string err;
  MakeVS(gfx_->dev, kShaderHeaderHlsl, "vs_shader", vs_, nullptr, &err);
}

std::string GpShaderPane::compile(const char* frag_hlsl) {
  std::string src = std::string(kShaderHeaderHlsl) + "\n" + frag_hlsl;
  std::string err;
  ComPtr<ID3D11PixelShader> ps;
  if (!MakePS(gfx_->dev, src.c_str(), "fmain", ps, &err)) {
    return err.empty() ? "shader compile failed" : err;
  }
  ps_ = ps;
  LARGE_INTEGER c; QueryPerformanceCounter(&c);
  start_time_ = (double)c.QuadPart;
  return "";
}

void GpShaderPane::set_param(int i, float v) {
  if (i >= 0 && i < 8) params_[i] = v;
}

void GpShaderPane::render(ID3D11RenderTargetView* rtv) {
  if (!ps_ || !vs_) return;
  ID3D11DeviceContext* ctx = gfx_->ctx;
  LARGE_INTEGER c, f;
  QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
  float buf[36] = { 0 };   // 144 bytes, HLSL cbuffer layout (§5.5)
  buf[0] = (float)(((double)c.QuadPart - start_time_) / (double)f.QuadPart);
  buf[1] = aspect_;
  for (int i = 0; i < 8; i++) buf[4 + i * 4] = params_[i];
  ctx->UpdateSubresource(cb_.Get(), 0, nullptr, buf, 0, 0);
  ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
  ctx->ClearRenderTargetView(rtv, kBlack);
  ctx->IASetInputLayout(nullptr);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(vs_.Get(), nullptr, 0);
  ctx->PSSetShader(ps_.Get(), nullptr, 0);
  ctx->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
  ctx->Draw(3, 0);
}

// ============================================================================
// GpEngine
// ============================================================================

GpEngine* GpEngine::instance() {
  static GpEngine* g = NULL;
  if (g == NULL) g = new GpEngine();
  return g;
}

GpEngine::GpEngine()
    : pane_(NULL), sprites_(NULL), blitter_(NULL), text_(NULL), shader_(NULL),
      sfx_(NULL),
      fullscreen_(false), direct_(false), open_(false),
      logical_w_(0), logical_h_(0), frames_(0), last_qpc_(0), qpc_freq_(1),
      present_hwnd_(NULL), sc_w_(0), sc_h_(0) { bg_[0] = NULL; bg_[1] = NULL; }

// SFX is process-lifetime (one IXAudio2 per process), created on first use and
// kept across game close/re-open — unlike the per-game D3D panes.
GpSfx* GpEngine::sfx() {
  if (sfx_ == NULL) sfx_ = new GpSfx();
  return sfx_;
}

bool GpEngine::ensure_device(std::string* err) {
  if (device_) return true;
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
  D3D_FEATURE_LEVEL got;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                 want, 2, D3D11_SDK_VERSION, &device_, &got, &ctx_);
  if (FAILED(hr)) {
    // Fall back to the WARP software rasterizer (fully D3D11-capable) so headless
    // hosts with no/limited GPU still render deterministically.
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                           want, 2, D3D11_SDK_VERSION, &device_, &got, &ctx_);
  }
  if (FAILED(hr)) { if (err) *err = "gamepane: no D3D11 device"; return false; }

  D3D11_RASTERIZER_DESC rd = {};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;          // the big triangle is CCW; never cull
  rd.DepthClipEnable = TRUE;
  device_->CreateRasterizerState(&rd, &raster_);

  D3D11_BLEND_DESC bd = {};
  bd.RenderTarget[0].BlendEnable = TRUE;
  bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
  bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
  bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  device_->CreateBlendState(&bd, &blend_alpha_);

  D3D11_SAMPLER_DESC sd = {};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  device_->CreateSamplerState(&sd, &sampler_);

  gfx_.dev = device_.Get();
  gfx_.ctx = ctx_.Get();
  gfx_.raster = raster_.Get();
  gfx_.blendAlpha = blend_alpha_.Get();
  gfx_.sampler = sampler_.Get();

  LARGE_INTEGER f; QueryPerformanceFrequency(&f);
  qpc_freq_ = f.QuadPart;
  return true;
}

int64_t GpEngine::open(int w, int h, int world_w, int world_h, bool direct,
                       std::string* err) {
  if (!ensure_device(err)) return 0;
  close();
  if (direct) {
    if (err) *err = "gamepane: direct framebuffer mode deferred (S6b)";
    return 0;
  }
  if (world_w < w) world_w = w;
  if (world_h < h) world_h = h;
  direct_ = false;
  logical_w_ = w; logical_h_ = h;

  D3D11_TEXTURE2D_DESC td = {};
  td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  offscreen_srv_.Reset();
  if (FAILED(device_->CreateTexture2D(&td, nullptr, &offscreen_)) ||
      FAILED(device_->CreateRenderTargetView(offscreen_.Get(), nullptr,
                                             &offscreen_rtv_)) ||
      FAILED(device_->CreateShaderResourceView(offscreen_.Get(), nullptr,
                                               &offscreen_srv_))) {
    if (err) *err = "gamepane: offscreen create failed";
    return 0;
  }
  staging_.Reset();   // recreated lazily by snap()
  // A new offscreen invalidates the swapchain's backbuffer sizing baseline; it is
  // re-created on the next present. Keep present_hwnd_ so a re-open re-attaches.
  swapchain_.Reset();
  backbuffer_rtv_.Reset();
  sc_w_ = sc_h_ = 0;

  std::string e;
  pane_ = new GpIndexedPane(&gfx_, world_w, world_h, w, h, &e);
  bg_[0] = new GpTileLayer(&gfx_, w, h, &e);   // RASM tile layers 1 & 2
  bg_[1] = new GpTileLayer(&gfx_, w, h, &e);
  sprites_ = new GpSprites(&gfx_, &e);
  blitter_ = new GpBlitter();
  text_ = new GpTextOverlay(&gfx_, w, h, &e);
  shader_ = new GpShaderPane(&gfx_);
  shader_->set_aspect((float)w / (float)h);
  open_ = true;
  frames_ = 0;
  last_qpc_ = 0;
  if (!e.empty()) {
    if (err) *err = e;
    close();
    return 0;
  }
  return 1;   // non-zero success sentinel (a real HWND once live-present lands)
}

void GpEngine::close() {
  delete pane_; pane_ = NULL;
  delete bg_[0]; bg_[0] = NULL;
  delete bg_[1]; bg_[1] = NULL;
  delete sprites_; sprites_ = NULL;
  delete blitter_; blitter_ = NULL;
  delete text_; text_ = NULL;
  delete shader_; shader_ = NULL;
  offscreen_rtv_.Reset();
  offscreen_srv_.Reset();
  offscreen_.Reset();
  staging_.Reset();
  // Keep the swapchain + present_hwnd_ across a game close/re-open (the widget's
  // HWND outlives a gpClose); the backbuffer RTV is rebuilt on the next present.
  backbuffer_rtv_.Reset();
  swapchain_.Reset();
  sc_w_ = sc_h_ = 0;
  direct_ = false;
  open_ = false;
}

// --- live present (T3): swapchain on the game widget's child HWND -------------

void GpEngine::set_present_target(HWND hwnd) {
  if (present_hwnd_ == hwnd) return;
  present_hwnd_ = hwnd;
  backbuffer_rtv_.Reset();
  swapchain_.Reset();       // rebound to the new HWND on the next present
  sc_w_ = sc_h_ = 0;
}

void GpEngine::detach_present() {
  present_hwnd_ = NULL;
  backbuffer_rtv_.Reset();
  swapchain_.Reset();
  sc_w_ = sc_h_ = 0;
}

bool GpEngine::ensure_present_pipeline(std::string* err) {
  if (ps_present_ && present_sampler_) return true;
  if (!device_) { if (err) *err = "gamepane: no device for present"; return false; }
  if (!vs_present_ && !MakeVS(device_.Get(), kPresentHlsl, "vs_present",
                              vs_present_, nullptr, err)) return false;
  if (!ps_present_ && !MakePS(device_.Get(), kPresentHlsl, "ps_present",
                              ps_present_, err)) return false;
  if (!present_sampler_) {
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;   // smooth upscale to the window
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&sd, &present_sampler_))) {
      if (err) *err = "gamepane: present sampler failed";
      return false;
    }
  }
  return true;
}

bool GpEngine::ensure_swapchain(std::string* err) {
  if (!present_hwnd_) { if (err) *err = "gamepane: no present target"; return false; }
  RECT rc; GetClientRect(present_hwnd_, &rc);
  int w = rc.right - rc.left, h = rc.bottom - rc.top;
  if (w <= 0) w = logical_w_ > 0 ? logical_w_ : 1;
  if (h <= 0) h = logical_h_ > 0 ? logical_h_ : 1;
  if (swapchain_ && backbuffer_rtv_ && sc_w_ == w && sc_h_ == h) return true;

  if (swapchain_) {
    backbuffer_rtv_.Reset();
    if (FAILED(swapchain_->ResizeBuffers(0, (UINT)w, (UINT)h,
                                         DXGI_FORMAT_UNKNOWN, 0))) {
      if (err) *err = "gamepane: ResizeBuffers failed";
      return false;
    }
  } else {
    ComPtr<IDXGIDevice> dxgiDev;
    if (FAILED(device_.As(&dxgiDev))) { if (err) *err = "gamepane: no IDXGIDevice"; return false; }
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDev->GetAdapter(&adapter))) { if (err) *err = "gamepane: no adapter"; return false; }
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
      if (err) *err = "gamepane: no IDXGIFactory2"; return false;
    }
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = (UINT)w; sd.Height = (UINT)h;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (FAILED(factory->CreateSwapChainForHwnd(device_.Get(), present_hwnd_, &sd,
                                               nullptr, nullptr, &swapchain_))) {
      if (err) *err = "gamepane: CreateSwapChainForHwnd failed";
      return false;
    }
    factory->MakeWindowAssociation(present_hwnd_, DXGI_MWA_NO_ALT_ENTER);
  }
  ComPtr<ID3D11Texture2D> bb;
  if (FAILED(swapchain_->GetBuffer(0, IID_PPV_ARGS(&bb))) ||
      FAILED(device_->CreateRenderTargetView(bb.Get(), nullptr, &backbuffer_rtv_))) {
    if (err) *err = "gamepane: backbuffer RTV failed";
    return false;
  }
  sc_w_ = w; sc_h_ = h;
  return true;
}

void GpEngine::present_blit_to_backbuffer() {
  if (!backbuffer_rtv_ || !offscreen_srv_ || !ps_present_) return;
  // Detach the offscreen as a render target before binding it as an SRV (it was
  // just written this frame), else the runtime unbinds it and warns.
  ID3D11RenderTargetView* nullRtv = nullptr;
  ctx_->OMSetRenderTargets(1, &nullRtv, nullptr);

  ctx_->OMSetRenderTargets(1, backbuffer_rtv_.GetAddressOf(), nullptr);
  ctx_->ClearRenderTargetView(backbuffer_rtv_.Get(), kBlack);

  // Letterbox: fit logical_w_ x logical_h_ into sc_w_ x sc_h_, preserving aspect.
  float cw = (float)sc_w_, ch = (float)sc_h_;
  float la = (float)logical_w_ / (float)(logical_h_ ? logical_h_ : 1);
  float ca = cw / (ch ? ch : 1);
  float vw, vh;
  if (ca > la) { vh = ch; vw = ch * la; } else { vw = cw; vh = cw / la; }
  D3D11_VIEWPORT vp = { (cw - vw) * 0.5f, (ch - vh) * 0.5f, vw, vh, 0.0f, 1.0f };
  ctx_->RSSetViewports(1, &vp);
  ctx_->RSSetState(raster_.Get());

  ctx_->IASetInputLayout(nullptr);
  ctx_->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
  ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx_->VSSetShader(vs_present_.Get(), nullptr, 0);
  ctx_->PSSetShader(ps_present_.Get(), nullptr, 0);
  ctx_->PSSetShaderResources(0, 1, offscreen_srv_.GetAddressOf());
  ctx_->PSSetSamplers(0, 1, present_sampler_.GetAddressOf());
  ctx_->OMSetBlendState(nullptr, nullptr, 0xffffffff);   // opaque copy
  ctx_->Draw(3, 0);

  ID3D11ShaderResourceView* nullSrv = nullptr;   // release the hazard for next frame
  ctx_->PSSetShaderResources(0, 1, &nullSrv);
}

void GpEngine::render_present() {
  if (!open_) return;
  LARGE_INTEGER c; QueryPerformanceCounter(&c);
  double dt = last_qpc_ == 0 ? 0.0
                             : (double)(c.QuadPart - last_qpc_) / (double)qpc_freq_;
  if (dt > 0.1) dt = 0.1;
  last_qpc_ = c.QuadPart;

  ctx_->OMSetRenderTargets(1, offscreen_rtv_.GetAddressOf(), nullptr);
  D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)logical_w_, (float)logical_h_, 0.0f, 1.0f };
  ctx_->RSSetViewports(1, &vp);
  ctx_->RSSetState(raster_.Get());

  sprites_->tick(dt);
  pane_->upload();
  text_->upload();
  bool has_shader = shader_->ready();
  if (has_shader) shader_->render(offscreen_rtv_.Get());
  else ctx_->ClearRenderTargetView(offscreen_rtv_.Get(), kBlack);   // clear once, up front
  bg_[0]->render(offscreen_rtv_.Get(), pane_->palette_srv());       // RASM tile layer 1
  bg_[1]->render(offscreen_rtv_.Get(), pane_->palette_srv());       // RASM tile layer 2
  pane_->render(offscreen_rtv_.Get(), false);                       // indexed pixels (bg cleared above)
  sprites_->render(offscreen_rtv_.Get(), (double)pane_->scroll_x(),
                   (double)pane_->scroll_y(), (double)logical_w_, (double)logical_h_);
  text_->render(offscreen_rtv_.Get());
  frames_++;

  // T3: if a game widget is on screen, blit this frame to its swapchain and
  // present it — the game is now live and animating in the window.
  if (present_hwnd_ && ensure_present_pipeline(nullptr) && ensure_swapchain(nullptr)) {
    present_blit_to_backbuffer();
    swapchain_->Present(1, 0);   // vsync-paced
  }
}

// --- gpsnap: offscreen -> WIC PNG (BGRA straight through) --------------------

static bool WriteRgbaPng(const std::vector<uint8_t>& rgba, int w, int h,
                         const char* path, std::string* err) {
  ComPtr<IWICImagingFactory> wic;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
    if (err) *err = "gamepane: WIC factory unavailable";
    return false;
  }
  int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
  std::wstring wpath(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, path, -1, &wpath[0], n);

  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> enc;
  ComPtr<IWICBitmapFrameEncode> frame;
  IPropertyBag2* bag = nullptr;
  bool ok = false;
  if (SUCCEEDED(wic->CreateStream(&stream)) &&
      SUCCEEDED(stream->InitializeFromFilename(wpath.c_str(), GENERIC_WRITE)) &&
      SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
      SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
      SUCCEEDED(enc->CreateNewFrame(&frame, &bag)) &&
      SUCCEEDED(frame->Initialize(bag))) {
    frame->SetSize(w, h);
    // The PNG encoder's native 32bpp format is BGRA, not RGBA — requesting RGBA
    // makes SetPixelFormat silently fall back to BGRA and misencode (R/B swap).
    // We feed straight BGRA bytes (the offscreen/backbuffer format) and declare it.
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&fmt);
    if (SUCCEEDED(frame->WritePixels(h, (UINT)(w * 4), (UINT)rgba.size(),
                                     const_cast<uint8_t*>(rgba.data()))) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit())) {
      ok = true;
    }
  }
  if (bag) bag->Release();
  if (!ok && err) *err = "gamepane: PNG encode failed";
  return ok;
}

bool GpEngine::snap(const char* path, std::string* err) {
  if (!open_ || !offscreen_) { if (err) *err = "gamepane: not open"; return false; }
  int w = logical_w_, h = logical_h_;
  if (!staging_) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &staging_))) {
      if (err) *err = "gamepane: staging create failed";
      return false;
    }
  }
  ctx_->CopyResource(staging_.Get(), offscreen_.Get());
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(ctx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &m))) {
    if (err) *err = "gamepane: staging map failed";
    return false;
  }
  std::vector<uint8_t> rgba((size_t)w * h * 4);
  for (int y = 0; y < h; y++) {
    const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
    for (int x = 0; x < w; x++) {
      rgba[((size_t)y * w + x) * 4 + 0] = row[x * 4 + 0];   // B
      rgba[((size_t)y * w + x) * 4 + 1] = row[x * 4 + 1];   // G
      rgba[((size_t)y * w + x) * 4 + 2] = row[x * 4 + 2];   // R
      rgba[((size_t)y * w + x) * 4 + 3] = 255;              // opaque; kept BGRA for WIC
    }
  }
  ctx_->Unmap(staging_.Get(), 0);
  return WriteRgbaPng(rgba, w, h, path, err);
}

// snap_present: the ON-SCREEN image (letterboxed present) read back from the
// swapchain backbuffer — proves what the live window actually shows, distinct
// from snap()'s raw offscreen. Blits the current offscreen into the backbuffer,
// copies it to a staging texture BEFORE Present (flip-model discards after), and
// PNG-encodes it, then presents so the window stays in sync.
bool GpEngine::snap_present(const char* path, std::string* err) {
  if (!open_) { if (err) *err = "gamepane: not open"; return false; }
  if (!present_hwnd_) { if (err) *err = "gamepane: no live window"; return false; }
  if (!ensure_present_pipeline(err) || !ensure_swapchain(err)) return false;
  present_blit_to_backbuffer();

  int w = sc_w_, h = sc_h_;
  ComPtr<ID3D11Texture2D> bb;
  if (FAILED(swapchain_->GetBuffer(0, IID_PPV_ARGS(&bb)))) {
    if (err) *err = "gamepane: present readback GetBuffer failed"; return false;
  }
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_STAGING;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> stg;
  if (FAILED(device_->CreateTexture2D(&td, nullptr, &stg))) {
    if (err) *err = "gamepane: present staging failed"; return false;
  }
  ctx_->CopyResource(stg.Get(), bb.Get());
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(ctx_->Map(stg.Get(), 0, D3D11_MAP_READ, 0, &m))) {
    if (err) *err = "gamepane: present map failed"; return false;
  }
  std::vector<uint8_t> rgba((size_t)w * h * 4);
  for (int y = 0; y < h; y++) {
    const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
    for (int x = 0; x < w; x++) {
      rgba[((size_t)y * w + x) * 4 + 0] = row[x * 4 + 0];
      rgba[((size_t)y * w + x) * 4 + 1] = row[x * 4 + 1];
      rgba[((size_t)y * w + x) * 4 + 2] = row[x * 4 + 2];
      rgba[((size_t)y * w + x) * 4 + 3] = 255;
    }
  }
  ctx_->Unmap(stg.Get(), 0);
  swapchain_->Present(0, 0);   // keep the window in sync with the captured frame
  return WriteRgbaPng(rgba, w, h, path, err);
}

}  // namespace windart_gamepane
