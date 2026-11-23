/* Toolchain smoke test: volk + Vulkan instance + device list, built with llvm-mingw. */
#define VK_USE_PLATFORM_WIN32_KHR
#include "volk.h"
#include <stdio.h>

int main(void)
{
    if (volkInitialize() != VK_SUCCESS) { puts("no Vulkan loader"); return 1; }
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "vk_probe", 1, "none", 1, VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app };
    VkInstance inst;
    if (vkCreateInstance(&ci, 0, &inst) != VK_SUCCESS) { puts("vkCreateInstance failed"); return 1; }
    volkLoadInstance(inst);
    uint32_t n = 8; VkPhysicalDevice pd[8];
    vkEnumeratePhysicalDevices(inst, &n, pd);
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd[i], &p);
        printf("%u: %s (Vulkan %u.%u)\n", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion));
    }
    vkDestroyInstance(inst, 0);
    return 0;
}
