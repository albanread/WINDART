// WINDART game pane — the layered Direct3D 11 engine (thread 0 / UI-pump only).
//
// The Windows/D3D11 port of macdart/cocoa/gamepane/gp_engine.{h,mm} (the Metal
// engine). Object-model map + gpApply semantics: gamepane-design/
// GP_ENGINE_D3D_DESIGN.md (signed off, arch-notes/S6_design_signoff.md). Every
// CPU method (pset/line/circle/fill/cls/load, sprite parse/tick/hit/place, the
// text raster, the blitter's CPU-mirror op, the SMF builder) ports VERBATIM from
// the .mm; only the GPU objects beneath change (Metal -> D3D11).
//
// Owned by the UI isolate: every method is reached from a dart:win native on the
// pump thread, so it runs single-threaded by construction — no locks, no
// cross-thread D3D (as the mac engine is thread-0 only).
//
// Frames render into a persistent OFFSCREEN BGRA render target; gpsnap reads
// honest pixels back from it (CopyResource -> STAGING -> Map -> WIC PNG),
// independent of any swapchain. Live on-screen present (a DXGI swapchain on a
// child HWND) is DEFERRED this landing — see S6b_NOTES.md; the offscreen + gpsnap
// path is the milestone proof.
#ifndef WINDART_GP_ENGINE_D3D_H_
#define WINDART_GP_ENGINE_D3D_H_

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace windart_gamepane {

using Microsoft::WRL::ComPtr;

class GpSfx;   // gp_audio_win.h — XAudio2 SFX (lazily created; process-lifetime)

const int kNumBuffers = 8;         // FRONT=0, BACK=1, 2..7 asset slots
const int kFront = 0;
const int kBack = 1;

// Shared GPU state the engine owns and the panes borrow (non-owning pointers).
struct GpGfx {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  ID3D11RasterizerState* raster = nullptr;    // CULL_NONE (the big triangle is CCW)
  ID3D11BlendState* blendAlpha = nullptr;     // straight src-alpha over
  ID3D11SamplerState* sampler = nullptr;      // POINT + CLAMP (text)
};

// --- layer 1: the 8-bit indexed framebuffer ---------------------------------
class GpIndexedPane {
 public:
  GpIndexedPane(GpGfx* gfx, int world_w, int world_h,
                int viewport_w, int viewport_h, std::string* err);

  void set_active(int slot);
  void swap_buffers();
  void set_scroll(int64_t x, int64_t y);
  void set_rgb(uint8_t index, uint8_t r, uint8_t g, uint8_t b);        // 16..255
  void set_line_rgb(int line, uint8_t index, uint8_t r, uint8_t g, uint8_t b);
  void load_default_palette();
  void cls(uint8_t index);
  void pset(int64_t x, int64_t y, uint8_t index);
  uint8_t pget(int64_t x, int64_t y);
  void load(const uint8_t* data, size_t len);
  void fill_rect(int64_t x, int64_t y, int64_t w, int64_t h, uint8_t index);
  void line(int64_t x0, int64_t y0, int64_t x1, int64_t y1, uint8_t index);
  void circle(int64_t cx, int64_t cy, int64_t r, uint8_t index);
  void disc(int64_t cx, int64_t cy, int64_t r, uint8_t index);
  void upload();                   // dirty slots + palette -> GPU
  void render(ID3D11RenderTargetView* rtv, bool clear);

  int world_w() const { return world_w_; }
  int world_h() const { return world_h_; }
  int viewport_w() const { return viewport_w_; }
  int viewport_h() const { return viewport_h_; }
  int64_t scroll_x() const { return scroll_x_; }
  int64_t scroll_y() const { return scroll_y_; }
  int active() const { return active_; }
  std::vector<uint8_t>& buffer(int slot) { return buffers_[slot]; }
  bool dirty(int slot) const { return dirty_[slot]; }
  void set_dirty(int slot, bool d) { dirty_[slot] = d; }
  // The shared palette (per-line 0..15 + global 16..255) — tile layers resolve
  // through it too, so all layers agree on colour.
  ID3D11ShaderResourceView* palette_srv() const { return palette_srv_.Get(); }

 private:
  GpGfx* gfx_;
  int world_w_, world_h_, viewport_w_, viewport_h_;
  int active_;
  int64_t scroll_x_, scroll_y_;
  std::vector<uint8_t> buffers_[kNumBuffers];
  ComPtr<ID3D11Texture2D> tex_[kNumBuffers];
  ComPtr<ID3D11ShaderResourceView> srv_[kNumBuffers];
  bool dirty_[kNumBuffers];
  std::vector<float> palette_;     // float4 per entry
  ComPtr<ID3D11Buffer> palette_buf_;
  ComPtr<ID3D11ShaderResourceView> palette_srv_;
  bool palette_dirty_;
  ComPtr<ID3D11Buffer> cb_;        // IndexedUniforms (b0)
  ComPtr<ID3D11VertexShader> vs_;
  ComPtr<ID3D11PixelShader> ps_;
};

// --- layers 1 & 2: indexed tile layers (RASM tilemaps + parallax) -----------
// A background layer: a tile ATLAS (R8_UINT, `count` tiles stacked vertically,
// tile_w x tile_h each) + a MAP (R8_UINT, cols x rows cells -> tile id; id 0 =
// empty cell) + an independent scroll that WRAPS as a torus (infinite parallax
// from a small map). Resolves through the SHARED palette in the pixel shader
// (index 0 = the only transparency), composited behind the free-draw pane. Two
// instances (bg0/bg1) give classic multi-speed parallax.
class GpTileLayer {
 public:
  GpTileLayer(GpGfx* gfx, int viewport_w, int viewport_h, std::string* err);
  void set_tileset(const uint8_t* atlas, int tile_w, int tile_h, int count);
  void set_map(const uint8_t* map, int cols, int rows);
  void set_scroll(double x, double y) { scroll_x_ = x; scroll_y_ = y; }
  void clear() { cols_ = 0; rows_ = 0; }               // hide the layer
  bool ready() const {
    return cols_ > 0 && rows_ > 0 && tile_count_ > 0 && atlas_srv_ && map_srv_;
  }
  // Composite this layer (discarding empty cells / index-0 pixels) into rtv,
  // resolving colour through the caller's shared palette SRV.
  void render(ID3D11RenderTargetView* rtv, ID3D11ShaderResourceView* palette);

 private:
  GpGfx* gfx_;
  int viewport_w_, viewport_h_;
  int tile_w_, tile_h_, tile_count_;
  int cols_, rows_;
  double scroll_x_, scroll_y_;
  ComPtr<ID3D11Texture2D> atlas_tex_;
  ComPtr<ID3D11ShaderResourceView> atlas_srv_;
  ComPtr<ID3D11Texture2D> map_tex_;
  ComPtr<ID3D11ShaderResourceView> map_srv_;
  ComPtr<ID3D11Buffer> cb_;            // TileUniforms (b0), 32 bytes
  ComPtr<ID3D11VertexShader> vs_;
  ComPtr<ID3D11PixelShader> ps_;
};

// --- layer 2: sprites --------------------------------------------------------
struct GpSpriteDef {
  int w, h;
  std::vector<ComPtr<ID3D11Texture2D>> frames;
  std::vector<ComPtr<ID3D11ShaderResourceView>> frame_srv;
  float palette[16][4];
  ComPtr<ID3D11Buffer> palette_buf;
  ComPtr<ID3D11ShaderResourceView> palette_srv;
  bool palette_dirty;
};

struct GpSpriteInstance {
  int def;
  double x, y, scale, rot_deg, alpha;
  int frame;
  bool visible;
  double fps, accum;
};

class GpSprites {
 public:
  GpSprites(GpGfx* gfx, std::string* err);

  int define(const char* rows);
  bool add_frame(int def, const char* rows);
  void set_rgb(int def, int index, uint8_t r, uint8_t g, uint8_t b);
  int place(int def, double x, double y);
  GpSpriteInstance* instance(int id);
  bool hit(int a, int b);
  void tick(double dt);
  void render(ID3D11RenderTargetView* rtv, double scroll_x, double scroll_y,
              double vw, double vh);

 private:
  GpGfx* gfx_;
  ComPtr<ID3D11VertexShader> vs_;
  ComPtr<ID3D11PixelShader> ps_;
  ComPtr<ID3D11InputLayout> layout_;
  ComPtr<ID3D11Buffer> vb_;        // dynamic per-instance quad
  ComPtr<ID3D11Buffer> cb_;        // SpriteUniforms (b0)
  std::vector<GpSpriteDef> defs_;
  std::vector<GpSpriteInstance> instances_;
};

// --- the CPU blitter ---------------------------------------------------------
// This landing runs ALL blit modes on the CPU mirror + reupload (the design's
// "CPU-mirror baseline" applied uniformly): copy/transparent/and/or/xor/clear
// are computed into the destination slot's CPU buffer (verbatim from
// gp_engine.mm:743-760), then the slot is marked dirty so upload() re-pushes it
// via UpdateSubresource. Always correct; needs no R8_UINT typed-UAV support. The
// GPU compute-blitter (cs_blit_*) is a cap-gated optimisation, DEFERRED.
class GpBlitter {
 public:
  // mode: 0 copy, 1 transparent, 2 and, 3 or, 4 xor, 5 clear(value)
  void blit(GpIndexedPane* pane, int mode, int src, int dst,
            int64_t sx, int64_t sy, int64_t dx, int64_t dy,
            int64_t w, int64_t h, uint8_t value);
};

// --- layer 3: the text overlay ----------------------------------------------
class GpTextOverlay {
 public:
  GpTextOverlay(GpGfx* gfx, int w, int h, std::string* err);
  void clear();
  void draw_text(int64_t x, int64_t y, const char* text,
                 uint8_t r, uint8_t g, uint8_t b);
  void upload();
  void render(ID3D11RenderTargetView* rtv);

 private:
  void set_px(int64_t x, int64_t y, uint8_t r, uint8_t g, uint8_t b);
  void fill_px(int64_t x, int64_t y, int64_t w, int64_t h,
               uint8_t r, uint8_t g, uint8_t b);
  GpGfx* gfx_;
  int w_, h_;
  std::vector<uint8_t> rgba_;
  bool dirty_;
  ComPtr<ID3D11Texture2D> tex_;
  ComPtr<ID3D11ShaderResourceView> srv_;
  ComPtr<ID3D11VertexShader> vs_;
  ComPtr<ID3D11PixelShader> ps_;
};

// --- layer 0: the shader background -----------------------------------------
// Compiles the game's fragment body at runtime via D3DCompile. Game bodies are
// expected to be HLSL (GP_ENGINE_D3D_DESIGN.md §5.5); an MSL body (as the stock
// demos ship) fails to compile and the pane simply keeps its black background —
// the compile error is returned as a string, never an abort.
class GpShaderPane {
 public:
  GpShaderPane(GpGfx* gfx);
  std::string compile(const char* frag_hlsl);   // "" on success, else error
  bool ready() const { return ps_ != nullptr; }
  void set_param(int i, float v);
  void set_aspect(float a) { aspect_ = a; }
  void render(ID3D11RenderTargetView* rtv);

 private:
  GpGfx* gfx_;
  ComPtr<ID3D11VertexShader> vs_;
  ComPtr<ID3D11PixelShader> ps_;
  ComPtr<ID3D11Buffer> cb_;        // ShaderUniforms (b0), 144 bytes
  double start_time_;
  float params_[8];
  float aspect_;
};

// --- the engine --------------------------------------------------------------
class GpEngine {
 public:
  static GpEngine* instance();

  // Returns a non-zero handle on success (headless: a sentinel; a real child
  // HWND once live-present lands), 0 on failure (err set). direct mode (§6b) is
  // DEFERRED — open() with direct=true fails cleanly.
  int64_t open(int w, int h, int world_w, int world_h, bool direct,
               std::string* err);
  void close();
  bool is_open() const { return open_; }
  bool is_direct() const { return direct_; }

  GpIndexedPane* pane() { return pane_; }
  GpSprites* sprites() { return sprites_; }
  GpBlitter* blitter() { return blitter_; }
  GpTextOverlay* text() { return text_; }
  GpShaderPane* shader() { return shader_; }
  GpTileLayer* bg(int i) { return (i >= 0 && i < 2) ? bg_[i] : nullptr; }
  GpSfx* sfx();                    // lazily started XAudio2 SFX (T3 audio)

  void set_fullscreen(bool on) { fullscreen_ = on; }   // no live window yet
  bool fullscreen() const { return fullscreen_; }

  // --- T3 live on-screen present -------------------------------------------
  // A DXGI flip-model swapchain is bound to the game widget's child HWND (created
  // by win_view's kGame, modelled on kCanvas). set_present_target() records the
  // HWND; the swapchain is (re)created lazily on the first present after the
  // device exists, and each render_present() letterbox-blits the offscreen RT
  // into the backbuffer and Present()s — so the game is VISIBLE and animates.
  void set_present_target(HWND hwnd);
  void detach_present();
  bool has_present() const { return present_hwnd_ != nullptr; }
  // Read back the on-screen backbuffer (the letterboxed present image) to a PNG —
  // the honest "what the window shows" proof, distinct from snap()'s offscreen.
  bool snap_present(const char* path, std::string* err);

  void begin_frame() {}
  void render_present();
  bool snap(const char* path, std::string* err);   // offscreen -> PNG

  int frames_presented() const { return frames_; }
  int logical_w() const { return logical_w_; }
  int logical_h() const { return logical_h_; }
  int direct_stride() const { return 0; }

 private:
  GpEngine();
  bool ensure_device(std::string* err);
  bool ensure_present_pipeline(std::string* err);   // vs/ps_present + sampler + offscreen SRV
  bool ensure_swapchain(std::string* err);          // (re)create swapchain to fit the HWND
  void present_blit_to_backbuffer();                // clear + letterbox-blit offscreen -> backbuffer

  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> ctx_;
  ComPtr<ID3D11RasterizerState> raster_;
  ComPtr<ID3D11BlendState> blend_alpha_;
  ComPtr<ID3D11SamplerState> sampler_;
  GpGfx gfx_;

  ComPtr<ID3D11Texture2D> offscreen_;
  ComPtr<ID3D11RenderTargetView> offscreen_rtv_;
  ComPtr<ID3D11ShaderResourceView> offscreen_srv_;   // offscreen as source for present
  ComPtr<ID3D11Texture2D> staging_;

  // live present (T3)
  HWND present_hwnd_;
  ComPtr<IDXGISwapChain1> swapchain_;
  ComPtr<ID3D11RenderTargetView> backbuffer_rtv_;
  int sc_w_, sc_h_;
  ComPtr<ID3D11VertexShader> vs_present_;
  ComPtr<ID3D11PixelShader> ps_present_;
  ComPtr<ID3D11SamplerState> present_sampler_;       // LINEAR/CLAMP

  GpIndexedPane* pane_;
  GpTileLayer* bg_[2];              // RASM layers 1 & 2 (behind the indexed pane)
  GpSprites* sprites_;
  GpBlitter* blitter_;
  GpTextOverlay* text_;
  GpShaderPane* shader_;
  GpSfx* sfx_;                      // process-lifetime (survives game close/re-open)

  bool fullscreen_, direct_, open_;
  int logical_w_, logical_h_;
  int frames_;
  int64_t last_qpc_;
  int64_t qpc_freq_;
};

}  // namespace windart_gamepane

#endif  // WINDART_GP_ENGINE_D3D_H_
