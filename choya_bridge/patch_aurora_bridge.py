#!/usr/bin/env python3
"""Inserta los ganchos del puente Choya en una fuente de Aurora única (ya parcheada por MacioGalaxy).

  patch_aurora_bridge.py <tipo> <entrada> <salida>

Tipos: frame, recording, command_processor, texture, framebuffer (aurora_gx); gpu, aurora, window (aurora_core). Cada inserción es obligatoria: si su texto de
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
    t = replace_once(t, "  auto& frame = *recorded.packet;\n",
                     "  auto& frame = *recorded.packet;\n  ::galaxy_choya::capture_packet(frame);\n",
                     "end_frame: recorded.packet")
    # Con Choya, las lecturas del EFB las hace el núcleo (capture_segment): el backend nulo de WebGPU no se lee.
    return replace_once(
        t,
        "      auto mapReadback = PetariNative::GX::encodeSnapshot(frame.encoder, source, ticket, ready);\n",
        "      auto mapReadback = ::galaxy_choya::enabled()\n"
        "                             ? AfterSubmitCallback{[] {}}\n"
        "                             : PetariNative::GX::encodeSnapshot(frame.encoder, source, ticket, ready);\n",
        "petari_submit_segment: encodeSnapshot")


PRESENTS = ('extern "C" bool galaxy_choya_presents() __attribute__((weak));  // galaxy-choya-native\n'
            'static bool galaxy_choya_presents_now() { return galaxy_choya_presents != nullptr && galaxy_choya_presents(); }\n')


def gpu(t: str) -> str:
    # Choya presenta: sin superficie WebGPU (el backend nulo no muestra nada y en la consola no hay X11/Wayland).
    t = replace_once(t, "static bool create_surface() {\n",
                     "static bool create_surface() {\n"
                     "  if (galaxy_choya_presents_now()) {\n"
                     "    window::SurfaceLock surfaceLock;\n"
                     "    release_surface_locked();\n"
                     "    Log.info(\"galaxy-choya-native: Choya presenta; sin superficie WebGPU\");\n"
                     "    return true;\n"
                     "  }\n",
                     "create_surface")
    t = replace_once(t,
                     "  const wgpu::Status status = g_surface.GetCapabilities(g_adapter, &g_surfaceCapabilities);\n",
                     "  const bool choyaNoSurface = !g_surface && galaxy_choya_presents_now();\n"
                     "  const wgpu::Status status = choyaNoSurface\n"
                     "                                  ? wgpu::Status::Success\n"
                     "                                  : wgpu::Status(g_surface.GetCapabilities(g_adapter, &g_surfaceCapabilities));\n",
                     "GetCapabilities")
    t = replace_once(t, "  if (g_surfaceCapabilities.formatCount == 0) {\n    Log.error(\"Surface has no formats\");",
                     "  if (!choyaNoSurface && g_surfaceCapabilities.formatCount == 0) {\n"
                     "    Log.error(\"Surface has no formats\");",
                     "Surface has no formats")
    t = replace_once(t, "  if (g_surfaceCapabilities.presentModeCount == 0) {\n",
                     "  if (!choyaNoSurface && g_surfaceCapabilities.presentModeCount == 0) {\n",
                     "Surface has no present modes")
    t = replace_once(t, "  auto surfaceFormat = best_surface_format();\n",
                     "  auto surfaceFormat = choyaNoSurface ? wgpu::TextureFormat::RGBA8Unorm : best_surface_format();\n",
                     "best_surface_format")
    t = replace_once(t, "  if (!g_surface || !g_device || width == 0 || height == 0 || nativeHeight == 0 || nativeWidth == 0) {\n",
                     "  if ((!g_surface && !galaxy_choya_presents_now()) || !g_device || width == 0 || height == 0 ||\n"
                     "      nativeHeight == 0 || nativeWidth == 0) {\n",
                     "resize_swapchain_internal")
    t = replace_once(t, "    g_surface.Configure(&surfaceConfiguration);\n",
                     "    if (g_surface) g_surface.Configure(&surfaceConfiguration);\n", "Configure")
    return t


def aurora(t: str) -> str:
    t = replace_once(t, "    if (!g_surface) {\n      webgpu::refresh_surface(true);\n",
                     "    if (!g_surface && !galaxy_choya_presents_now()) {\n      webgpu::refresh_surface(true);\n",
                     "begin_frame: refresh_surface")
    # El SDL_Renderer de ImGui (backend nulo) tomaría la superficie GL de la ventana (y el KMS en la consola): con
    # Choya la ventana es del hilo de pantalla. ImGui sigue por su camino WebGPU (nulo): no se ve (BLOQUEOS).
    return replace_once(t, "  if (webgpu::g_backendType == wgpu::BackendType::Null) {\n"
                           "    AURORA_ASSERT(window::create_renderer(),",
                        "  if (webgpu::g_backendType == wgpu::BackendType::Null && !galaxy_choya_presents_now()) {\n"
                        "    AURORA_ASSERT(window::create_renderer(),",
                        "initialize: create_renderer")


def window(t: str) -> str:
    return replace_once(
        t, "  SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;\n",
        "  SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;\n"
        "  if (galaxy_choya_window_flags != nullptr) {  // galaxy-choya-native: SDL_WINDOW_OPENGL si Choya usa SDL\n"
        "    flags |= static_cast<SDL_WindowFlags>(galaxy_choya_window_flags());\n"
        "  }\n",
        "create_window: flags")


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
    # Lectura del EFB a la hora de un token (efb_recording.inc de MacioGalaxy, pegado aquí): con Choya la hace el
    # núcleo sobre lo grabado hasta este punto; Aurora sigue partiendo el cuadro igual (petari_submit_segment).
    t = replace_once(
        t,
        "  enqueue_pass(frame, g_recorder.currentRenderPass);\n  petari_submit_segment(frame, source, ticket, ready);\n",
        "  enqueue_pass(frame, g_recorder.currentRenderPass);\n"
        "  if (::galaxy_choya::enabled()) {\n"
        "    ::galaxy_choya::capture_segment(frame, ticket, ready,\n"
        "                                    ::galaxy_choya::SnapshotArea{logical.x, logical.y, source.offsetX,\n"
        "                                                                 source.offsetY, source.scaleX, source.scaleY});\n"
        "  }\n"
        "  petari_submit_segment(frame, source, ticket, ready);\n",
        "petari_capture_current_efb")
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
         "framebuffer": framebuffer, "gpu": gpu, "aurora": aurora, "window": window}
# Los de aurora_core no ven el puente (está en aurora_gx): solo declaraciones débiles, sin el .hpp.
WEAK_ONLY = {"gpu": PRESENTS, "aurora": PRESENTS,
             "window": 'extern "C" uint64_t galaxy_choya_window_flags() __attribute__((weak));  // galaxy-choya-native\n'}


def main() -> int:
    kind, src, dst = sys.argv[1:4]
    with open(src, encoding="utf-8") as f:
        text = f.read()
    if "galaxy_choya_bridge.hpp" in text:
        raise SystemExit(f"patch_aurora_bridge: {src} ya trae el puente (la entrada debe ser la fuente sin él)")
    if kind in WEAK_ONLY:
        text = KINDS[kind](text)
        i = text.index("#include")
        j = text.index("\n", i) + 1
        text = text[:j] + WEAK_ONLY[kind] + text[j:]
    else:
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
