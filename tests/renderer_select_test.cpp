// galaxy-choya-native: pruebas del selector GALAXY_RENDERER.
#include "galaxy/renderer_select.hpp"

#include <cstdio>

using namespace galaxy;
static int fails = 0;
static void check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FALLO", what);
  if (!ok) ++fails;
}

int main() {
  std::string why;
  auto def = parse_renderer_config(nullptr, nullptr, nullptr);
  check(def.requested == Renderer::Gx && def.warning.empty(), "sin variable: gx");
  check(renderer_for_scene(def, "CocoonExGalaxy", &why) == Renderer::Gx, "gx en cualquier escena");

  auto bad = parse_renderer_config("vulkan", nullptr, nullptr);
  check(bad.requested == Renderer::Gx && !bad.warning.empty(), "valor desconocido: gx con aviso");

  auto direct = parse_renderer_config("DIRECT", "TestScene, OtraEscena", nullptr);
  check(direct.requested == Renderer::Direct, "direct reconocido sin distinguir mayúsculas");
  check(direct.extraScenes.size() == 2 && direct.extraScenes[1] == "OtraEscena", "lista de escenas separada por comas");
  check(renderer_for_scene(direct, "TestScene", &why) == Renderer::Direct, "escena de la lista: direct");
  check(renderer_for_scene(direct, "HeavensDoorGalaxy", &why) == Renderer::Gx, "escena sin validar: gx");
  std::printf("      motivo: %s\n", why.c_str());

  auto forced = parse_renderer_config("direct", nullptr, "1");
  check(renderer_for_scene(forced, "HeavensDoorGalaxy", &why) == Renderer::Direct, "forzado: direct en todas");
  auto notForced = parse_renderer_config("direct", nullptr, "0");
  check(!notForced.force, "GALAXY_DIRECT_FORCE=0 no fuerza");
  check(validated_direct_scenes().empty(), "ninguna escena validada sin medir en la consola");
  return fails ? 1 : 0;
}
