// galaxy-choya-native: J3DShape → mallas de la ruta directa (ver j3d_shape_bridge.hpp).
// Se compila dentro de la configuración de MacioGalaxy (cmake/linux_port.cmake: galaxy_direct_j3d).
#include "galaxy/j3d_shape_bridge.hpp"

#include "JSystem/J3DGraphBase/J3DShape.hpp"
#include "JSystem/J3DGraphBase/J3DShapeDraw.hpp"
#include "JSystem/J3DGraphBase/J3DVertex.hpp"

namespace galaxy::direct {
namespace {

// Bytes de un elemento del arreglo de un atributo (lo que avanza un índice).
uint32_t array_stride(GXAttr attr, GXCompCnt cnt, GXCompType type) {
  auto comp = [](GXCompType t) -> uint32_t {
    switch (t) {
    case GX_U8:
    case GX_S8: return 1;
    case GX_U16:
    case GX_S16: return 2;
    case GX_F32: return 4;
    default: return 0;
    }
  };
  switch (attr) {
  case GX_VA_POS: return comp(type) * (cnt == GX_POS_XY ? 2u : 3u);
  case GX_VA_NRM: return comp(type) * 3u;
  case GX_VA_CLR0:
  case GX_VA_CLR1:
    // J3D guarda los colores del modelo como GXColor (RGBA8) aunque el formato diga otra cosa.
    return 4;
  default:
    if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) return comp(type) * (cnt == GX_TEX_S ? 1u : 2u);
    return 0;
  }
}

const void* array_of(const J3DVertexData& vd, GXAttr attr) {
  switch (attr) {
  case GX_VA_POS: return vd.getVtxPosArray();
  case GX_VA_NRM: return vd.getVtxNrmArray();
  case GX_VA_CLR0: return vd.getVtxColorArray(0);
  case GX_VA_CLR1: return vd.getVtxColorArray(1);
  default:
    if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) return vd.getVtxTexCoordArray(static_cast<u8>(attr - GX_VA_TEX0));
    return nullptr;
  }
}

} // namespace

bool describe_shape(J3DShape& shape, const J3DVertexData& vd, GxVertexSpec& spec, std::string* reason) {
  spec = GxVertexSpec{};
  const GXVtxDescList* desc = shape.getVtxDesc();
  const GXVtxAttrFmtList* fmt = vd.getVtxAttrFmtList();
  if (desc == nullptr || fmt == nullptr) {
    if (reason) *reason = "forma sin descriptor o formato de vértice";
    return false;
  }
  if (shape.getNBTFlag()) {
    if (reason) *reason = "forma con normales NBT: ruta GX";
    return false;
  }
  for (const GXVtxDescList* d = desc; d->attr != GX_VA_NULL; ++d) {
    if (d->attr > GX_VA_TEX7) {
      if (reason) *reason = "atributo fuera de POS..TEX7";
      return false;
    }
    auto& a = spec.attr[d->attr];
    a.type = static_cast<uint8_t>(d->type);
    if (d->attr <= GX_VA_TEX7MTXIDX) continue;
    const GXVtxAttrFmtList* f = fmt;
    while (f->attr != GX_VA_NULL && f->attr != d->attr) ++f;
    if (f->attr == GX_VA_NULL) {
      if (reason) *reason = "atributo sin formato en la lista de la forma";
      return false;
    }
    a.cnt = static_cast<uint8_t>(f->cnt);
    a.compType = static_cast<uint8_t>(f->type);
    a.frac = f->frac;
    if (d->type == GX_INDEX8 || d->type == GX_INDEX16) {
      a.array = static_cast<const uint8_t*>(array_of(vd, d->attr));
      a.stride = array_stride(d->attr, f->cnt, f->type);
      if (d->attr == GX_VA_POS) a.arrayBytes = static_cast<size_t>(vd.getVtxNum()) * a.stride;
      if (d->attr == GX_VA_NRM) a.arrayBytes = static_cast<size_t>(vd.getNrmNum()) * a.stride;
      if (d->attr == GX_VA_CLR0 || d->attr == GX_VA_CLR1) a.compType = GX_RGBA8;
    }
  }
  return true;
}

ShapeConversion convert_shape(J3DShape& shape, const J3DVertexData& vd) {
  ShapeConversion out;
  GxVertexSpec spec;
  if (!describe_shape(shape, vd, spec, &out.reason)) return out;
  for (u32 i = 0; i < shape.getMtxGroupNum(); ++i) {
    J3DShapeDraw* draw = shape.getShapeDraw(static_cast<u16>(i));
    if (draw == nullptr) continue;
    ConvertedMesh mesh;
    if (!convert_display_list(spec, draw->getDisplayList(), draw->getDisplayListSize(), mesh)) {
      out.reason = "paquete " + std::to_string(i) + ": " + mesh.error;
      out.packets.clear();
      return out;
    }
    out.packets.push_back(std::move(mesh));
  }
  out.ok = !out.packets.empty();
  if (!out.ok) out.reason = "forma sin paquetes";
  return out;
}

} // namespace galaxy::direct
