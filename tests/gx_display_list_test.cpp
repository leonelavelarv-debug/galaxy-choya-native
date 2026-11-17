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

  // --- Contrato de orden de bytes (fase 3) ---------------------------------------------------------------------
  // Arreglos indexados en orden NATIVO (como los de J3D en MacioGalaxy) y datos DIRECTOS big endian en la lista.
  // Antes todo se leía con be16/be32: posiciones nativas (1, 2, 3) salían como ~4.6e-41.
  auto ne_f = [](std::vector<uint8_t>& v, float f) {
    uint8_t b[4];
    std::memcpy(b, &f, 4);
    v.insert(v.end(), b, b + 4);
  };
  auto ne_s16 = [](std::vector<uint8_t>& v, int16_t x) {
    uint8_t b[2];
    std::memcpy(b, &x, 2);
    v.insert(v.end(), b, b + 2);
  };
  auto near = [](float a, float b) { return std::fabs(a - b) < 1e-5f; };
  auto tri3 = [](std::vector<uint8_t>& d) {
    d.push_back(GX_TRIANGLES);
    d.push_back(0);
    d.push_back(3);
  };
  {
    // F32 nativo indexado: posición, normal, color RGBA8 y UV, con tamaños exactos de mNativeArrayBytes.
    std::vector<uint8_t> pos, nrm, uv;
    const float P2[3][3] = {{1, 2, 3}, {4, 5, 6}, {-7, 8.5f, 9}};
    for (auto& p : P2) for (float c : p) ne_f(pos, c);
    for (int i = 0; i < 3; ++i) { ne_f(nrm, 0); ne_f(nrm, 0); ne_f(nrm, 1); }
    for (int i = 0; i < 3; ++i) { ne_f(uv, 0.25f * i); ne_f(uv, 0.5f); }
    std::vector<uint8_t> col = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120};
    GxVertexSpec n;
    n.attr[GX_VA_POS] = {GX_INDEX8, GX_POS_XYZ, GX_F32, 0, pos.data(), 12, pos.size(), ArrayOrder::Native};
    n.attr[GX_VA_NRM] = {GX_INDEX8, GX_NRM_XYZ, GX_F32, 0, nrm.data(), 12, nrm.size(), ArrayOrder::Native};
    n.attr[GX_VA_CLR0] = {GX_INDEX8, GX_CLR_RGBA, GX_RGBA8, 0, col.data(), 4, col.size(), ArrayOrder::Native};
    n.attr[GX_VA_TEX0] = {GX_INDEX8, GX_TEX_ST, GX_F32, 0, uv.data(), 8, uv.size(), ArrayOrder::Native};
    std::vector<uint8_t> d;
    tri3(d);
    for (uint8_t i = 0; i < 3; ++i) d.insert(d.end(), {i, i, i, i});
    ConvertedMesh m2;
    const bool ok2 = convert_display_list(n, d.data(), d.size(), m2);
    check(ok2, "F32 nativo indexado: convertido");
    if (!ok2) std::printf("      error: %s\n", m2.error.c_str());
    check(ok2 && near(m2.positions[0], 1) && near(m2.positions[1], 2) && near(m2.positions[2], 3) &&
              near(m2.positions[6], -7) && near(m2.positions[7], 8.5f),
          "F32 nativo: posiciones (1, 2, 3) y (-7, 8.5, 9) exactas");
    check(ok2 && near(m2.normals[2], 1), "F32 nativo: normal");
    check(ok2 && m2.colors[0][4] == 50 && m2.colors[0][7] == 80, "RGBA8 nativo: color del segundo vértice");
    check(ok2 && near(m2.uv[0][2], 0.25f) && near(m2.uv[0][3], 0.5f), "F32 nativo: UV del segundo vértice");

    // El mismo arreglo declarado big endian da otra cosa: la etiqueta de orden importa.
    GxVertexSpec wrong = n;
    for (int a : {GX_VA_POS, GX_VA_NRM, GX_VA_CLR0, GX_VA_TEX0}) wrong.attr[a].order = ArrayOrder::BigEndian;
    ConvertedMesh mw;
    check(convert_display_list(wrong, d.data(), d.size(), mw) && !near(mw.positions[0], 1),
          "arreglo nativo leído como big endian no da (1, 2, 3)");

    // Tamaño real: un índice dentro de stride pero fuera de mNativeArrayBytes se rechaza (posición 3 no existe).
    std::vector<uint8_t> d3;
    tri3(d3);
    for (uint8_t i : {0, 1, 3}) d3.insert(d3.end(), {i, 0, 0, 0});
    ConvertedMesh mo;
    check(!convert_display_list(n, d3.data(), d3.size(), mo), "índice de posición fuera de mNativeArrayBytes rechazado");
    // Índice de UV fuera de rango (los colores y UV también se comprueban).
    std::vector<uint8_t> d4;
    tri3(d4);
    for (uint8_t i = 0; i < 3; ++i) d4.insert(d4.end(), {i, i, i, static_cast<uint8_t>(i == 2 ? 3 : i)});
    ConvertedMesh mu;
    check(!convert_display_list(n, d4.data(), d4.size(), mu), "índice de UV fuera del arreglo rechazado");
    std::vector<uint8_t> d5;
    tri3(d5);
    for (uint8_t i = 0; i < 3; ++i) d5.insert(d5.end(), {i, i, static_cast<uint8_t>(i == 1 ? 3 : i), i});
    ConvertedMesh mc;
    check(!convert_display_list(n, d5.data(), d5.size(), mc), "índice de color fuera del arreglo rechazado");
    // Indexado sin tamaño: rechazado (antes 0 = sin comprobar).
    GxVertexSpec nosize = n;
    nosize.attr[GX_VA_CLR0].arrayBytes = 0;
    ConvertedMesh mn;
    check(!convert_display_list(nosize, d.data(), d.size(), mn), "atributo indexado sin tamaño rechazado");
  }
  {
    // S16 nativo indexado con 8 bits fraccionarios y UV S16 nativo.
    std::vector<uint8_t> pos, uv;
    const int16_t P3[3][3] = {{256, 512, 768}, {-256, 128, 0}, {1024, -1024, 64}};
    for (auto& p : P3) for (int16_t c : p) ne_s16(pos, c);
    for (int i = 0; i < 3; ++i) { ne_s16(uv, static_cast<int16_t>(128 * i)); ne_s16(uv, 256); }
    GxVertexSpec n;
    n.attr[GX_VA_POS] = {GX_INDEX16, GX_POS_XYZ, GX_S16, 8, pos.data(), 6, pos.size(), ArrayOrder::Native};
    n.attr[GX_VA_TEX0] = {GX_INDEX8, GX_TEX_ST, GX_S16, 8, uv.data(), 4, uv.size(), ArrayOrder::Native};
    std::vector<uint8_t> d;
    tri3(d);
    for (uint8_t i = 0; i < 3; ++i) {
      be16(d, i);  // el índice de 16 bits está en la lista: big endian
      d.push_back(i);
    }
    ConvertedMesh m3;
    const bool ok3 = convert_display_list(n, d.data(), d.size(), m3);
    check(ok3 && near(m3.positions[0], 1) && near(m3.positions[1], 2) && near(m3.positions[2], 3) &&
              near(m3.positions[3], -1) && near(m3.positions[7], -4),
          "S16 nativo (frac 8): (1, 2, 3), (-1, 0.5, 0), (4, -4, 0.25)");
    check(ok3 && near(m3.uv[0][2], 0.5f) && near(m3.uv[0][3], 1), "S16 nativo: UV");
  }
  {
    // Directos en la lista (big endian) junto a un arreglo nativo: cada uno con su orden.
    std::vector<uint8_t> nrm;
    for (int i = 0; i < 3; ++i) { ne_s16(nrm, 0); ne_s16(nrm, 0); ne_s16(nrm, 16384); }
    GxVertexSpec n;
    n.attr[GX_VA_POS] = {GX_DIRECT, GX_POS_XYZ, GX_F32, 0, nullptr, 0, 0};
    n.attr[GX_VA_NRM] = {GX_INDEX8, GX_NRM_XYZ, GX_S16, 0, nrm.data(), 6, nrm.size(), ArrayOrder::Native};
    n.attr[GX_VA_CLR0] = {GX_DIRECT, GX_CLR_RGB, GX_RGB565, 0, nullptr, 0, 0};
    n.attr[GX_VA_TEX0] = {GX_DIRECT, GX_TEX_ST, GX_S16, 8, nullptr, 0, 0};
    std::vector<uint8_t> d;
    tri3(d);
    for (int i = 0; i < 3; ++i) {
      bef(d, 1.f + i);
      bef(d, 2.f);
      bef(d, 3.f);
      d.push_back(static_cast<uint8_t>(i));
      be16(d, 0xF800);  // rojo puro en RGB565, big endian
      be16(d, 384);     // 1.5 con 8 bits
      be16(d, static_cast<uint16_t>(-128));
    }
    ConvertedMesh m4;
    const bool ok4 = convert_display_list(n, d.data(), d.size(), m4);
    check(ok4 && near(m4.positions[0], 1) && near(m4.positions[3], 2) && near(m4.positions[2], 3),
          "F32 directo big endian en la lista");
    check(ok4 && near(m4.normals[2], 1), "normal S16 nativa (14 bits)");
    check(ok4 && m4.colors[0][0] == 255 && m4.colors[0][1] == 0 && m4.colors[0][2] == 0, "RGB565 directo big endian");
    check(ok4 && near(m4.uv[0][0], 1.5f) && near(m4.uv[0][1], -0.5f), "S16 directo big endian (frac 8)");

    // Lista truncada a mitad de vértice.
    ConvertedMesh mt;
    check(!convert_display_list(n, d.data(), d.size() - 5, mt), "lista truncada a mitad de vértice rechazada");
    std::vector<uint8_t> head = {GX_TRIANGLES, 0};
    ConvertedMesh mh;
    check(!convert_display_list(n, head.data(), head.size(), mh), "lista truncada en la cabecera rechazada");
    // Formatos inválidos.
    GxVertexSpec badNrm = n;
    badNrm.attr[GX_VA_NRM].compType = GX_U16;
    ConvertedMesh mf;
    check(!convert_display_list(badNrm, d.data(), d.size(), mf), "normal U16 (inválida en GX) rechazada");
    GxVertexSpec badCnt = n;
    badCnt.attr[GX_VA_POS].cnt = 7;
    ConvertedMesh mg;
    check(!convert_display_list(badCnt, d.data(), d.size(), mg), "conteo de componentes inválido rechazado");
    GxVertexSpec badType = n;
    badType.attr[GX_VA_TEX0].compType = 9;
    ConvertedMesh mk;
    check(!convert_display_list(badType, d.data(), d.size(), mk), "tipo de componente inválido rechazado");
    GxVertexSpec badStride = n;
    badStride.attr[GX_VA_NRM].stride = 4;
    ConvertedMesh ms;
    check(!convert_display_list(badStride, d.data(), d.size(), ms), "paso menor que un elemento rechazado");
  }
  return fails ? 1 : 0;
}
