/* Renderer stub: accepts NV2A draws and flips, draws nothing. Replaced by gpu_vulkan.c. */
#include "rt.h"
#include "kelvin.h"

void gpu_init(void) {}
void gpu_draw(int topo, const KVertex *v, uint32_t nv, const uint32_t *idx, uint32_t ni) { (void)topo; (void)v; (void)nv; (void)idx; (void)ni; }
void gpu_clear(uint32_t flags) { (void)flags; }
void gpu_flip(uint32_t surface_phys) { (void)surface_phys; }
void gpu_vblank(void) {}
