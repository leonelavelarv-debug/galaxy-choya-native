// galaxy-choya-native: describe_vertex/convert con un J3DVertexData real de MacioGalaxy (arreglos en orden nativo,
// tamaños de mNativeArrayBytes) y una lista GX big endian, como la deja J3DModelLoader al cargar un BMD.
// Reproduce el fallo de la revisión: posiciones nativas (1, 2, 3) que salían como ~4.6e-41 con éxito.
#include "galaxy/j3d_shape_bridge.hpp"

#include "JSystem/J3DGraphBase/J3DVertex.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace galaxy::direct;
static int fails = 0;
static void check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FALLO", what);
  if (!ok) ++fails;
}
static bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

int main() {
  // Arreglos tal como los deja el cargador: componentes en el orden del host.
  alignas(4) static float pos[] = {1, 2, 3, 4, 5, 6, -7, 8.5f, 9};
  alignas(4) static float nrm[] = {0, 0, 1, 0, 1, 0, 1, 0, 0};
  alignas(4) static int16_t uv[] = {0, 256, 128, 256, 256, 0};  // S16 con 8 bits fraccionarios
  static GXColor col[] = {{10, 20, 30, 40}, {50, 60, 70, 80}, {90, 100, 110, 120}};
  static GXVtxAttrFmtList fmt[] = {{GX_VA_POS, GX_POS_XYZ, GX_F32, 0},
                                   {GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0},
                                   {GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0},
                                   {GX_VA_TEX0, GX_TEX_ST, GX_S16, 8},
                                   {GX_VA_NULL, GX_POS_XYZ, GX_F32, 0}};
  static GXVtxDescList desc[] = {{GX_VA_POS, GX_INDEX8},
                                 {GX_VA_NRM, GX_INDEX8},
                                 {GX_VA_CLR0, GX_INDEX8},
                                 {GX_VA_TEX0, GX_INDEX8},
                                 {GX_VA_NULL, GX_NONE}};
  J3DVertexData vd;
  vd.mVtxNum = 3;
  vd.mNrmNum = 3;
  vd.mColNum = 3;
  vd.mVtxAttrFmtList = fmt;
  vd.mVtxPosArray = pos;
  vd.mVtxNrmArray = nrm;
  vd.mVtxColorArray[0] = col;
  vd.mVtxTexCoordArray[0] = uv;
  vd.mNativeArrayBytes[0] = sizeof(pos);
  vd.mNativeArrayBytes[1] = sizeof(nrm);
  vd.mNativeArrayBytes[3] = sizeof(col);
  vd.mNativeArrayBytes[5] = sizeof(uv);

  GxVertexSpec spec;
  std::string why;
  const bool described = describe_vertex(desc, vd, spec, &why);
  check(described, "descriptor de J3D descrito");
  if (!described) std::printf("      motivo: %s\n", why.c_str());
  check(spec.attr[GX_VA_POS].order == ArrayOrder::Native && spec.attr[GX_VA_TEX0].order == ArrayOrder::Native,
        "arreglos de J3D marcados como nativos");
  check(spec.attr[GX_VA_POS].arrayBytes == sizeof(pos) && spec.attr[GX_VA_CLR0].arrayBytes == sizeof(col) &&
            spec.attr[GX_VA_TEX0].arrayBytes == sizeof(uv),
        "tamaños tomados de mNativeArrayBytes (posición, color y UV)");

  // La lista sigue en big endian (índices de 8 bits aquí).
  const uint8_t dl[] = {GX_TRIANGLES, 0, 3, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2};
  ConvertedMesh m;
  const bool ok = described && convert_display_list(spec, dl, sizeof(dl), m);
  check(ok, "lista convertida");
  if (!ok) std::printf("      error: %s\n", m.error.c_str());
  check(ok && near(m.positions[0], 1) && near(m.positions[1], 2) && near(m.positions[2], 3),
        "posición nativa (1, 2, 3) exacta (antes ~4.6e-41)");
  check(ok && near(m.positions[6], -7) && near(m.positions[7], 8.5f), "tercera posición (-7, 8.5, 9)");
  check(ok && near(m.normals[4], 1), "normal F32 nativa");
  check(ok && m.colors[0][4] == 50 && m.colors[0][11] == 120, "colores GXColor");
  check(ok && near(m.uv[0][1], 1) && near(m.uv[0][2], 0.5f), "UV S16 nativas con 8 bits fraccionarios");

  // Un índice que cabe en el paso pero no en el arreglo real (mNativeArrayBytes) se rechaza.
  vd.mNativeArrayBytes[5] = 8;  // solo dos UV
  GxVertexSpec small;
  ConvertedMesh m2;
  check(describe_vertex(desc, vd, small, nullptr) && !convert_display_list(small, dl, sizeof(dl), m2),
        "UV fuera del tamaño nativo rechazada");
  // Arreglo indexado sin tamaño nativo: la forma no se describe (no se lee a ciegas).
  vd.mNativeArrayBytes[3] = 0;
  GxVertexSpec none;
  check(!describe_vertex(desc, vd, none, &why), "color indexado sin tamaño nativo rechazado");
  return fails ? 1 : 0;
}
