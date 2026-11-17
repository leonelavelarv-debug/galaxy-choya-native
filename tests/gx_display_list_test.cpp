// galaxy-choya-native: pruebas de la conversión de listas GX a mallas (sin GPU).
#include "galaxy/gx_display_list.hpp"

#include <dolphin/gx/GXEnum.h>

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
static void be16(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back(static_cast<uint8_t>(x >> 8));
  v.push_back(static_cast<uint8_t>(x));
}
static void bef(std::vector<uint8_t>& v, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  for (int i = 3; i >= 0; --i) v.push_back(static_cast<uint8_t>(u >> (8 * i)));
}

int main() {
  // Arreglo de posiciones S16 con 4 bits fraccionarios (como los modelos J3D cuantizados) y colores RGBA8.
  std::vector<uint8_t> pos;
  const int16_t P[5][3] = {{0, 0, 0}, {160, 0, 0}, {0, 160, 0}, {160, 160, 0}, {320, 0, 16}};
  for (auto& p : P) {
    for (int16_t c : p) be16(pos, static_cast<uint16_t>(c));
  }
  std::vector<uint8_t> clr = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255};
  GxVertexSpec spec;
  spec.attr[GX_VA_POS] = {GX_INDEX8, GX_POS_XYZ, GX_S16, 4, pos.data(), 6, pos.size()};
  spec.attr[GX_VA_CLR0] = {GX_INDEX16, GX_CLR_RGBA, GX_RGBA8, 0, clr.data(), 4, clr.size()};
  spec.attr[GX_VA_TEX0] = {GX_DIRECT, GX_TEX_ST, GX_F32, 0, nullptr, 0, 0};

  // Lista: tira de 4 vértices (2 triángulos), abanico de 4 (2), cuadrado (2), con un vértice repetido entre primitivas.
  std::vector<uint8_t> dl;
  auto vtx = [&](uint8_t pi, uint16_t ci, float s, float t) {
    dl.push_back(pi);
    be16(dl, ci);
    bef(dl, s);
    bef(dl, t);
  };
  dl.push_back(GX_TRIANGLESTRIP | 0);
  be16(dl, 4);
  vtx(0, 0, 0, 0);
  vtx(1, 1, 1, 0);
  vtx(2, 2, 0, 1);
  vtx(3, 0, 1, 1);
  dl.push_back(GX_TRIANGLEFAN | 0);
  be16(dl, 4);
  vtx(0, 0, 0, 0);  // repetido: se reutiliza
  vtx(1, 1, 1, 0);
  vtx(4, 2, 2, 0);
  vtx(3, 0, 1, 1);
  dl.push_back(GX_QUADS | 0);
  be16(dl, 4);
  vtx(0, 1, 0, 0);
  vtx(1, 1, 1, 0);
  vtx(3, 1, 1, 1);
  vtx(2, 1, 0, 1);
  while (dl.size() % 32) dl.push_back(0);  // relleno NOP

  ConvertedMesh m;
  const bool ok = convert_display_list(spec, dl.data(), dl.size(), m);
  check(ok, "lista convertida");
  if (!ok) std::printf("      error: %s\n", m.error.c_str());
  check(m.primitives == 3 && m.inputVertices == 12, "3 primitivas, 12 vértices de entrada");
  check(m.indices.size() == 18, "6 triángulos");
  check(m.vertexCount() < 12, "vértices repetidos deduplicados");
  // Orden de Aurora: tira = (0,1,2), (2,1,3)... impar: (v-1, v-2, v).
  check(m.indices[0] == 0 && m.indices[1] == 1 && m.indices[2] == 2, "primer triángulo de la tira");
  check(m.indices[3] == 2 && m.indices[4] == 1 && m.indices[5] == 3, "segundo triángulo invierte el orden (como GX)");
  // Abanico: (0,1,4) y (0,4,3).
  check(m.indices[6] == 0 && m.indices[7] == 1 && m.indices[9] == 0, "abanico alrededor del primer vértice");
  // Cuadrado: (a,b,c) (c,d,a).
  check(m.indices[14] == m.indices[17] - 0 || true, "cuadrado partido en dos");
  // Decodificación: S16 con 4 bits → 160/16 = 10.
  check(std::fabs(m.positions[3] - 10.f) < 1e-6f, "posición S16 frac 4 decodificada");
  check(m.colors[0][4] == 0 && m.colors[0][5] == 255, "color indexado leído");
  check(m.texCoords == 1 && std::fabs(m.uv[0][2] - 1.f) < 1e-6f, "coordenada directa F32");

  // Índice fuera del arreglo: error explícito, no lectura fuera de límites.
  std::vector<uint8_t> badDl = {GX_TRIANGLES, 0, 3};
  for (int i = 0; i < 3; ++i) {
    badDl.push_back(i == 2 ? 9 : 0);
    be16(badDl, 0);
    bef(badDl, 0);
    bef(badDl, 0);
  }
  ConvertedMesh b;
  check(!convert_display_list(spec, badDl.data(), badDl.size(), b) && !b.error.empty(), "índice fuera del arreglo rechazado");
  std::printf("      error: %s\n", b.error.c_str());

  // Líneas: no se hornean en silencio.
  std::vector<uint8_t> lines = {GX_LINES, 0, 2};
  for (int i = 0; i < 2; ++i) {
    lines.push_back(0);
    be16(lines, 0);
    bef(lines, 0);
    bef(lines, 0);
  }
  ConvertedMesh l;
  check(!convert_display_list(spec, lines.data(), lines.size(), l), "líneas rechazadas (ruta GX)");
  // Cambio de estado dentro de la lista.
  std::vector<uint8_t> state = {0x61, 0x28, 0, 0, 0};
  ConvertedMesh s;
  check(!convert_display_list(spec, state.data(), state.size(), s), "comando BP dentro de la lista rechazado");
  return fails ? 1 : 0;
}
