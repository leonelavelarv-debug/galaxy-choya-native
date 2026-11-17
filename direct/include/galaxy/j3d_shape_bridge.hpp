// galaxy-choya-native: puente J3D → ruta directa (se compila dentro del árbol de MacioGalaxy, con PETARI_NATIVE).
//
// Toma un J3DShape ya cargado por los lectores de MacioGalaxy (sin cambiarlos) y convierte sus listas de
// visualización a una malla de la ruta directa, una vez, al cargar el modelo. No modifica el J3DShape ni sus arreglos:
// solo los lee mientras el modelo está vivo (el llamador libera la malla al destruir la escena).
#pragma once

#include "galaxy/gx_display_list.hpp"

class J3DShape;
class J3DVertexData;

namespace galaxy::direct {

struct ShapeConversion {
  bool ok = false;
  std::string reason;            // por qué no se pudo (la forma sigue por la ruta GX)
  std::vector<ConvertedMesh> packets;  // una malla por J3DShapeDraw (cada una con su matriz de paquete)
};

// Describe los atributos de la forma a partir de su GXVtxDescList y del GXVtxAttrFmtList de sus datos de vértice.
// vertexData: el de J3DModelData (getVertexData()), el mismo que la forma usa al dibujar por GX.
bool describe_shape(J3DShape& shape, const J3DVertexData& vertexData, GxVertexSpec& spec, std::string* reason);
ShapeConversion convert_shape(J3DShape& shape, const J3DVertexData& vertexData);

} // namespace galaxy::direct
