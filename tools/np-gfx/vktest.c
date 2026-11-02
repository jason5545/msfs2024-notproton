/*
 * vktest: what the Vulkan driver behind winevulkan looks like from a Windows x64 program.
 * Prints the instance version, each physical device (name, driver, API version), the core features
 * vkd3d-proton and MSFS 2024 need (geometry/tessellation shaders and friends), the presence of the
 * device extensions vkd3d-proton cares about, and whether vkCreateDevice works.
 *
 * Build: build.sh (llvm-mingw, x86_64-w64-mingw32). Loads vulkan-1.dll at run time.
 */
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>

static PFN_vkGetInstanceProcAddr gipa;
#define IPA(inst, name) ((PFN_##name)gipa(inst, #name))

static const char *wanted_exts[] = {
  "VK_KHR_swapchain", "VK_KHR_push_descriptor", "VK_KHR_dynamic_rendering", "VK_KHR_synchronization2",
  "VK_KHR_maintenance4", "VK_KHR_maintenance5", "VK_KHR_maintenance6", "VK_KHR_buffer_device_address",
  "VK_KHR_timeline_semaphore", "VK_KHR_shader_float16_int8", "VK_KHR_draw_indirect_count",
  "VK_EXT_robustness2", "VK_EXT_descriptor_indexing", "VK_EXT_mutable_descriptor_type",
  "VK_VALVE_mutable_descriptor_type", "VK_EXT_descriptor_buffer", "VK_EXT_transform_feedback",
  "VK_EXT_extended_dynamic_state", "VK_EXT_extended_dynamic_state2", "VK_EXT_extended_dynamic_state3",
  "VK_EXT_vertex_attribute_divisor", "VK_EXT_conditional_rendering", "VK_EXT_depth_clip_enable",
  "VK_EXT_custom_border_color", "VK_EXT_shader_demote_to_helper_invocation", "VK_EXT_subgroup_size_control",
  "VK_EXT_image_view_min_lod", "VK_EXT_mesh_shader", "VK_EXT_fragment_shader_interlock",
  "VK_KHR_fragment_shading_rate", "VK_EXT_shader_stencil_export", "VK_KHR_external_memory_win32",
  "VK_KHR_external_semaphore_win32", "VK_KHR_win32_keyed_mutex", "VK_EXT_external_memory_host",
  "VK_KHR_portability_subset", "VK_KHR_ray_tracing_pipeline", "VK_KHR_acceleration_structure",
};

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  HMODULE lib = LoadLibraryA("vulkan-1.dll");
  if (!lib) { printf("LoadLibrary(vulkan-1.dll) failed %lu\n", GetLastError()); return 1; }
  gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(lib, "vkGetInstanceProcAddr");
  if (!gipa) { printf("no vkGetInstanceProcAddr\n"); return 1; }

  uint32_t iver = VK_API_VERSION_1_0;
  PFN_vkEnumerateInstanceVersion eiv = IPA(NULL, vkEnumerateInstanceVersion);
  if (eiv) eiv(&iver);
  printf("instance version %u.%u.%u\n", VK_API_VERSION_MAJOR(iver), VK_API_VERSION_MINOR(iver), VK_API_VERSION_PATCH(iver));

  uint32_t n = 0;
  IPA(NULL, vkEnumerateInstanceExtensionProperties)(NULL, &n, NULL);
  printf("instance extensions %u\n", n);

  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vktest", 1, "vktest", 1, VK_API_VERSION_1_3};
  VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
  VkInstance inst;
  VkResult r = IPA(NULL, vkCreateInstance)(&ici, NULL, &inst);
  printf("vkCreateInstance = %d\n", r);
  if (r) return 1;

  VkPhysicalDevice pds[8];
  n = 8;
  r = IPA(inst, vkEnumeratePhysicalDevices)(inst, &n, pds);
  printf("vkEnumeratePhysicalDevices = %d, %u device(s)\n", r, n);
  int failures = 0;
  for (uint32_t i = 0; i < n; i++) {
    VkPhysicalDeviceDriverProperties drv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drv};
    IPA(inst, vkGetPhysicalDeviceProperties2)(pds[i], &p2);
    VkPhysicalDeviceProperties *p = &p2.properties;
    printf("device %u: %s  api %u.%u.%u  driver \"%s\" \"%s\" id %d  vendor 0x%x device 0x%x\n", i, p->deviceName,
           VK_API_VERSION_MAJOR(p->apiVersion), VK_API_VERSION_MINOR(p->apiVersion), VK_API_VERSION_PATCH(p->apiVersion),
           drv.driverName, drv.driverInfo, drv.driverID, p->vendorID, p->deviceID);

    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
    IPA(inst, vkGetPhysicalDeviceFeatures2)(pds[i], &f2);
    VkPhysicalDeviceFeatures *f = &f2.features;
    printf("  geometryShader %u tessellationShader %u fillModeNonSolid %u shaderInt64 %u shaderFloat64 %u "
           "multiDrawIndirect %u depthClamp %u dualSrcBlend %u sampleRateShading %u independentBlend %u\n",
           f->geometryShader, f->tessellationShader, f->fillModeNonSolid, f->shaderInt64, f->shaderFloat64,
           f->multiDrawIndirect, f->depthClamp, f->dualSrcBlend, f->sampleRateShading, f->independentBlend);
    printf("  timelineSemaphore %u bufferDeviceAddress %u descriptorIndexing %u "
           "descriptorBindingPartiallyBound %u runtimeDescriptorArray %u shaderFloat16 %u\n",
           f12.timelineSemaphore, f12.bufferDeviceAddress, f12.descriptorIndexing,
           f12.descriptorBindingPartiallyBound, f12.runtimeDescriptorArray, f12.shaderFloat16);
    printf("  dynamicRendering %u synchronization2 %u maintenance4 %u shaderDemoteToHelperInvocation %u "
           "subgroupSizeControl %u\n",
           f13.dynamicRendering, f13.synchronization2, f13.maintenance4, f13.shaderDemoteToHelperInvocation,
           f13.subgroupSizeControl);

    uint32_t ne = 0;
    IPA(inst, vkEnumerateDeviceExtensionProperties)(pds[i], NULL, &ne, NULL);
    VkExtensionProperties *exts = calloc(ne, sizeof(*exts));
    IPA(inst, vkEnumerateDeviceExtensionProperties)(pds[i], NULL, &ne, exts);
    printf("  device extensions %u\n", ne);
    for (size_t w = 0; w < sizeof(wanted_exts) / sizeof(wanted_exts[0]); w++) {
      int have = 0;
      for (uint32_t e = 0; e < ne; e++) if (!strcmp(exts[e].extensionName, wanted_exts[w])) have = 1;
      printf("    %c %s\n", have ? '+' : '-', wanted_exts[w]);
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    VkDevice dev;
    r = IPA(inst, vkCreateDevice)(pds[i], &dci, NULL, &dev);
    printf("  vkCreateDevice(all reported features) = %d\n", r);
    if (r) failures++;
    else IPA(inst, vkDestroyDevice)(dev, NULL);
    free(exts);
  }
  IPA(inst, vkDestroyInstance)(inst, NULL);
  printf("RESULT %s\n", (n && !failures) ? "PASS" : "FAIL");
  return failures;
}
