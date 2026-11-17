// galaxy-choya-native: puente entre la Aurora única (encounter/aurora 08122911 + parches de MacioGalaxy) y el motor
// GLES de Choya (choya_gx_core). Sustituye a choya/data/aurora/choya_gx.cpp, que se escribió para aurora-main de
// BrawlRecompiled y no encaja en esta Aurora (otro cuadro, otros uniformes, otras funciones auxiliares).
//
// Qué hace:
//   - Al cerrar cada cuadro (gfx::end_frame), recorre el FramePacket que Aurora ya armó: pases, viewport/scissor,
//     dibujos GX, borrados parciales, copias del EFB, paletas sobre copias. Lo entrega al núcleo con capture_*.
//   - Registra pipelines (gx::PipelineConfig → choya::gx::PipelineDesc), borrados y grupos de texturas en el momento
//     en que Aurora los crea, y envía al núcleo los datos RGBA8 de cada textura cuando Aurora los convierte.
//   - Reempaqueta por dibujo el bloque de uniformes de Aurora al formato del núcleo: cabecera (vtx_start, matriz
//     actual, tamaño del viewport, comienzos de arreglos) + el bloque de Aurora tal cual + niebla y matrices
//     indirectas convertidas. Los vértices, índices y arreglos indexados se pasan sin tocar (big endian de GX).
//
// Lo que no traduce lo cuenta y lo registra (log una vez por motivo); no se omite nada en silencio.
// Solo se activa con CHOYA_GLES=1 (como el adaptador original de Choya).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace aurora::gfx {
struct TextureRef;
namespace detail {
struct FramePacket;
struct DrawCommand;
} // namespace detail
namespace clear {
struct PipelineConfig;
}
} // namespace aurora::gfx
namespace aurora::gx {
struct PipelineConfig;
struct ShaderInfo;
} // namespace aurora::gx

namespace galaxy_choya {

bool enabled() noexcept;

// Ganchos insertados en Aurora por choya_bridge/patch_aurora_bridge.py.
void register_gx_pipeline(uint64_t ref, const aurora::gx::PipelineConfig& config) noexcept;
void register_clear_pipeline(uint64_t ref, const aurora::gfx::clear::PipelineConfig& config) noexcept;
void register_bind_group(uint64_t ref, const aurora::gx::ShaderInfo& info) noexcept;
void texture_data(const aurora::gfx::TextureRef& ref, const uint8_t* data, size_t size) noexcept;
std::shared_ptr<aurora::gfx::TextureRef> tracked(std::shared_ptr<aurora::gfx::TextureRef> handle) noexcept;
void note_display_dest(const void* dest) noexcept;
void note_copy(const void* dest, const aurora::gfx::TextureRef* texture) noexcept;
void capture_packet(aurora::gfx::detail::FramePacket& packet) noexcept;

// Contadores desde el inicio (pruebas y log.txt).
struct Stats {
  uint64_t frames = 0, passes = 0, gxDraws = 0, clears = 0, copies = 0, displayCopies = 0, paletteConvs = 0;
  uint64_t generatedIndexDraws = 0, uniformBytes = 0;
  uint64_t skippedUnknownPipeline = 0, skippedUniformSize = 0, skippedCustomDraws = 0, skippedTextureCopies = 0;
  uint64_t skippedEncoderTasks = 0, lineDraws = 0, approxFogRange = 0, approxBump = 0, approxPolygonOffset = 0;
  uint64_t texturesSent = 0, texturesExpanded = 0, texturesUnsupported = 0, texturesReleased = 0;
};
Stats stats() noexcept;

} // namespace galaxy_choya

// Definida en recording.cpp (parcheado): qué es un dibujo del cuadro. 1 = GX (payload gx::DrawData),
// 2 = borrado (payload clear::DrawData), 0 = otro (ImGui, RmlUi, tipos registrados).
namespace aurora::gfx {
int galaxy_choya_draw_kind(const detail::DrawCommand& command, const void** payload) noexcept;
}

// Para la pantalla que presente con GLES (contexto ya activo en el hilo que llama).
extern "C" bool galaxy_choya_draw_frame(int screenWidth, int screenHeight);
