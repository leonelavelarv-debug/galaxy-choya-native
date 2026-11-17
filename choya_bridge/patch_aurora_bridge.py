#!/usr/bin/env python3
"""Inserta los ganchos del puente Choya en una fuente de Aurora única (ya parcheada por MacioGalaxy).

  patch_aurora_bridge.py <tipo> <entrada> <salida>

Tipos: frame, recording, command_processor, texture, framebuffer. Cada inserción es obligatoria: si su texto de
anclaje no aparece exactamente una vez, el script falla y la configuración se detiene (no se compila un puente a
medias). Idempotente: si la salida ya lleva el gancho, se rehace desde la entrada igual.
"""
import sys

INCLUDE = '#include "galaxy_choya_bridge.hpp"  // galaxy-choya-native: puente hacia el motor GLES de Choya\n'


def replace_once(text: str, old: str, new: str, what: str) -> str:
    n = text.count(old)
    if n != 1:
        raise SystemExit(f"patch_aurora_bridge: anclaje '{what}' aparece {n} veces (se esperaba 1)")
    return text.replace(old, new, 1)


def add_include(text: str) -> str:
    # Después del primer #include (el propio .hpp del archivo).
    i = text.index("#include")
    j = text.index("\n", i) + 1
    return text[:j] + INCLUDE + text[j:]


def frame(t: str) -> str:
    return replace_once(t, "  auto& frame = *recorded.packet;\n",
                        "  auto& frame = *recorded.packet;\n  ::galaxy_choya::capture_packet(frame);\n",
                        "end_frame: recorded.packet")


def recording(t: str) -> str:
    t = replace_once(
        t,
        "PipelineRef pipeline_ref(const gx::PipelineConfig& config) {\n"
        "  return find_pipeline(config, get_render_target_layout());\n}",
        "PipelineRef pipeline_ref(const gx::PipelineConfig& config) {\n"
        "  const PipelineRef ref = find_pipeline(config, get_render_target_layout());\n"
        "  ::galaxy_choya::register_gx_pipeline(ref, config);\n  return ref;\n}",
        "pipeline_ref(gx)")
    t = replace_once(
        t,
        "PipelineRef pipeline_ref(const clear::PipelineConfig& config) {\n"
        "  return find_pipeline(config, get_render_target_layout());\n}",
        "PipelineRef pipeline_ref(const clear::PipelineConfig& config) {\n"
        "  const PipelineRef ref = find_pipeline(config, get_render_target_layout());\n"
        "  ::galaxy_choya::register_clear_pipeline(ref, config);\n  return ref;\n}",
        "pipeline_ref(clear)")
    # Clasificador de dibujos: necesita encode_draw<>, que está en el espacio anónimo de este archivo.
    tail = "} // namespace aurora::gfx\n"
    if not t.rstrip().endswith(tail.strip()):
        raise SystemExit("patch_aurora_bridge: recording.cpp no termina en '} // namespace aurora::gfx'")
    k = t.rindex(tail)
    t = t[:k] + (
        "// galaxy-choya-native: qué es un dibujo del cuadro (para el puente hacia Choya).\n"
        "int galaxy_choya_draw_kind(const detail::DrawCommand& command, const void** payload) noexcept {\n"
        "  if (command.encoder == encode_draw<gx::render, gx::DrawData>) {\n"
        "    *payload = &inline_payload<gx::DrawData>(const_cast<std::byte*>(command.payload.data()));\n"
        "    return 1;\n  }\n"
        "  if (command.encoder == encode_draw<clear::render, clear::DrawData>) {\n"
        "    *payload = &inline_payload<clear::DrawData>(const_cast<std::byte*>(command.payload.data()));\n"
        "    return 2;\n  }\n"
        "  *payload = nullptr;\n  return 0;\n}\n") + t[k:]
    return t


def command_processor(t: str) -> str:
    return replace_once(
        t, "    cache.bindGroups = build_bind_groups(cache.shaderInfo);\n",
        "    cache.bindGroups = build_bind_groups(cache.shaderInfo);\n"
        "    ::galaxy_choya::register_bind_group(cache.bindGroups.textureBindGroup, cache.shaderInfo);\n",
        "build_bind_groups")


def texture(t: str) -> str:
    hook = "  ::galaxy_choya::texture_data(ref, data.data(), data.size());\n"
    t = replace_once(t, "  uint32_t offset = 0;\n  for (uint32_t mip = 0; mip < mips; ++mip) {\n",
                     hook + "  uint32_t offset = 0;\n  for (uint32_t mip = 0; mip < mips; ++mip) {\n",
                     "new_static_texture_2d: subida")
    t = replace_once(t, "  uint32_t offset = 0;\n  for (uint32_t mip = 0; mip < ref.mipCount; ++mip) {\n",
                     hook + "  uint32_t offset = 0;\n  for (uint32_t mip = 0; mip < ref.mipCount; ++mip) {\n",
                     "write_texture: subida")
    # Cada TextureRef nuevo se sigue (para avisar al núcleo cuando se libera).
    marker = "return std::make_shared<TextureRef>("
    n = t.count(marker)
    if n < 3:
        raise SystemExit(f"patch_aurora_bridge: se esperaban al menos 3 make_shared<TextureRef>, hay {n}")
    out, pos = [], 0
    while True:
        i = t.find(marker, pos)
        if i < 0:
            out.append(t[pos:])
            break
        end = t.index(");", i)
        out.append(t[pos:i] + "return ::galaxy_choya::tracked(std::make_shared<TextureRef>(" +
                   t[i + len(marker):end] + "));")
        pos = end + 2
    return "".join(out)


def framebuffer(t: str) -> str:
    t = replace_once(t, "  g_gxState.copyTextures[dest] = handle;\n",
                     "  g_gxState.copyTextures[dest] = handle;\n  ::galaxy_choya::note_copy(dest, handle.handle.get());\n",
                     "copy_tex: copyTextures[dest]")
    t = replace_once(t, "void GXCopyDisp(void* dest, GXBool clear) {\n",
                     "void GXCopyDisp(void* dest, GXBool clear) {\n  ::galaxy_choya::note_display_dest(dest);\n",
                     "GXCopyDisp")
    return t


KINDS = {"frame": frame, "recording": recording, "command_processor": command_processor, "texture": texture,
         "framebuffer": framebuffer}


def main() -> int:
    kind, src, dst = sys.argv[1:4]
    with open(src, encoding="utf-8") as f:
        text = f.read()
    if "galaxy_choya_bridge.hpp" in text:
        raise SystemExit(f"patch_aurora_bridge: {src} ya trae el puente (la entrada debe ser la fuente sin él)")
    text = add_include(KINDS[kind](text))
    old = None
    try:
        with open(dst, encoding="utf-8") as f:
            old = f.read()
    except FileNotFoundError:
        pass
    if old != text:  # sin reescribir si no cambió: no fuerza recompilar
        import os
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "w", encoding="utf-8") as f:
            f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
