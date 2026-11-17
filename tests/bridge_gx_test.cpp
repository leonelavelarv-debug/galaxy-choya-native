// galaxy-choya-native: prueba de extremo a extremo del puente Aurora única → motor GLES de Choya.
//
// Llamadas GX reales (las mismas que hace J3D: GXSetVtxDesc, GXBegin/GXPosition..., texturas, TEV, GXCopyDisp) →
// FIFO → Aurora única con todos los parches de MacioGalaxy (backend WebGPU nulo de Dawn: Aurora no dibuja nada) →
// galaxy_choya::capture_packet → choya_gx_core dibujando con Mesa (EGL sin superficie) → píxeles leídos.
//
// Comprueba: geometría directa (cuadrado → índices de Aurora), GX_TRIANGLES sin índices (índices generados por el
// puente), textura RGB565 decodificada por Aurora y enviada por el gancho, TEV de reemplazo, color de vértice, borrado
// del EFB, copia a pantalla (GXCopyDisp) y que ningún dibujo quede omitido.
// Salida: "PRUEBA PUENTE OK". Código 2 si no hay GLES 3.2 o Aurora no arranca con el backend nulo.
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <dolphin/gx.h>

#include "galaxy_choya_bridge.hpp"
#include "choya_gx_core.hpp"

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
void* gEgl = nullptr;
void* gGles = nullptr;
void* (*gEglProc)(const char*) = nullptr;
int gFails = 0;

void* gl_proc(const char* name) {
  void* p = dlsym(gGles, name);
  return p ? p : (gEglProc ? gEglProc(name) : nullptr);
}
void core_log(const char* line) {
  if (std::getenv("GALAXY_TEST_VERBOSE")) std::printf("[motor] %s\n", line);
}

bool make_context(int w, int h) {
  gEgl = dlopen("libEGL.so.1", RTLD_NOW);
  gGles = dlopen("libGLESv2.so.2", RTLD_NOW);
  if (!gEgl || !gGles) return false;
  auto sym = [](const char* n) { return dlsym(gEgl, n); };
  gEglProc = reinterpret_cast<void* (*)(const char*)>(sym("eglGetProcAddress"));
  auto getDisplay = reinterpret_cast<void* (*)(void*)>(sym("eglGetDisplay"));
  auto initialize = reinterpret_cast<unsigned (*)(void*, int*, int*)>(sym("eglInitialize"));
  auto bindApi = reinterpret_cast<unsigned (*)(unsigned)>(sym("eglBindAPI"));
  auto chooseConfig = reinterpret_cast<unsigned (*)(void*, const int*, void**, int, int*)>(sym("eglChooseConfig"));
  auto createPbuffer = reinterpret_cast<void* (*)(void*, void*, const int*)>(sym("eglCreatePbufferSurface"));
  auto createContext = reinterpret_cast<void* (*)(void*, void*, void*, const int*)>(sym("eglCreateContext"));
  auto makeCurrent = reinterpret_cast<unsigned (*)(void*, void*, void*, void*)>(sym("eglMakeCurrent"));
  void* dpy = getDisplay(nullptr);
  int major = 0, minor = 0;
  if (!dpy || !initialize(dpy, &major, &minor)) return false;
  bindApi(0x30A0);
  const int cfgAttrs[] = {0x3033, 0x0001, 0x3040, 0x0040, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3025, 24,
                          0x3038};
  void* cfg = nullptr;
  int n = 0;
  if (!chooseConfig(dpy, cfgAttrs, &cfg, 1, &n) || n == 0) return false;
  const int pb[] = {0x3057, w, 0x3056, h, 0x3038};
  void* surf = createPbuffer(dpy, cfg, pb);
  const int ctxAttrs[] = {0x3098, 3, 0x30FB, 2, 0x3038};
  void* ctx = createContext(dpy, cfg, nullptr, ctxAttrs);
  if (!surf || !ctx || !makeCurrent(dpy, surf, surf, ctx)) return false;
  choya::gx::set_gl_loader(gl_proc);
  choya::gx::set_logger(core_log);
  return true;
}

uint32_t pixel(int x, int y, int h) {  // y = 0 arriba
  static auto readPixels = reinterpret_cast<void (*)(int, int, int, int, unsigned, unsigned, void*)>(
      dlsym(gGles, "glReadPixels"));
  unsigned char p[4];
  readPixels(x, h - 1 - y, 1, 1, 0x1908, 0x1401, p);
  return (uint32_t{p[0]} << 16) | (uint32_t{p[1]} << 8) | p[2];
}

void check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FALLO", what);
  if (!ok) ++gFails;
}

void quiet(AuroraLogLevel level, const char* module, const char* message, unsigned int length) {
  if (level >= LOG_ERROR || std::getenv("GALAXY_TEST_VERBOSE")) {
    std::fprintf(stderr, "[aurora %s] %.*s\n", module, static_cast<int>(length), message);
  }
  if (level == LOG_FATAL) std::abort();
}

constexpr int W = 640, H = 480;

void setup_common() {
  GXSetViewport(0, 0, W, H, 0, 1);
  GXSetScissor(0, 0, W, H);
  f32 proj[4][4] = {};
  proj[0][0] = 2.f / W;
  proj[0][3] = -1.f;
  proj[1][1] = -2.f / H;
  proj[1][3] = 1.f;
  proj[2][2] = -1.f;
  proj[2][3] = -1.f;
  proj[3][3] = 1.f;
  GXSetProjection(proj, GX_ORTHOGRAPHIC);
  f32 ident[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
  GXLoadPosMtxImm(ident, GX_PNMTX0);
  GXSetCurrentMtx(GX_PNMTX0);
  GXSetNumChans(1);
  GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
  GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
  GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
  GXSetCullMode(GX_CULL_NONE);
  GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
  GXSetColorUpdate(GX_TRUE);
  GXSetAlphaUpdate(GX_TRUE);
}

// Textura 8x8 RGB565 amarilla (big endian como en el disco; uniforme, así que el mosaico 4x4 no importa).
alignas(32) uint8_t gTexture[8 * 8 * 2];
GXTexObj gTexObj;

void draw_scene(float triX) {
  setup_common();
  // 1) Cuadrado rojo (GX_QUADS: Aurora genera índices) con color de vértice.
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
  GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
  GXSetNumTexGens(0);
  GXSetNumTevStages(1);
  GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
  GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
  GXBegin(GX_QUADS, GX_VTXFMT0, 4);
  GXPosition3f32(0, 0, -0.5f);   GXColor4u8(255, 0, 0, 255);
  GXPosition3f32(320, 0, -0.5f); GXColor4u8(255, 0, 0, 255);
  GXPosition3f32(320, 240, -0.5f); GXColor4u8(255, 0, 0, 255);
  GXPosition3f32(0, 240, -0.5f); GXColor4u8(255, 0, 0, 255);
  GXEnd();
  // 2) Triángulo verde (GX_TRIANGLES sin fusionar: Aurora lo dibuja sin índices; el puente los genera). Otro formato
  // de vértice (mismos atributos) para que Aurora no lo fusione con el cuadrado.
  GXSetVtxAttrFmt(GX_VTXFMT2, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
  GXSetVtxAttrFmt(GX_VTXFMT2, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
  GXBegin(GX_TRIANGLES, GX_VTXFMT2, 3);
  GXPosition3f32(triX, 0, -0.5f);       GXColor4u8(0, 255, 0, 255);
  GXPosition3f32(triX + 200, 0, -0.5f); GXColor4u8(0, 255, 0, 255);
  GXPosition3f32(triX, 200, -0.5f);     GXColor4u8(0, 255, 0, 255);
  GXEnd();
  // 3) Cuadrado con textura (TEV GX_REPLACE: solo la textura).
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
  GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
  GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
  GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
  GXSetNumTexGens(1);
  GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
  GXLoadTexObj(&gTexObj, GX_TEXMAP0);
  GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
  GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
  GXBegin(GX_QUADS, GX_VTXFMT1, 4);
  GXPosition3f32(0, 300, -0.5f);   GXTexCoord2f32(0, 0);
  GXPosition3f32(200, 300, -0.5f); GXTexCoord2f32(1, 0);
  GXPosition3f32(200, 460, -0.5f); GXTexCoord2f32(1, 1);
  GXPosition3f32(0, 460, -0.5f);   GXTexCoord2f32(0, 1);
  GXEnd();
}

alignas(32) uint8_t gXfb[W * H * 2];
alignas(32) uint8_t gCopy[64 * 64 * 4];  // destino de GXCopyTex (RGBA8)
GXTexObj gCopyObj;

// 4) Canal con iluminación: sin luces (máscara 0), ambiente verde del registro y material blanco del vértice →
// verde (GX: material × (ambiente + luces)). Pone a prueba el bloque de luces y el orden de los colores de canal.
void draw_lit() {
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
  GXSetVtxDesc(GX_VA_NRM, GX_DIRECT);
  GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
  GXSetVtxAttrFmt(GX_VTXFMT3, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
  GXSetVtxAttrFmt(GX_VTXFMT3, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
  GXSetVtxAttrFmt(GX_VTXFMT3, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
  f32 ident[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
  GXLoadNrmMtxImm(ident, GX_PNMTX0);
  GXSetNumChans(1);
  GXSetChanAmbColor(GX_COLOR0A0, GXColor{0, 255, 0, 255});
  GXSetChanCtrl(GX_COLOR0A0, GX_ENABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_CLAMP, GX_AF_NONE);
  GXSetNumTexGens(0);
  GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
  GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
  GXBegin(GX_QUADS, GX_VTXFMT3, 4);
  GXPosition3f32(400, 300, -0.5f); GXNormal3f32(0, 0, 1); GXColor4u8(255, 255, 255, 255);
  GXPosition3f32(500, 300, -0.5f); GXNormal3f32(0, 0, 1); GXColor4u8(255, 255, 255, 255);
  GXPosition3f32(500, 400, -0.5f); GXNormal3f32(0, 0, 1); GXColor4u8(255, 255, 255, 255);
  GXPosition3f32(400, 400, -0.5f); GXNormal3f32(0, 0, 1); GXColor4u8(255, 255, 255, 255);
  GXEnd();
  GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
}

// 5) Copia del EFB a textura (GXCopyTex, sin borrar) de una zona del cuadrado rojo, y un cuadrado que la muestrea en
// el mismo cuadro: la copia la hace el núcleo al terminar el pase y el dibujo siguiente la lee.
void draw_copy() {
  GXSetTexCopySrc(0, 0, 64, 64);
  GXSetTexCopyDst(64, 64, GX_TF_RGBA8, GX_FALSE);
  GXCopyTex(gCopy, GX_FALSE);
  GXInitTexObj(&gCopyObj, gCopy, 64, 64, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
  GXInitTexObjLOD(&gCopyObj, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
  GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
  GXSetNumTexGens(1);
  GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
  GXLoadTexObj(&gCopyObj, GX_TEXMAP1);
  GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP1, GX_COLOR0A0);
  GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
  GXBegin(GX_QUADS, GX_VTXFMT1, 4);
  GXPosition3f32(540, 20, -0.5f);  GXTexCoord2f32(0, 0);
  GXPosition3f32(620, 20, -0.5f);  GXTexCoord2f32(1, 0);
  GXPosition3f32(620, 100, -0.5f); GXTexCoord2f32(1, 1);
  GXPosition3f32(540, 100, -0.5f); GXTexCoord2f32(0, 1);
  GXEnd();
}
} // namespace

int main(int, char** argv) {
  setenv("CHOYA_GLES", "1", 1);
  setenv("SDL_VIDEO_DRIVER", "offscreen", 0);
  for (size_t i = 0; i < sizeof gTexture; i += 2) {
    gTexture[i] = 0xFF;  // RGB565 0xFFE0 = amarillo
    gTexture[i + 1] = 0xE0;
  }
  AuroraConfig config{};
  config.appName = "galaxy-choya-native bridge test";
  config.userPath = "/tmp/galaxy-bridge-test";
  config.cachePath = "/tmp/galaxy-bridge-test";
  config.desiredBackend = BACKEND_NULL;
  config.windowWidth = W;
  config.windowHeight = H;
  config.logCallback = quiet;
  config.logLevel = LOG_WARNING;
  const AuroraInfo info = aurora_initialize(1, argv, &config);
  if (!info.window) {
    std::puts("SIN AURORA (backend nulo no disponible)");
    return 2;
  }
  if (!make_context(W, H)) {
    std::puts("SIN GLES 3.2 (Mesa EGL sin superficie no disponible)");
    aurora_shutdown();
    return 2;
  }
  // Como el juego: GXInit con su FIFO antes de cualquier estado GX.
  static void* fifo = std::aligned_alloc(32, 0x40000);
  if (!fifo || !GXInit(fifo, 0x40000)) {
    std::puts("GXInit falló");
    return 1;
  }
  GXInitTexObj(&gTexObj, gTexture, 8, 8, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
  GXInitTexObjLOD(&gTexObj, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);

  // Cuatro cuadros: los dos primeros calientan los pipelines (MacioGalaxy puede diferir un dibujo hasta que su
  // pipeline compila); se comprueban los dos últimos.
  const float positions[4] = {400.f, 400.f, 400.f, 100.f};
  uint32_t samples[4][4] = {};
  uint32_t litSample = 0, copySample = 0;
  for (int frame = 0; frame < 4; ++frame) {
    for (const AuroraEvent* ev = aurora_update(); ev && ev->type != AURORA_NONE; ++ev) {
    }
    if (!aurora_begin_frame()) {
      std::puts("aurora_begin_frame devolvió false");
      ++gFails;
      continue;
    }
    GXSetCopyClear(GXColor{0, 0, 255, 255}, 0x00FFFFFF);
    draw_scene(positions[frame]);
    draw_lit();
    draw_copy();
    GXSetDispCopySrc(0, 0, W, H);
    GXSetDispCopyDst(W, H);
    GXCopyDisp(gXfb, GX_TRUE);
    GXDrawDone();
    aurora_end_frame();
    if (frame == 3 && std::getenv("GALAXY_TEST_VERBOSE")) {
      choya::gx::describe_frame([](const char* line) { std::printf("[cuadro] %s\n", line); });
    }
    const bool drawn = galaxy_choya_draw_frame(W, H);
    check(drawn, "el motor dibujó el cuadro capturado");
    samples[frame][0] = pixel(10, 230, H);  // fuera de las dos posiciones del triángulo
    samples[frame][1] = pixel(static_cast<int>(positions[frame]) + 20, 20, H);
    samples[frame][2] = pixel(100, 380, H);
    samples[frame][3] = pixel(600, 450, H);
    litSample = pixel(450, 350, H);
    copySample = pixel(580, 60, H);
    std::printf("      cuadro %d: rojo %06x verde %06x textura %06x fondo %06x iluminado %06x copia %06x\n", frame,
                samples[frame][0], samples[frame][1], samples[frame][2], samples[frame][3], litSample, copySample);
  }
  check(samples[2][0] == 0xFF0000, "cuadrado rojo (GX_QUADS, color de vértice)");
  check(samples[2][1] == 0x00FF00, "triángulo verde (GX_TRIANGLES sin índices: índices del puente)");
  check(samples[2][2] == 0xFFFF00, "cuadrado con textura RGB565 (decodificada por Aurora, enviada por el gancho)");
  check(samples[2][3] == 0x0000FF, "fondo del borrado del EFB (GXSetCopyClear)");
  check(samples[3][1] == 0x00FF00 && samples[3][0] == 0xFF0000 && pixel(420, 20, H) != 0x00FF00,
        "último cuadro: el triángulo siguió a sus nuevos vértices");
  check(litSample == 0x00FF00, "canal iluminado: material del vértice × ambiente del registro (bloque de luces)");
  check(copySample == 0xFF0000, "copia del EFB a textura reutilizada en el mismo cuadro (GXCopyTex)");
  const auto st = galaxy_choya::stats();
  std::printf("medida: cuadros=%llu pases=%llu dibujos_gx=%llu indices_generados=%llu copias_a_pantalla=%llu "
              "texturas=%llu omitidos(pipeline=%llu uniformes=%llu otros=%llu) bytes_uniformes=%llu\n",
              (unsigned long long)st.frames, (unsigned long long)st.passes, (unsigned long long)st.gxDraws,
              (unsigned long long)st.generatedIndexDraws, (unsigned long long)st.displayCopies,
              (unsigned long long)(st.texturesSent + st.texturesExpanded), (unsigned long long)st.skippedUnknownPipeline,
              (unsigned long long)st.skippedUniformSize, (unsigned long long)st.skippedCustomDraws,
              (unsigned long long)st.uniformBytes);
  check(st.frames == 4, "cuatro cuadros capturados");
  check(st.gxDraws >= 6, "todos los dibujos GX llegaron al núcleo");
  check(st.generatedIndexDraws >= 1, "el puente generó índices para el dibujo sin índices");
  check(st.displayCopies == 4, "GXCopyDisp reconocida como copia a pantalla en cada cuadro");
  check(st.skippedUnknownPipeline == 0 && st.skippedUniformSize == 0, "ningún dibujo GX omitido");
  check(st.texturesSent + st.texturesExpanded >= 1, "la textura llegó al núcleo");
  check(st.copies >= 4, "una copia del EFB a textura por cuadro");
  aurora_shutdown();
  if (gFails) {
    std::printf("PRUEBA PUENTE: %d fallo(s)\n", gFails);
    return 1;
  }
  std::puts("PRUEBA PUENTE OK");
  return 0;
}
