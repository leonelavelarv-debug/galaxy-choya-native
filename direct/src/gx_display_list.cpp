// galaxy-choya-native: listas de visualización GX → mallas (ver gx_display_list.hpp).
#include "galaxy/gx_display_list.hpp"

// Los mismos valores de hardware en la Aurora única (dolphin/) y en el SDK de Petari (revolution/).
#if __has_include(<dolphin/gx/GXEnum.h>)
#include <dolphin/gx/GXEnum.h>
#else
#include <revolution/gx/GXEnum.h>
#endif

#include <array>
#include <cstring>
#include <map>

namespace galaxy::direct {
namespace {

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t* p) {
  return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
}

// Tamaño en bytes de un componente.
int comp_bytes(uint8_t compType) {
  switch (compType) {
  case GX_U8:
  case GX_S8: return 1;
  case GX_U16:
  case GX_S16: return 2;
  case GX_F32: return 4;
  default: return 0;
  }
}

// Componentes de un atributo de GXCompCnt.
int components(int attr, uint8_t cnt) {
  if (attr == GX_VA_POS) return cnt == GX_POS_XY ? 2 : 3;
  if (attr == GX_VA_NRM) return 3;  // NBT no soportado (se rechaza antes)
  if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) return cnt == GX_CLR_RGB ? 3 : 4;
  if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) return cnt == GX_TEX_S ? 1 : 2;
  return 1;
}

// Bytes de un dato directo (o de un elemento del arreglo).
int element_bytes(int attr, const GxAttrSource& s) {
  if (attr <= GX_VA_TEX7MTXIDX) return 1;
  if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
    switch (s.compType) {
    case GX_RGB565:
    case GX_RGBA4: return 2;
    case GX_RGB8:
    case GX_RGBA6: return 3;
    case GX_RGBX8:
    case GX_RGBA8: return 4;
    default: return 0;
    }
  }
  return comp_bytes(s.compType) * components(attr, s.cnt);
}

float read_comp(const uint8_t* p, uint8_t compType, uint8_t frac) {
  const float scale = 1.f / static_cast<float>(1u << frac);
  switch (compType) {
  case GX_U8: return static_cast<float>(p[0]) * scale;
  case GX_S8: return static_cast<float>(static_cast<int8_t>(p[0])) * scale;
  case GX_U16: return static_cast<float>(be16(p)) * scale;
  case GX_S16: return static_cast<float>(static_cast<int16_t>(be16(p))) * scale;
  case GX_F32: {
    const uint32_t u = be32(p);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
  }
  default: return 0.f;
  }
}

std::array<uint8_t, 4> read_color(const uint8_t* p, uint8_t compType) {
  auto x5 = [](uint32_t v) { return static_cast<uint8_t>((v << 3) | (v >> 2)); };
  auto x6 = [](uint32_t v) { return static_cast<uint8_t>((v << 2) | (v >> 4)); };
  auto x4 = [](uint32_t v) { return static_cast<uint8_t>((v << 4) | v); };
  switch (compType) {
  case GX_RGB565: {
    const uint32_t v = be16(p);
    return {x5((v >> 11) & 31), x6((v >> 5) & 63), x5(v & 31), 255};
  }
  case GX_RGB8: return {p[0], p[1], p[2], 255};
  case GX_RGBX8: return {p[0], p[1], p[2], 255};
  case GX_RGBA4: {
    const uint32_t v = be16(p);
    return {x4((v >> 12) & 15), x4((v >> 8) & 15), x4((v >> 4) & 15), x4(v & 15)};
  }
  case GX_RGBA6: {
    const uint32_t v = (uint32_t{p[0]} << 16) | (uint32_t{p[1]} << 8) | p[2];
    return {x6((v >> 18) & 63), x6((v >> 12) & 63), x6((v >> 6) & 63), x6(v & 63)};
  }
  default: return {p[0], p[1], p[2], p[3]};
  }
}

} // namespace

bool convert_display_list(const GxVertexSpec& spec, const uint8_t* dl, size_t size, ConvertedMesh& out) {
  out = ConvertedMesh{};
  auto fail = [&](std::string why) {
    out.error = std::move(why);
    return false;
  };
  // Atributos presentes, en el orden en que GX los escribe en cada vértice.
  for (int a = GX_VA_TEX0MTXIDX; a <= GX_VA_TEX7MTXIDX; ++a) {
    if (spec.attr[a].type != GX_NONE) return fail("matrices de textura por vértice: pendiente (ruta GX)");
  }
  if (spec.attr[GX_VA_POS].type == GX_NONE) return fail("sin posición");
  if (spec.attr[GX_VA_NRM].type != GX_NONE && spec.attr[GX_VA_NRM].cnt != GX_NRM_XYZ) {
    return fail("normales NBT: pendiente (ruta GX)");
  }
  for (int a = GX_VA_POS; a <= GX_VA_TEX7; ++a) {
    const auto& s = spec.attr[a];
    if (s.type == GX_NONE) continue;
    if (element_bytes(a, s) == 0) return fail("formato de componente desconocido en el atributo " + std::to_string(a));
    if ((s.type == GX_INDEX8 || s.type == GX_INDEX16) && s.array == nullptr) {
      return fail("atributo indexado sin arreglo: " + std::to_string(a));
    }
  }
  out.hasNormal = spec.attr[GX_VA_NRM].type != GX_NONE;
  out.hasColor[0] = spec.attr[GX_VA_CLR0].type != GX_NONE;
  out.hasColor[1] = spec.attr[GX_VA_CLR1].type != GX_NONE;
  out.hasMatrixIndex = spec.attr[GX_VA_PNMTXIDX].type != GX_NONE;
  for (int t = 0; t < 8; ++t) {
    if (spec.attr[GX_VA_TEX0 + t].type != GX_NONE) out.texCoords = static_cast<uint8_t>(t + 1);
  }
  for (int t = 0; t < out.texCoords; ++t) {
    if (spec.attr[GX_VA_TEX0 + t].type == GX_NONE) return fail("coordenadas de textura no consecutivas");
  }

  // Deduplicación: la tupla de bytes de un vértice (índices o datos directos) → índice de salida.
  std::map<std::string, uint16_t> seen;
  size_t p = 0;
  std::vector<uint16_t> prim;
  while (p < size) {
    const uint8_t op = dl[p++];
    if (op == 0x00) continue;  // NOP (relleno a 32 bytes)
    if (op == 0x48) continue;  // invalidar caché de vértices
    if ((op & 0x80) == 0) {
      // Cambios de estado dentro de la lista (CP/XF/BP, cargas indexadas, llamadas): no se pueden hornear.
      return fail("comando de estado 0x" + std::to_string(op) + " dentro de la lista");
    }
    const uint8_t primType = op & 0xF8;
    if (p + 2 > size) return fail("lista truncada");
    const uint16_t count = be16(dl + p);
    p += 2;
    prim.clear();
    for (uint16_t v = 0; v < count; ++v) {
      std::string key;
      const size_t start = p;
      // Recorre los atributos para avanzar el cursor y armar la clave.
      for (int a = GX_VA_PNMTXIDX; a <= GX_VA_TEX7; ++a) {
        const auto& s = spec.attr[a];
        size_t n = 0;
        if (s.type == GX_DIRECT) n = static_cast<size_t>(element_bytes(a, s));
        else if (s.type == GX_INDEX8) n = 1;
        else if (s.type == GX_INDEX16) n = 2;
        if (p + n > size) return fail("lista truncada en un vértice");
        p += n;
      }
      key.assign(reinterpret_cast<const char*>(dl + start), p - start);
      auto it = seen.find(key);
      if (it == seen.end()) {
        if (out.vertexCount() >= 65536) return fail("más de 65536 vértices distintos");
        const uint16_t idx = static_cast<uint16_t>(out.vertexCount());
        // Decodifica el vértice nuevo.
        size_t q = start;
        for (int a = GX_VA_PNMTXIDX; a <= GX_VA_TEX7; ++a) {
          const auto& s = spec.attr[a];
          if (s.type == GX_NONE) continue;
          const uint8_t* src = nullptr;
          if (s.type == GX_DIRECT) {
            src = dl + q;
            q += static_cast<size_t>(element_bytes(a, s));
          } else {
            const uint32_t i = s.type == GX_INDEX8 ? dl[q] : be16(dl + q);
            q += s.type == GX_INDEX8 ? 1 : 2;
            const size_t off = static_cast<size_t>(i) * s.stride;
            if (s.arrayBytes != 0 && off + static_cast<size_t>(element_bytes(a, s)) > s.arrayBytes) {
              return fail("índice fuera del arreglo del atributo " + std::to_string(a));
            }
            src = s.array + off;
          }
          if (a == GX_VA_PNMTXIDX) {
            out.matrixIndices.push_back(src[0]);
          } else if (a == GX_VA_POS) {
            const int c = components(a, s.cnt);
            for (int k = 0; k < 3; ++k) {
              out.positions.push_back(k < c ? read_comp(src + k * comp_bytes(s.compType), s.compType, s.frac) : 0.f);
            }
          } else if (a == GX_VA_NRM) {
            // GX: las normales enteras usan 6 (S8) o 14 (S16) bits fraccionarios, no los del formato.
            const uint8_t nfrac = s.compType == GX_S8 ? 6 : s.compType == GX_S16 ? 14 : 0;
            for (int k = 0; k < 3; ++k) out.normals.push_back(read_comp(src + k * comp_bytes(s.compType), s.compType, nfrac));
          } else if (a == GX_VA_CLR0 || a == GX_VA_CLR1) {
            const auto c = read_color(src, s.compType);
            auto& dst = out.colors[a - GX_VA_CLR0];
            dst.insert(dst.end(), c.begin(), c.end());
          } else {
            const int t = a - GX_VA_TEX0;
            const int c = components(a, s.cnt);
            for (int k = 0; k < 2; ++k) {
              out.uv[t].push_back(k < c ? read_comp(src + k * comp_bytes(s.compType), s.compType, s.frac) : 0.f);
            }
          }
        }
        it = seen.emplace(std::move(key), idx).first;
      }
      prim.push_back(it->second);
    }
    out.inputVertices += count;
    ++out.primitives;
    // Triángulos con el mismo orden que Aurora (prepare_idx_buffer).
    auto tri = [&](uint16_t a, uint16_t b, uint16_t c) { out.indices.insert(out.indices.end(), {a, b, c}); };
    switch (primType) {
    case GX_QUADS:
    case 0x88:  // GX_QUADS2
      for (size_t i = 0; i + 3 < prim.size(); i += 4) {
        tri(prim[i], prim[i + 1], prim[i + 2]);
        tri(prim[i + 2], prim[i + 3], prim[i]);
      }
      break;
    case GX_TRIANGLES:
      for (size_t i = 0; i + 2 < prim.size(); i += 3) tri(prim[i], prim[i + 1], prim[i + 2]);
      break;
    case GX_TRIANGLESTRIP:
      if (prim.size() >= 3) tri(prim[0], prim[1], prim[2]);
      for (size_t v = 3; v < prim.size(); ++v) {
        if ((v & 1) == 0) tri(prim[v - 2], prim[v - 1], prim[v]);
        else tri(prim[v - 1], prim[v - 2], prim[v]);
      }
      break;
    case GX_TRIANGLEFAN:
      if (prim.size() >= 3) tri(prim[0], prim[1], prim[2]);
      for (size_t v = 3; v < prim.size(); ++v) tri(prim[0], prim[v - 1], prim[v]);
      break;
    default:
      // Líneas y puntos: Aurora los dibuja con un camino propio; aquí no se hornean.
      ++out.skippedPrimitives;
      return fail("primitiva de líneas/puntos (0x" + std::to_string(primType) + "): ruta GX");
    }
  }
  return true;
}

} // namespace galaxy::direct
