// galaxy-choya-native: pruebas de la ruta directa contra el motor GLES real de Choya, con Mesa (EGL sin superficie).
//
//  1. Equivalencia: la misma escena por la ruta directa y por el formato de captura que produce la ruta GX (vértices
//     big endian de GX, PipelineDesc con la disposición de Aurora) da los mismos píxeles.
//  2. Persistencia: varios cuadros con matrices distintas no reconstruyen la geometría (arena estable).
//  3. Materiales no soportados se rechazan con un motivo (no se dibujan a medias).
//  4. Coste de CPU de preparar un cuadro de N dibujos por la ruta directa (medido aquí: PC + llvmpipe, NO la Mali).
//
// Salida: "PRUEBA DIRECTA OK" y líneas "medida: ..." para docs/MEDICIONES.md. Código 2 si Mesa no da GLES 3.2.
#include "galaxy/choya_direct.hpp"

#include "choya_gx_core.hpp"

#include <dolphin/gx/GXEnum.h>
#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace gxc = choya::gx;
using namespace galaxy::direct;

namespace {
void* gEgl = nullptr;
void* gGles = nullptr;
void* (*gEglProc)(const char*) = nullptr;
int gFails = 0;

void* gl_proc(const char* name) {
  void* p = dlsym(gGles, name);
  return p ? p : (gEglProc ? gEglProc(name) : nullptr);
}
void quiet_log(const char* line) {
  if (std::getenv("GALAXY_TEST_VERBOSE")) std::printf("[motor] %s\n", line);
}

bool make_context(int w, int h) {
  gEgl = dlopen("libEGL.so.1", RTLD_NOW);
  gGles = dlopen("libGLESv2.so.2", RTLD_NOW);
  if (!gEgl || !gGles) return false;
  using Fn = void* (*)(...);
  auto sym = [](const char* n) { return dlsym(gEgl, n); };
  gEglProc = reinterpret_cast<void* (*)(const char*)>(sym("eglGetProcAddress"));
  auto getDisplay = reinterpret_cast<void* (*)(void*)>(sym("eglGetDisplay"));
  auto initialize = reinterpret_cast<unsigned (*)(void*, int*, int*)>(sym("eglInitialize"));
  auto bindApi = reinterpret_cast<unsigned (*)(unsigned)>(sym("eglBindAPI"));
  auto chooseConfig = reinterpret_cast<unsigned (*)(void*, const int*, void**, int, int*)>(sym("eglChooseConfig"));
  auto createPbuffer = reinterpret_cast<void* (*)(void*, void*, const int*)>(sym("eglCreatePbufferSurface"));
  auto createContext = reinterpret_cast<void* (*)(void*, void*, void*, const int*)>(sym("eglCreateContext"));
  auto makeCurrent = reinterpret_cast<unsigned (*)(void*, void*, void*, void*)>(sym("eglMakeCurrent"));
  (void)sizeof(Fn);
  void* dpy = getDisplay(nullptr);
  int major = 0, minor = 0;
  if (!dpy || !initialize(dpy, &major, &minor)) return false;
  bindApi(0x30A0);  // EGL_OPENGL_ES_API
  const int cfgAttrs[] = {0x3033, 0x0001, 0x3040, 0x0040, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3025, 24,
                          0x3038};  // PBUFFER, ES3, RGBA8, depth 24
  void* cfg = nullptr;
  int n = 0;
  if (!chooseConfig(dpy, cfgAttrs, &cfg, 1, &n) || n == 0) return false;
  const int pb[] = {0x3057, w, 0x3056, h, 0x3038};
  void* surf = createPbuffer(dpy, cfg, pb);
  const int ctxAttrs[] = {0x3098, 3, 0x30FB, 2, 0x3038};  // ES 3.2
  void* ctx = createContext(dpy, cfg, nullptr, ctxAttrs);
  if (!surf || !ctx || !makeCurrent(dpy, surf, surf, ctx)) return false;
  gxc::set_gl_loader(gl_proc);
  gxc::set_logger(quiet_log);
  return true;
}

std::vector<uint8_t> read_screen(int w, int h) {
  static auto readPixels = reinterpret_cast<void (*)(int, int, int, int, unsigned, unsigned, void*)>(
      dlsym(gGles, "glReadPixels"));
  std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
  readPixels(0, 0, w, h, 0x1908, 0x1401, px.data());
  return px;
}
// (x, y) con y = 0 arriba, como la pantalla de la Wii.
uint32_t at(const std::vector<uint8_t>& px, int w, int h, int x, int y) {
  const uint8_t* p = &px[(static_cast<size_t>(h - 1 - y) * w + x) * 4];
  return (uint32_t{p[0]} << 16) | (uint32_t{p[1]} << 8) | p[2];
}
void check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FALLO", what);
  if (!ok) ++gFails;
}

constexpr int W = 640, H = 480;

// Proyección ortográfica de pantalla (x 0..640 a la derecha, y 0..480 hacia abajo), fila mayor como GX.
void ortho(float p[4][4]) {
  std::memset(p, 0, sizeof(float) * 16);
  p[0][0] = 2.f / W;
  p[0][3] = -1.f;
  p[1][1] = -2.f / H;
  p[1][3] = 1.f;
  p[2][3] = 0.5f;
  p[3][3] = 1.f;
}
Mat34 translate(float x, float y) {
  Mat34 m{};
  m.m[0][0] = m.m[1][1] = m.m[2][2] = 1.f;
  m.m[0][3] = x;
  m.m[1][3] = y;
  return m;
}

// Material de un paso: color del rasterizador (canal 0, color de vértice).
MaterialDesc passthrough_material() {
  MaterialDesc m;
  TevStage s{};
  s.colorIn[0] = s.colorIn[1] = s.colorIn[2] = GX_CC_ZERO;
  s.colorIn[3] = GX_CC_RASC;
  s.colorOp = GX_TEV_ADD;
  s.colorScale = GX_CS_SCALE_1;
  s.colorClamp = 1;
  s.colorOut = GX_TEVPREV;
  s.alphaIn[0] = s.alphaIn[1] = s.alphaIn[2] = GX_CA_ZERO;
  s.alphaIn[3] = GX_CA_RASA;
  s.alphaOp = GX_TEV_ADD;
  s.alphaScale = GX_CS_SCALE_1;
  s.alphaClamp = 1;
  s.alphaOut = GX_TEVPREV;
  s.kcSel = GX_TEV_KCSEL_1;
  s.kaSel = GX_TEV_KASEL_1;
  s.channel = GX_COLOR0A0;
  m.stages.push_back(s);
  m.channels[0] = {GX_SRC_VTX, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE};
  m.channels[2] = {GX_SRC_VTX, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE};
  m.numChannels = 1;
  m.depthTest = false;
  m.depthWrite = false;
  m.depthFunc = GX_ALWAYS;
  m.cullMode = GX_CULL_NONE;
  m.colorUpdate = true;
  m.alphaUpdate = true;
  return m;
}

// Dos mallas: un cuadrado rojo (0..320 x 0..480) y un triángulo verde que se mueve con su matriz.
struct Scene {
  MeshId quad = 0, tri = 0;
  MaterialId mat = 0;
};
Scene build_scene(Device& dev) {
  Scene s;
  const float qp[] = {0, 0, 0, 320, 0, 0, 320, 480, 0, 0, 480, 0};
  const uint8_t qc[] = {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};
  const uint16_t qi[] = {0, 1, 2, 0, 2, 3};
  MeshDesc q;
  q.layout.color0 = true;
  q.vertexCount = 4;
  q.positions = qp;
  q.colors0 = qc;
  q.indices = qi;
  q.indexCount = 6;
  q.label = "cuadrado";
  std::string err;
  s.quad = dev.create_mesh(q, &err);
  const float tp[] = {0, 0, 0, 200, 0, 0, 0, 200, 0};
  const uint8_t tc[] = {0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255};
  const uint16_t ti[] = {0, 1, 2};
  MeshDesc t;
  t.layout.color0 = true;
  t.vertexCount = 3;
  t.positions = tp;
  t.colors0 = tc;
  t.indices = ti;
  t.indexCount = 3;
  t.label = "triángulo";
  s.tri = dev.create_mesh(t, &err);
  s.mat = dev.register_material(passthrough_material(), &err);
  return s;
}

void draw_scene(Device& dev, const Scene& s, float triX, float triY) {
  const float blue[4] = {0, 0, 1, 1};
  dev.begin_frame();
  dev.begin_pass(W, H, true, true, blue, 0.f, true);
  dev.set_viewport(0, 0, W, H, 0, 1);
  dev.set_scissor(0, 0, W, H);
  DrawParams p{};
  ortho(p.projection);
  for (auto& m : p.posMatrices) m = translate(0, 0);
  dev.draw(s.quad, s.mat, kInvalid, p);
  p.posMatrices[0] = translate(triX, triY);
  dev.draw(s.tri, s.mat, kInvalid, p);
  dev.end_frame(true);
}

// La misma escena en el formato de captura de la ruta GX: vértices como los deja Aurora (posición f32 big endian y
// color RGBA8 directos), PipelineDesc con la disposición de uniformes de la prueba del motor de Choya.
void draw_reference(float triX, float triY) {
  std::vector<uint8_t> verts;
  auto be = [&](float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    for (int i = 3; i >= 0; --i) verts.push_back(static_cast<uint8_t>(u >> (8 * i)));
  };
  auto vtx = [&](float x, float y, uint8_t r, uint8_t g, uint8_t b) {
    be(x);
    be(y);
    be(0);
    verts.insert(verts.end(), {r, g, b, 255});
  };
  vtx(0, 0, 255, 0, 0);
  vtx(320, 0, 255, 0, 0);
  vtx(320, 480, 255, 0, 0);
  vtx(0, 480, 255, 0, 0);
  const uint32_t triStart = static_cast<uint32_t>(verts.size());
  vtx(0, 0, 0, 255, 0);
  vtx(200, 0, 0, 255, 0);
  vtx(0, 200, 0, 255, 0);
  const uint16_t idx[] = {0, 1, 2, 0, 2, 3, 0, 1, 2, 0};
  gxc::PipelineDesc d{};
  d.sampledChannels = 1;
  d.postexCount = 1;
  d.offProj = 20;
  d.offPostex = 36;
  d.offChanMat[0] = 48;
  d.offChanMatA[0] = 48;
  d.tevStageCount = 1;
  auto& st = d.stages[0];
  st.ca = st.cb = st.cc = GX_CC_ZERO;
  st.cd = GX_CC_RASC;
  st.cop = GX_TEV_ADD;
  st.cclamp = 1;
  st.cout = GX_TEVPREV;
  st.aa = st.ab = st.ac = GX_CA_ZERO;
  st.ad = GX_CA_RASA;
  st.aop = GX_TEV_ADD;
  st.aclamp = 1;
  st.aout = GX_TEVPREV;
  st.channel = GX_COLOR0A0;
  st.texCoord = st.texMap = 0xFF;
  st.pad = 0xFF;
  st.kcSel = GX_TEV_KCSEL_1;
  st.kaSel = GX_TEV_KASEL_1;
  for (int i = 0; i < 4; ++i) {
    for (int c = 0; c < 4; ++c) d.swap[i][c] = static_cast<uint8_t>(c);
  }
  d.alphaComp0 = d.alphaComp1 = GX_ALWAYS;
  for (auto& tg : d.texgens) tg = {GX_TG_MTX2x4, GX_MAX_TEXGENSRC, 0xFF, 0};
  d.depthFunc = GX_ALWAYS;
  d.cullMode = GX_CULL_NONE;
  d.blendMode = GX_BM_NONE;
  d.pixelFmt = GX_PF_RGB8_Z24;
  d.colorUpdate = d.alphaUpdate = 1;
  d.depthUpdate = 1;
  for (auto& a : d.attrs) a = {GX_NONE, 0, 0, 0, 0, 0, 0, 0};
  // Color del vértice en el canal 0 (color y alfa), sin iluminación.
  d.chans[0] = {GX_SRC_VTX, GX_SRC_REG, 0, 0x80};
  d.chans[2] = {GX_SRC_VTX, GX_SRC_REG, 0, 0x80};
  d.attrs[GX_VA_POS] = {GX_DIRECT, 3, GX_F32, 0, 0, 0, 0, 0};
  d.attrs[GX_VA_CLR0] = {GX_DIRECT, 4, GX_RGBA8, 12, 0, 0, 0, 0};
  d.vtxStride = 16;
  d.ext.valid = 1;
  d.ext.zTexStage = 0xFF;
  for (auto& p : d.ext.tgPost) p = 0xFF;
  for (auto& i : d.ext.ind) i = {0xFF, 0xFF, 0, 0};
  constexpr uint64_t kRef = 0x1234;
  if (!gxc::has_pipeline(kRef)) gxc::register_pipeline(kRef, d);
  std::vector<uint8_t> uni(512, 0);
  auto words = [&](size_t base, size_t w, const float* v, size_t n) { std::memcpy(&uni[base + w * 4], v, n * 4); };
  float proj[4][4];
  ortho(proj);
  const float white[4] = {1, 1, 1, 1};
  const float vp[2] = {W, H};
  for (int k = 0; k < 2; ++k) {
    const size_t base = k * 256;
    const uint32_t start = k == 0 ? 0 : triStart;
    std::memcpy(&uni[base], &start, 4);
    words(base, 2, vp, 2);
    words(base, 20, &proj[0][0], 16);
    const Mat34 m = k == 0 ? translate(0, 0) : translate(triX, triY);
    words(base, 36, &m.m[0][0], 12);
    words(base, 48, white, 4);
  }
  gxc::capture_begin(verts.data(), verts.size(), uni.data(), uni.size(), reinterpret_cast<const uint8_t*>(idx),
                     sizeof idx, nullptr, 0);
  const float blue[4] = {0, 0, 1, 1};
  gxc::capture_pass(W, H, true, true, blue, 0.f, true, gxc::Rect{0, 0, W, H});
  gxc::capture_viewport(gxc::Viewport{0, 0, W, H, 0, 1});
  gxc::capture_scissor(gxc::Rect{0, 0, W, H});
  gxc::capture_draw(kRef, 0, 6, 1, 0, UINT32_MAX, 0);
  gxc::capture_draw(kRef, 12, 3, 1, 256, UINT32_MAX, 0);
  gxc::capture_end(true);
}

double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
} // namespace

int main() {
  if (!make_context(W, H)) {
    std::puts("SIN GLES 3.2 (Mesa EGL sin superficie no disponible)");
    return 2;
  }
  Device dev;
  const Scene s = build_scene(dev);
  check(s.quad != kInvalid && s.tri != kInvalid && s.mat != kInvalid, "mallas y material registrados");

  // 1. Ruta directa.
  draw_scene(dev, s, 400, 100);
  check(gxc::draw_frame(W, H), "el motor dibujó el cuadro directo");
  const auto direct = read_screen(W, H);
  check(at(direct, W, H, 100, 240) == 0xFF0000, "cuadrado rojo (directo)");
  check(at(direct, W, H, 420, 120) == 0x00FF00, "triángulo verde trasladado (directo)");
  check(at(direct, W, H, 600, 450) == 0x0000FF, "fondo azul (directo)");

  // Referencia en formato de captura de la ruta GX.
  draw_reference(400, 100);
  check(gxc::draw_frame(W, H), "el motor dibujó el cuadro de referencia");
  const auto ref = read_screen(W, H);
  size_t diff = 0;
  for (size_t i = 0; i < ref.size(); i += 4) {
    if (std::memcmp(&ref[i], &direct[i], 3) != 0) ++diff;
  }
  std::printf("medida: píxeles distintos directo vs referencia GX: %zu de %d\n", diff, W * H);
  if (diff) {
    const int pts[][2] = {{100, 240}, {420, 120}, {600, 450}, {330, 10}, {10, 470}};
    for (auto& q : pts) {
      std::printf("      (%d,%d) directo %06x referencia %06x\n", q[0], q[1], at(direct, W, H, q[0], q[1]),
                  at(ref, W, H, q[0], q[1]));
    }
  }
  check(diff == 0, "directo y referencia GX dan la misma imagen");

  // 2. Persistencia: otro cuadro con el triángulo en otro lugar, sin reconstruir la geometría.
  const uint64_t rebuilds = dev.stats().arenaRebuilds;
  draw_scene(dev, s, 100, 300);
  gxc::draw_frame(W, H);
  const auto moved = read_screen(W, H);
  check(at(moved, W, H, 120, 320) == 0x00FF00, "el triángulo siguió a su matriz");
  check(at(moved, W, H, 420, 120) == 0x0000FF, "y dejó libre su lugar anterior");
  check(dev.stats().arenaRebuilds == rebuilds, "la geometría no se reconstruyó entre cuadros");

  // 3. Materiales fuera del alcance: rechazo explícito.
  MaterialDesc lit = passthrough_material();
  lit.channels[0].lighting = 1;
  std::string why;
  check(dev.register_material(lit, &why) == kInvalid && !why.empty(), "material con luz rechazado con motivo");
  std::printf("      motivo: %s\n", why.c_str());
  MeshDesc bad;
  check(dev.create_mesh(bad, &why) == kInvalid, "malla vacía rechazada");

  // 4. Coste de CPU por la ruta directa: N dibujos de una malla pequeña (sin contar la GPU).
  for (int n : {500, 2000}) {
    const int frames = 30;
    double prep = 0, gpu = 0;
    for (int f = 0; f < frames; ++f) {
      const auto t0 = std::chrono::steady_clock::now();
      const float blue[4] = {0, 0, 1, 1};
      dev.begin_frame();
      dev.begin_pass(W, H, true, true, blue, 0.f, true);
      dev.set_viewport(0, 0, W, H, 0, 1);
      dev.set_scissor(0, 0, W, H);
      DrawParams p{};
      ortho(p.projection);
      for (int i = 0; i < n; ++i) {
        p.posMatrices[0] = translate(static_cast<float>(i % 600), static_cast<float>((i / 600) * 20 % 440));
        dev.draw(s.tri, s.mat, kInvalid, p);
      }
      dev.end_frame(true);
      prep += ms_since(t0);
      const auto t1 = std::chrono::steady_clock::now();
      gxc::draw_frame(W, H);
      gpu += ms_since(t1);
    }
    std::printf("medida: ruta directa, %d dibujos: preparar y entregar %.3f ms/cuadro; draw_frame del motor %.3f ms/cuadro "
                "(PC x86-64 + llvmpipe, no es la Mali)\n",
                n, prep / frames, gpu / frames);
  }
  std::printf("medida: dibujos=%llu rechazados=%llu reconstrucciones_de_geometria=%llu\n",
              static_cast<unsigned long long>(dev.stats().draws),
              static_cast<unsigned long long>(dev.stats().rejectedDraws),
              static_cast<unsigned long long>(dev.stats().arenaRebuilds));
  if (gFails) {
    std::printf("PRUEBA DIRECTA: %d fallo(s)\n", gFails);
    return 1;
  }
  std::puts("PRUEBA DIRECTA OK");
  return 0;
}
