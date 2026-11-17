// galaxy-choya-native: selector de backend aplicado desde el juego (SceneControlInfo::setStage, parche 0008).
//
// La decisión es por escena completa y se registra (log.txt del port). Mientras la ruta directa no esté conectada a
// los modelos J3D de esta compilación (conversión y caché en la carga, docs/BLOQUEOS.md B11), una escena que el
// selector daría a "direct" sigue por GX y se dice por qué: nunca se pierde un dibujo ni se mezclan backends.
#include "galaxy/renderer_select.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

namespace {
std::mutex gMutex;
std::string gLastStage;
std::atomic<int> gEffective{0};  // 0 gx, 1 direct
constexpr bool kDirectConnected = false;  // la ruta directa todavía no recibe formas de J3D en el ejecutable
}

extern "C" void galaxy_scene_renderer(const char* stageName) {
  if (stageName == nullptr) return;
  static const galaxy::RendererConfig cfg = [] {
    auto c = galaxy::renderer_config_from_env();
    if (!c.warning.empty()) std::fprintf(stderr, "[galaxy-backend] %s\n", c.warning.c_str());
    return c;
  }();
  std::lock_guard lock(gMutex);
  if (gLastStage == stageName) return;
  gLastStage = stageName;
  std::string reason;
  galaxy::Renderer r = galaxy::renderer_for_scene(cfg, gLastStage, &reason);
  if (r == galaxy::Renderer::Direct && !kDirectConnected) {
    reason += "; la ruta directa no está conectada a J3D en este ejecutable: la escena entera sigue por gx";
    r = galaxy::Renderer::Gx;
  }
  gEffective.store(r == galaxy::Renderer::Direct ? 1 : 0);
  std::fprintf(stderr, "[galaxy-backend] escena %s: %s (%s)\n", gLastStage.c_str(), galaxy::renderer_name(r),
               reason.c_str());
}

extern "C" int galaxy_scene_renderer_effective() { return gEffective.load(); }
