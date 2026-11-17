// galaxy-choya-native: lo único de dawn::native que Aurora usa fuera de la API C de WebGPU (gpu.cpp: la descripción
// de instancia encadenada). El resto lo da el WebGPU nulo de Choya. Sin Dawn no hay validación ni plataforma.
#include <dawn/native/DawnNative.h>

namespace dawn::native {
DawnInstanceDescriptor::DawnInstanceDescriptor() { sType = wgpu::SType::DawnInstanceDescriptor; }
} // namespace dawn::native
