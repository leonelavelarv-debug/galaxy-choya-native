// galaxy-choya-native: conversión de listas de visualización GX (las de J3DShape) a mallas de la ruta directa.
//
// Paso 1 de la migración J3D: la geometría estática se decodifica UNA vez al cargar el modelo (no por cuadro).
// Entrada: descriptor de vértice (GXVtxDesc), formato (GXVtxAttrFmt), arreglos (GXSetArray) y la lista.
// Salida: vértices ya decodificados (float / RGBA8), deduplicados por tupla de índices, y triángulos con el mismo
// orden de vértices que Aurora (command_processor.cpp: prepare_idx_buffer) para que el descarte de caras coincida.
//
// Lo que no se convierte se informa (no se descarta en silencio): líneas y puntos, formatos de vértice sin soporte,
// comandos que cambian estado a mitad de la lista. En ese caso la forma sigue por la ruta GX.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace galaxy::direct {

// Orden de bytes de un arreglo indexado. Los datos DIRECTOS van dentro de la lista GX y son siempre big endian (la
// lista no se convierte al cargar). Los arreglos indexados dependen de quién los dio:
//  - Native: los de J3D en MacioGalaxy (J3DModelLoader los pasa al orden del host al cargar y J3DShape los publica
//    con GXSetArray(..., true)); su tamaño exacto está en J3DVertexData::mNativeArrayBytes.
//  - BigEndian: los que llegan tal como están en el disco (GXSetArray sin conversión).
enum class ArrayOrder : uint8_t { BigEndian, Native };

struct GxAttrSource {
  uint8_t type = 0;          // GX_NONE, GX_DIRECT, GX_INDEX8, GX_INDEX16
  uint8_t cnt = 0;           // GXCompCnt (GX_POS_XY/XYZ, GX_NRM_XYZ, GX_CLR_RGB/RGBA, GX_TEX_S/ST)
  uint8_t compType = 0;      // GXCompType
  uint8_t frac = 0;          // bits fraccionarios de los enteros
  const uint8_t* array = nullptr;  // GXSetArray (solo atributos indexados)
  uint32_t stride = 0;
  size_t arrayBytes = 0;     // tamaño del arreglo: obligatorio en los indexados (0 = se rechaza)
  ArrayOrder order = ArrayOrder::BigEndian;  // orden de los componentes del arreglo (no de los datos directos)
};

struct GxVertexSpec {
  // Índices de GXAttr: 0 PNMTXIDX, 1..8 TEXnMTXIDX, 9 POS, 10 NRM, 11 CLR0, 12 CLR1, 13..20 TEX0..7.
  GxAttrSource attr[21];
};

struct ConvertedMesh {
  bool hasNormal = false, hasColor[2] = {false, false}, hasMatrixIndex = false;
  uint8_t texCoords = 0;
  std::vector<float> positions, normals;
  std::vector<uint8_t> colors[2];
  std::vector<float> uv[8];
  std::vector<uint8_t> matrixIndices;
  std::vector<uint16_t> indices;  // triángulos
  uint32_t vertexCount() const { return static_cast<uint32_t>(positions.size() / 3); }
  // Diagnóstico.
  uint32_t primitives = 0, skippedPrimitives = 0, inputVertices = 0;
  std::string error;  // vacío si todo se convirtió
};

// Convierte la lista. Devuelve false (con error) si algo no se puede convertir sin perder información.
bool convert_display_list(const GxVertexSpec& spec, const uint8_t* dl, size_t size, ConvertedMesh& out);

} // namespace galaxy::direct
