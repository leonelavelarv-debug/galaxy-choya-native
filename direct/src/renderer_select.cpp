// galaxy-choya-native: selección de backend (ver renderer_select.hpp).
#include "galaxy/renderer_select.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace galaxy {
namespace {
std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}
std::vector<std::string> split(const char* s) {
  std::vector<std::string> out;
  if (!s) return out;
  std::string cur;
  for (const char* p = s;; ++p) {
    if (*p == ',' || *p == '\0') {
      while (!cur.empty() && cur.back() == ' ') cur.pop_back();
      size_t b = cur.find_first_not_of(' ');
      if (b != std::string::npos) out.push_back(cur.substr(b));
      cur.clear();
      if (*p == '\0') break;
    } else {
      cur += *p;
    }
  }
  return out;
}
} // namespace

RendererConfig parse_renderer_config(const char* renderer, const char* scenes, const char* force) {
  RendererConfig c;
  if (renderer && *renderer) {
    const std::string r = lower(renderer);
    if (r == "direct" || r == "directo") {
      c.requested = Renderer::Direct;
    } else if (r != "gx") {
      c.warning = "GALAXY_RENDERER='" + std::string(renderer) + "' no reconocido; se usa gx";
    }
  }
  c.extraScenes = split(scenes);
  c.force = force && *force && std::strcmp(force, "0") != 0;
  return c;
}

RendererConfig renderer_config_from_env() {
  return parse_renderer_config(std::getenv("GALAXY_RENDERER"), std::getenv("GALAXY_DIRECT_SCENES"),
                               std::getenv("GALAXY_DIRECT_FORCE"));
}

const std::vector<std::string>& validated_direct_scenes() {
  // Vacía a propósito: ninguna escena se ha validado todavía en la RG35XX H (docs/MEDICIONES.md). Se añade una
  // escena aquí solo después de comparar imagen y tiempos de ambos backends en la consola.
  static const std::vector<std::string> scenes;
  return scenes;
}

Renderer renderer_for_scene(const RendererConfig& cfg, const std::string& scene, std::string* reason) {
  auto why = [&](std::string s) {
    if (reason) *reason = std::move(s);
  };
  if (cfg.requested == Renderer::Gx) {
    why("gx: backend por defecto");
    return Renderer::Gx;
  }
  if (cfg.force) {
    why("direct: forzado con GALAXY_DIRECT_FORCE (escena '" + scene + "' sin validar)");
    return Renderer::Direct;
  }
  const auto& v = validated_direct_scenes();
  if (std::find(v.begin(), v.end(), scene) != v.end()) {
    why("direct: escena validada");
    return Renderer::Direct;
  }
  if (std::find(cfg.extraScenes.begin(), cfg.extraScenes.end(), scene) != cfg.extraScenes.end()) {
    why("direct: escena en GALAXY_DIRECT_SCENES (sin validar en consola)");
    return Renderer::Direct;
  }
  why("gx: la escena '" + scene + "' no está validada para direct");
  return Renderer::Gx;
}

const char* renderer_name(Renderer r) { return r == Renderer::Direct ? "direct" : "gx"; }

} // namespace galaxy
