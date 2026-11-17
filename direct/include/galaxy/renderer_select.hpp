// galaxy-choya-native: selección del backend gráfico (interfaz nueva de este proyecto).
//
//   GALAXY_RENDERER=gx       (por defecto) Petari → J3D/GX → Aurora → adaptador Choya → GLES
//   GALAXY_RENDERER=direct   Petari → J3D adaptado → API directa de Choya → GLES, SOLO en las escenas de la lista
//                            validada; el resto sigue por gx en esa misma ejecución
//   GALAXY_DIRECT_SCENES=a,b escenas (nombre de etapa de Galaxy) habilitadas para direct además de la lista validada
//   GALAXY_DIRECT_FORCE=1    direct en todas las escenas (para medir; puede faltar algo: se registra qué)
//
// La elección se hace por escena, al cambiar de escena, nunca a mitad de un cuadro: así ambos backends nunca
// comparten framebuffer ni estado GL dentro del mismo cuadro (docs/ARQUITECTURA.md, "Convivencia").
#pragma once

#include <string>
#include <vector>

namespace galaxy {

enum class Renderer { Gx, Direct };

struct RendererConfig {
  Renderer requested = Renderer::Gx;
  bool force = false;
  std::vector<std::string> extraScenes;
  std::string warning;  // valor no reconocido, etc.
};

RendererConfig parse_renderer_config(const char* renderer, const char* scenes, const char* force);
RendererConfig renderer_config_from_env();

// Escenas en las que la ruta directa está validada (vacía hasta medir en la consola: docs/MEDICIONES.md).
const std::vector<std::string>& validated_direct_scenes();

// Backend para una escena. reason explica la decisión (va al log.txt del port).
Renderer renderer_for_scene(const RendererConfig& cfg, const std::string& scene, std::string* reason);

const char* renderer_name(Renderer r);

} // namespace galaxy
