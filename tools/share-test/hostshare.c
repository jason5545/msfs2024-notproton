/* hostshare.c — step 0 for sharing D3D11/D3D12 textures through host memory (logs/notproton-log.md section 13).
 *
 * Checks, from a Windows x64 program running under CrossOver 27 Wine + KosmicKrisp, the premises of the
 * DXVK/vkd3d-proton patch: a pagefile-backed section (CreateFileMapping + MapViewOfFile) imported with
 * VK_EXT_external_memory_host into two VkDevices of the same process, with LINEAR images bound to it.
 *
 *   (d) extensions, minImportedHostPointerAlignment, memory types, vkGetMemoryHostPointerPropertiesEXT
 *   (a) LINEAR B8G8R8A8: device A uploads a per-pixel pattern, device B copies its image out and compares;
 *       then B clears and A reads back. Variants: two views / one view, data at view+64K / whole view
 *       imported and the image bound at offset 64K. The CPU also reads the pattern through the other view.
 *   (b) the same with OPTIMAL tiling (expected: not shared, KK binds tiled images to a private heap)
 *   (c) format/usage/flags matrix for LINEAR: vkGetPhysicalDeviceImageFormatProperties2 with and without
 *       HOST_ALLOCATION, then vkCreateImage + memory requirements + row pitch for several widths
 *
 * Build: build-hostshare.sh (brew x86_64-w64-mingw32-gcc, Vulkan headers from brew). Loads vulkan-1.dll.
 * Output: stdout and hostshare-out.txt. Exit code 0 only if (a) passes and (d) imports succeed.
 */
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

static FILE *out;
#define LOG(...) do { printf(__VA_ARGS__); fflush(stdout); if (out) { fprintf(out, __VA_ARGS__); fflush(out); } } while (0)

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkPhysicalDevice pdev;

#define FN(name) static PFN_##name p_##name
FN(vkEnumerateDeviceExtensionProperties); FN(vkGetPhysicalDeviceProperties2); FN(vkGetPhysicalDeviceMemoryProperties);
FN(vkGetPhysicalDeviceQueueFamilyProperties); FN(vkGetPhysicalDeviceImageFormatProperties2); FN(vkCreateDevice);
FN(vkDestroyDevice); FN(vkGetDeviceQueue); FN(vkCreateCommandPool); FN(vkDestroyCommandPool);
FN(vkAllocateCommandBuffers); FN(vkBeginCommandBuffer); FN(vkEndCommandBuffer); FN(vkCmdPipelineBarrier);
FN(vkCmdClearColorImage); FN(vkCmdCopyImageToBuffer); FN(vkCmdCopyBufferToImage); FN(vkQueueSubmit);
FN(vkCreateFence); FN(vkDestroyFence); FN(vkWaitForFences); FN(vkResetFences); FN(vkResetCommandBuffer);
FN(vkCreateBuffer); FN(vkDestroyBuffer); FN(vkGetBufferMemoryRequirements); FN(vkAllocateMemory);
FN(vkFreeMemory); FN(vkBindBufferMemory); FN(vkBindImageMemory); FN(vkMapMemory); FN(vkUnmapMemory);
FN(vkCreateImage); FN(vkDestroyImage); FN(vkGetImageMemoryRequirements); FN(vkGetImageSubresourceLayout);
FN(vkGetMemoryHostPointerPropertiesEXT); FN(vkDeviceWaitIdle);
#define LOAD(name) do { p_##name = (PFN_##name)gipa(inst, #name); if (!p_##name) { LOG("missing %s\n", #name); } } while (0)

static VkPhysicalDeviceMemoryProperties memprops;
static VkDeviceSize host_align;

struct dev
{
    VkDevice d;
    VkQueue q;
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkFence fence;
};

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

static int create_dev(struct dev *dv, uint32_t qf)
{
    const char *exts[] = {"VK_EXT_external_memory_host"};
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qf, 1, &prio};
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qi, 0, NULL, 1, exts, NULL};
    VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qf};
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, 0, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkResult r = p_vkCreateDevice(pdev, &di, NULL, &dv->d);
    if (r) { LOG("vkCreateDevice = %d\n", r); return 1; }
    p_vkGetDeviceQueue(dv->d, qf, 0, &dv->q);
    if ((r = p_vkCreateCommandPool(dv->d, &pi, NULL, &dv->pool))) { LOG("vkCreateCommandPool = %d\n", r); return 1; }
    ai.commandPool = dv->pool;
    if ((r = p_vkAllocateCommandBuffers(dv->d, &ai, &dv->cb))) { LOG("vkAllocateCommandBuffers = %d\n", r); return 1; }
    if ((r = p_vkCreateFence(dv->d, &fi, NULL, &dv->fence))) { LOG("vkCreateFence = %d\n", r); return 1; }
    return 0;
}

static void begin(struct dev *dv)
{
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    p_vkResetCommandBuffer(dv->cb, 0);
    p_vkBeginCommandBuffer(dv->cb, &bi);
}

static VkResult submit_wait(struct dev *dv)
{
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &dv->cb};
    VkResult r;
    p_vkEndCommandBuffer(dv->cb);
    p_vkResetFences(dv->d, 1, &dv->fence);
    if ((r = p_vkQueueSubmit(dv->q, 1, &si, dv->fence))) return r;
    return p_vkWaitForFences(dv->d, 1, &dv->fence, VK_TRUE, 10000000000ull);
}

static void barrier(struct dev *dv, VkImage img, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL,
        VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, from, to,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    p_vkCmdPipelineBarrier(dv->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static void host_barrier(struct dev *dv)
{
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT};
    p_vkCmdPipelineBarrier(dv->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

static int find_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

/* plain host-visible staging buffer on one device */
struct buf { VkBuffer b; VkDeviceMemory m; void *ptr; };
static int make_buf(struct dev *dv, VkDeviceSize size, struct buf *bf)
{
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size,
                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
    VkMemoryRequirements mr; VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; int t; VkResult r;
    if ((r = p_vkCreateBuffer(dv->d, &bi, NULL, &bf->b))) { LOG("vkCreateBuffer = %d\n", r); return 1; }
    p_vkGetBufferMemoryRequirements(dv->d, bf->b, &mr);
    t = find_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    ai.allocationSize = mr.size; ai.memoryTypeIndex = t;
    if ((r = p_vkAllocateMemory(dv->d, &ai, NULL, &bf->m))) { LOG("vkAllocateMemory(staging) = %d\n", r); return 1; }
    p_vkBindBufferMemory(dv->d, bf->b, bf->m, 0);
    p_vkMapMemory(dv->d, bf->m, 0, VK_WHOLE_SIZE, 0, &bf->ptr);
    return 0;
}
static void free_buf(struct dev *dv, struct buf *bf)
{
    p_vkUnmapMemory(dv->d, bf->m); p_vkDestroyBuffer(dv->d, bf->b, NULL); p_vkFreeMemory(dv->d, bf->m, NULL);
}

/* image imported from host memory */
struct himg { VkImage img; VkDeviceMemory mem; VkSubresourceLayout lay; VkMemoryRequirements req; uint32_t hostbits; };

static int make_himg(struct dev *dv, const char *who, VkImageTiling tiling, VkFormat fmt, uint32_t w, uint32_t h,
                     void *import_ptr, VkDeviceSize import_size, VkDeviceSize bind_off, struct himg *hi)
{
    VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, NULL,
                                           VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext, 0, VK_IMAGE_TYPE_2D, fmt, {w, h, 1}, 1, 1,
        VK_SAMPLE_COUNT_1_BIT, tiling,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkMemoryHostPointerPropertiesEXT hp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkImportMemoryHostPointerInfoEXT imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, NULL,
                                            VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, import_ptr};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, import_size, 0};
    VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    VkResult r; int t;

    memset(hi, 0, sizeof(*hi));
    if ((r = p_vkCreateImage(dv->d, &ci, NULL, &hi->img))) { LOG("  %s: vkCreateImage = %d\n", who, r); return 1; }
    p_vkGetImageMemoryRequirements(dv->d, hi->img, &hi->req);
    if (tiling == VK_IMAGE_TILING_LINEAR) p_vkGetImageSubresourceLayout(dv->d, hi->img, &sub, &hi->lay);
    r = p_vkGetMemoryHostPointerPropertiesEXT(dv->d, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, import_ptr, &hp);
    hi->hostbits = hp.memoryTypeBits;
    t = find_type(hi->req.memoryTypeBits & hp.memoryTypeBits, 0);
    LOG("  %s: image req size %llu align %llu bits %#x; host ptr %p props = %d bits %#x -> type %d; rowPitch %llu offset %llu size %llu\n",
        who, (unsigned long long)hi->req.size, (unsigned long long)hi->req.alignment, hi->req.memoryTypeBits, import_ptr, r,
        hp.memoryTypeBits, t, (unsigned long long)hi->lay.rowPitch, (unsigned long long)hi->lay.offset, (unsigned long long)hi->lay.size);
    if (r || t < 0) return 1;
    if (bind_off + hi->req.size > import_size) { LOG("  %s: image does not fit\n", who); return 1; }
    ai.memoryTypeIndex = t;
    if ((r = p_vkAllocateMemory(dv->d, &ai, NULL, &hi->mem))) { LOG("  %s: vkAllocateMemory(import %p, %llu) = %d\n", who, import_ptr, (unsigned long long)import_size, r); return 1; }
    if ((r = p_vkBindImageMemory(dv->d, hi->img, hi->mem, bind_off))) { LOG("  %s: vkBindImageMemory(offset %llu) = %d\n", who, (unsigned long long)bind_off, r); return 1; }
    return 0;
}
static void free_himg(struct dev *dv, struct himg *hi)
{
    if (hi->img) p_vkDestroyImage(dv->d, hi->img, NULL);
    if (hi->mem) p_vkFreeMemory(dv->d, hi->mem, NULL);
    memset(hi, 0, sizeof(*hi));
}

#define W 256
#define H 256
static uint32_t pattern(uint32_t x, uint32_t y) /* B8G8R8A8 bytes B,G,R,A as a little-endian dword */
{
    return (x & 0xff) | ((y & 0xff) << 8) | (((x ^ y) * 7 & 0xff) << 16) | 0xff000000u;
}

/* counts pixels equal to pattern() (mode 0) or to a solid dword (mode 1) */
static int count(const unsigned char *base, VkDeviceSize pitch, int solid, uint32_t want)
{
    int ok = 0;
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++)
        {
            uint32_t v; memcpy(&v, base + y * pitch + x * 4, 4);
            if (v == (solid ? want : pattern(x, y))) ok++;
        }
    return ok;
}


/* (a)/(b): returns 0 if both directions shared */
static int share_case(struct dev *A, struct dev *B, VkImageTiling tiling, int same_view, int whole_view)
{
    const VkDeviceSize SZ = 1024 * 1024, OFF = 65536;
    HANDLE sec; unsigned char *v1, *v2; struct himg ia, ib; struct buf up, rb; int ok_b, ok_cpu, ok_a, rc = 1;
    VkBufferImageCopy reg = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, H, 1}};
    VkClearColorValue cc = {{1.0f, 0.25f, 0.0f, 1.0f}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const uint32_t solid = 0xffff4000u; /* B8G8R8A8 of (1.0, 0.25, 0, 1) as a dword: B=0x00 G=0x40 R=0xff A=0xff */
    VkResult r;

    LOG("== %s, %s, %s\n", tiling == VK_IMAGE_TILING_LINEAR ? "LINEAR" : "OPTIMAL",
        same_view ? "one view for both devices" : "two views of one section",
        whole_view ? "whole view imported, image bound at +64K" : "view+64K imported, image bound at 0");
    sec = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)SZ, NULL);
    if (!sec) { LOG("  CreateFileMapping failed %lu\n", GetLastError()); return 1; }
    v1 = MapViewOfFile(sec, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    v2 = same_view ? v1 : MapViewOfFile(sec, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!v1 || !v2) { LOG("  MapViewOfFile failed %lu\n", GetLastError()); return 1; }
    memset(v1, 0, SZ);
    if (make_himg(A, "A", tiling, VK_FORMAT_B8G8R8A8_UNORM, W, H, whole_view ? v1 : v1 + OFF, whole_view ? SZ : SZ - OFF, whole_view ? OFF : 0, &ia)) goto done;
    if (make_himg(B, "B", tiling, VK_FORMAT_B8G8R8A8_UNORM, W, H, whole_view ? v2 : v2 + OFF, whole_view ? SZ : SZ - OFF, whole_view ? OFF : 0, &ib)) goto done;
    if (tiling == VK_IMAGE_TILING_LINEAR && ia.lay.rowPitch != ib.lay.rowPitch) LOG("  rowPitch differs: A %llu B %llu\n", (unsigned long long)ia.lay.rowPitch, (unsigned long long)ib.lay.rowPitch);

    /* A: upload the pattern */
    if (make_buf(A, W * H * 4, &up)) goto done;
    for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) { uint32_t v = pattern(x, y); memcpy((char *)up.ptr + (y * W + x) * 4, &v, 4); }
    begin(A);
    barrier(A, ia.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    p_vkCmdCopyBufferToImage(A->cb, up.b, ia.img, VK_IMAGE_LAYOUT_GENERAL, 1, &reg);
    host_barrier(A);
    if ((r = submit_wait(A))) LOG("  A submit = %d\n", r);
    free_buf(A, &up);

    /* CPU: read the pattern through the other view */
    if (tiling == VK_IMAGE_TILING_LINEAR)
    {
        ok_cpu = count(v2 + OFF + ia.lay.offset, ia.lay.rowPitch, 0, 0);
        LOG("  CPU reads A's upload through %s: %d / %d\n", same_view ? "the view" : "view 2", ok_cpu, W * H);
    }
    else
    {
        int nz = 0; for (VkDeviceSize i = OFF; i < SZ; i++) nz += v2[i] != 0;
        LOG("  CPU: %d nonzero bytes in the section after A's upload\n", nz);
    }

    /* B: copy its image out */
    if (make_buf(B, W * H * 4, &rb)) goto done;
    begin(B);
    barrier(B, ib.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    p_vkCmdCopyImageToBuffer(B->cb, ib.img, VK_IMAGE_LAYOUT_GENERAL, rb.b, 1, &reg);
    host_barrier(B);
    if ((r = submit_wait(B))) LOG("  B submit = %d\n", r);
    ok_b = count(rb.ptr, W * 4, 0, 0);
    LOG("  B reads A's pattern: px(0,0) %08x px(255,255) %08x; matching %d / %d -> %s\n",
        ((uint32_t *)rb.ptr)[0], ((uint32_t *)rb.ptr)[W * H - 1], ok_b, W * H, ok_b == W * H ? "SHARED OK" : "NOT SHARED");

    /* reverse: B clears, A reads */
    begin(B);
    p_vkCmdClearColorImage(B->cb, ib.img, VK_IMAGE_LAYOUT_GENERAL, &cc, 1, &range);
    host_barrier(B);
    if ((r = submit_wait(B))) LOG("  B submit = %d\n", r);
    free_buf(B, &rb);
    if (make_buf(A, W * H * 4, &rb)) goto done;
    begin(A);
    p_vkCmdCopyImageToBuffer(A->cb, ia.img, VK_IMAGE_LAYOUT_GENERAL, rb.b, 1, &reg);
    host_barrier(A);
    if ((r = submit_wait(A))) LOG("  A submit = %d\n", r);
    ok_a = count(rb.ptr, W * 4, 1, solid);
    LOG("  A reads B's clear: px(0,0) %08x; matching %d / %d -> %s\n", ((uint32_t *)rb.ptr)[0], ok_a, W * H,
        ok_a == W * H ? "SHARED OK" : "NOT SHARED");
    free_buf(A, &rb);
    rc = (ok_b == W * H && ok_a == W * H) ? 0 : 2;
done:
    p_vkDeviceWaitIdle(A->d); p_vkDeviceWaitIdle(B->d);
    free_himg(A, &ia); free_himg(B, &ib);
    if (!same_view) UnmapViewOfFile(v2);
    UnmapViewOfFile(v1);
    CloseHandle(sec);
    return rc;
}

static const struct { VkFormat f; const char *n; } fmts[] = {
    {VK_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM"}, {VK_FORMAT_B8G8R8A8_SRGB, "B8G8R8A8_SRGB"},
    {VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM"}, {VK_FORMAT_R8G8B8A8_SRGB, "R8G8B8A8_SRGB"},
    {VK_FORMAT_A2B10G10R10_UNORM_PACK32, "A2B10G10R10_UNORM"}, {VK_FORMAT_R16G16B16A16_SFLOAT, "R16G16B16A16_SFLOAT"},
    {VK_FORMAT_R32G32B32A32_SFLOAT, "R32G32B32A32_SFLOAT"}, {VK_FORMAT_R8_UNORM, "R8_UNORM"},
    {VK_FORMAT_R8G8_UNORM, "R8G8_UNORM"}, {VK_FORMAT_R16_UNORM, "R16_UNORM"},
    {VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, "NV12(G8_B8R8_2PLANE_420)"}, {VK_FORMAT_D32_SFLOAT, "D32_SFLOAT"},
};
#define TS VK_IMAGE_USAGE_TRANSFER_SRC_BIT
#define TD VK_IMAGE_USAGE_TRANSFER_DST_BIT
#define SA VK_IMAGE_USAGE_SAMPLED_BIT
#define CA VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
#define ST VK_IMAGE_USAGE_STORAGE_BIT
static const struct { VkImageUsageFlags u; const char *n; } usages[] = {
    {TS | TD | SA, "T|S"}, {TS | TD | SA | CA, "T|S|RT"}, {TS | TD | SA | CA | ST, "T|S|RT|UAV"}, {TS | TD | SA | ST, "T|S|UAV"},
    {TS | TD | CA, "T|RT"},
};
static const struct { VkImageCreateFlags f; const char *n; } cflags[] = {
    {0, "0"}, {VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, "MUT"}, {VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT, "MUT|EXT"},
};

static void matrix(struct dev *A)
{
    static const uint32_t widths[] = {256, 600, 1920, 3840};
    LOG("== (c) LINEAR format matrix (query without / with HOST_ALLOCATION; create with HOST_ALLOCATION 1920x1080)\n");
    for (size_t fi = 0; fi < sizeof(fmts) / sizeof(fmts[0]); fi++)
    {
        uint64_t pitch_seen[4] = {0}; int pitch_mismatch = 0;
        LOG("%s\n", fmts[fi].n);
        for (size_t ui = 0; ui < sizeof(usages) / sizeof(usages[0]); ui++)
            for (size_t ci = 0; ci < sizeof(cflags) / sizeof(cflags[0]); ci++)
            {
                VkPhysicalDeviceExternalImageFormatInfo ei = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, NULL,
                                                             VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
                VkPhysicalDeviceImageFormatInfo2 qi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, NULL,
                    fmts[fi].f, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_LINEAR, usages[ui].u, cflags[ci].f};
                VkExternalImageFormatProperties ep = {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
                VkImageFormatProperties2 p0 = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2}, p1 = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &ep};
                VkResult r0, r1, rc = VK_RESULT_MAX_ENUM;
                char extra[512] = "";
                r0 = p_vkGetPhysicalDeviceImageFormatProperties2(pdev, &qi, &p0);
                qi.pNext = &ei;
                r1 = p_vkGetPhysicalDeviceImageFormatProperties2(pdev, &qi, &p1);
                if (r1 == VK_SUCCESS)
                {
                    size_t len = 0;
                    for (size_t wi = 0; wi < 4; wi++)
                    {
                        VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, NULL,
                                                               VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
                        VkFormat vf[2] = {fmts[fi].f, fmts[fi].f};
                        VkImageFormatListCreateInfo fl = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, NULL, 1, vf};
                        VkImageCreateInfo ci2 = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext, cflags[ci].f, VK_IMAGE_TYPE_2D, fmts[fi].f,
                            {widths[wi], widths[wi] == 3840 ? 2160 : 1080, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_LINEAR,
                            usages[ui].u, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
                        VkImage img; VkMemoryRequirements mr; VkSubresourceLayout lay = {0};
                        VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
                        if (cflags[ci].f & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) ext.pNext = &fl;
                        if (fmts[fi].f == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM) sub.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
                        rc = p_vkCreateImage(A->d, &ci2, NULL, &img);
                        if (rc) { len += snprintf(extra + len, sizeof(extra) - len, " w%u:create=%d", widths[wi], rc); continue; }
                        p_vkGetImageMemoryRequirements(A->d, img, &mr);
                        p_vkGetImageSubresourceLayout(A->d, img, &sub, &lay);
                        if (!pitch_seen[wi]) pitch_seen[wi] = lay.rowPitch; else if (pitch_seen[wi] != lay.rowPitch) pitch_mismatch = 1;
                        if (wi == 2) len += snprintf(extra + len, sizeof(extra) - len, " create=0 size=%llu align=%llu bits=%#x",
                                                     (unsigned long long)mr.size, (unsigned long long)mr.alignment, mr.memoryTypeBits);
                        len += snprintf(extra + len, sizeof(extra) - len, " w%u:pitch=%llu", widths[wi], (unsigned long long)lay.rowPitch);
                        if (fmts[fi].f == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM && wi == 2)
                        {
                            VkSubresourceLayout l1 = {0}; VkImageSubresource s1 = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0};
                            p_vkGetImageSubresourceLayout(A->d, img, &s1, &l1);
                            len += snprintf(extra + len, sizeof(extra) - len, " [plane1 off=%llu pitch=%llu]",
                                            (unsigned long long)l1.offset, (unsigned long long)l1.rowPitch);
                        }
                        p_vkDestroyImage(A->d, img, NULL);
                    }
                }
                LOG("  %-11s %-8s plain=%d host=%d", usages[ui].n, cflags[ci].n, r0, r1);
                if (r1 == VK_SUCCESS)
                    LOG(" maxExt=%ux%u mips=%u layers=%u samples=%#x extFeat=%#x compat=%#x%s", p1.imageFormatProperties.maxExtent.width,
                        p1.imageFormatProperties.maxExtent.height, p1.imageFormatProperties.maxMipLevels,
                        p1.imageFormatProperties.maxArrayLayers, p1.imageFormatProperties.sampleCounts,
                        ep.externalMemoryProperties.externalMemoryFeatures, ep.externalMemoryProperties.compatibleHandleTypes, extra);
                LOG("\n");
            }
        LOG("  row pitch same for every usage/flags: %s (256:%llu 600:%llu 1920:%llu 3840:%llu)\n", pitch_mismatch ? "NO" : "yes",
            (unsigned long long)pitch_seen[0], (unsigned long long)pitch_seen[1], (unsigned long long)pitch_seen[2], (unsigned long long)pitch_seen[3]);
    }
}

int main(void)
{
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "hostshare", 1, "hostshare", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &hostp};
    VkExtensionProperties *ext; uint32_t n = 1, ne = 0, nq = 0, qf = ~0u;
    VkQueueFamilyProperties qfp[16];
    struct dev A = {0}, B = {0};
    HMODULE lib; VkResult r; int fails = 0;
    static const char *show[] = {"VK_EXT_external_memory_host", "VK_KHR_external_memory", "VK_KHR_external_memory_win32",
        "VK_KHR_external_memory_fd", "VK_KHR_win32_keyed_mutex", "VK_KHR_external_semaphore_win32", "VK_KHR_external_semaphore",
        "VK_EXT_image_drm_format_modifier", "VK_KHR_timeline_semaphore"};

    out = fopen("hostshare-out.txt", "w");
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!(lib = LoadLibraryA("vulkan-1.dll"))) { LOG("LoadLibrary(vulkan-1.dll) failed %lu\n", GetLastError()); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(lib, "vkGetInstanceProcAddr");
    if ((r = ((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &inst))) { LOG("vkCreateInstance = %d\n", r); return 1; }
    if ((r = ((PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices"))(inst, &n, &pdev)) < 0 || !n) { LOG("no physical device (%d)\n", r); return 1; }
    LOAD(vkEnumerateDeviceExtensionProperties); LOAD(vkGetPhysicalDeviceProperties2); LOAD(vkGetPhysicalDeviceMemoryProperties);
    LOAD(vkGetPhysicalDeviceQueueFamilyProperties); LOAD(vkGetPhysicalDeviceImageFormatProperties2); LOAD(vkCreateDevice);
    LOAD(vkDestroyDevice); LOAD(vkGetDeviceQueue); LOAD(vkCreateCommandPool); LOAD(vkDestroyCommandPool);
    LOAD(vkAllocateCommandBuffers); LOAD(vkBeginCommandBuffer); LOAD(vkEndCommandBuffer); LOAD(vkCmdPipelineBarrier);
    LOAD(vkCmdClearColorImage); LOAD(vkCmdCopyImageToBuffer); LOAD(vkCmdCopyBufferToImage); LOAD(vkQueueSubmit);
    LOAD(vkCreateFence); LOAD(vkDestroyFence); LOAD(vkWaitForFences); LOAD(vkResetFences); LOAD(vkResetCommandBuffer);
    LOAD(vkCreateBuffer); LOAD(vkDestroyBuffer); LOAD(vkGetBufferMemoryRequirements); LOAD(vkAllocateMemory);
    LOAD(vkFreeMemory); LOAD(vkBindBufferMemory); LOAD(vkBindImageMemory); LOAD(vkMapMemory); LOAD(vkUnmapMemory);
    LOAD(vkCreateImage); LOAD(vkDestroyImage); LOAD(vkGetImageMemoryRequirements); LOAD(vkGetImageSubresourceLayout);
    LOAD(vkGetMemoryHostPointerPropertiesEXT); LOAD(vkDeviceWaitIdle);

    /* (d) */
    p_vkGetPhysicalDeviceProperties2(pdev, &p2);
    LOG("== (d) device %s, api %u.%u.%u\n", p2.properties.deviceName, VK_API_VERSION_MAJOR(p2.properties.apiVersion),
        VK_API_VERSION_MINOR(p2.properties.apiVersion), VK_API_VERSION_PATCH(p2.properties.apiVersion));
    p_vkEnumerateDeviceExtensionProperties(pdev, NULL, &ne, NULL);
    ext = calloc(ne, sizeof(*ext));
    p_vkEnumerateDeviceExtensionProperties(pdev, NULL, &ne, ext);
    for (size_t i = 0; i < sizeof(show) / sizeof(show[0]); i++) LOG("  %-36s %d\n", show[i], has_ext(ext, ne, show[i]));
    host_align = hostp.minImportedHostPointerAlignment;
    LOG("  minImportedHostPointerAlignment %llu\n", (unsigned long long)host_align);
    p_vkGetPhysicalDeviceMemoryProperties(pdev, &memprops);
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
        LOG("  memory type %u: flags %#x heap %u (%llu MiB)\n", i, memprops.memoryTypes[i].propertyFlags, memprops.memoryTypes[i].heapIndex,
            (unsigned long long)(memprops.memoryHeaps[memprops.memoryTypes[i].heapIndex].size >> 20));
    if (!has_ext(ext, ne, "VK_EXT_external_memory_host")) { LOG("no VK_EXT_external_memory_host\n"); return 1; }
    nq = 16; p_vkGetPhysicalDeviceQueueFamilyProperties(pdev, &nq, qfp);
    for (uint32_t i = 0; i < nq && qf == ~0u; i++) if (qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) qf = i;
    if (create_dev(&A, qf) || create_dev(&B, qf)) return 1;
    LOG("  two VkDevices created (queue family %u)\n", qf);
    {
        /* host pointer properties for a section view, an offset into it, and a private VirtualAlloc block */
        HANDLE sec = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 1 << 20, NULL);
        unsigned char *v = sec ? MapViewOfFile(sec, FILE_MAP_ALL_ACCESS, 0, 0, 0) : NULL;
        unsigned char *va = VirtualAlloc(NULL, 1 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        unsigned char *ptrs[4] = {v, v + 65536, v + 4096, va};
        const char *names[4] = {"section view", "view+64K", "view+4K", "VirtualAlloc"};
        LOG("  section %p view %p, VirtualAlloc %p\n", sec, v, va);
        for (int i = 0; i < 4; i++)
        {
            VkMemoryHostPointerPropertiesEXT hp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
            VkImportMemoryHostPointerInfoEXT imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, NULL,
                                                    VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, ptrs[i]};
            VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, 65536, 0};
            VkDeviceMemory m = VK_NULL_HANDLE; VkResult ra;
            if (!ptrs[i]) continue;
            r = p_vkGetMemoryHostPointerPropertiesEXT(A.d, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, ptrs[i], &hp);
            ai.memoryTypeIndex = hp.memoryTypeBits ? (uint32_t)__builtin_ctz(hp.memoryTypeBits) : 0;
            ra = p_vkAllocateMemory(A.d, &ai, NULL, &m);
            LOG("  %-12s %p: vkGetMemoryHostPointerPropertiesEXT = %d bits %#x; import 64K as type %u = %d\n",
                names[i], ptrs[i], r, hp.memoryTypeBits, ai.memoryTypeIndex, ra);
            if (i < 2 && (r || ra)) fails++;
            if (m) p_vkFreeMemory(A.d, m, NULL);
        }
        if (v) UnmapViewOfFile(v);
        if (sec) CloseHandle(sec);
        if (va) VirtualFree(va, 0, MEM_RELEASE);
    }

    /* (a) */
    LOG("== (a) LINEAR sharing between two VkDevices\n");
    fails += share_case(&A, &B, VK_IMAGE_TILING_LINEAR, 0, 0) != 0;
    fails += share_case(&A, &B, VK_IMAGE_TILING_LINEAR, 1, 0) != 0;
    fails += share_case(&A, &B, VK_IMAGE_TILING_LINEAR, 0, 1) != 0;
    /* (b) */
    LOG("== (b) OPTIMAL (expected NOT SHARED)\n");
    share_case(&A, &B, VK_IMAGE_TILING_OPTIMAL, 0, 0);
    /* (c) */
    matrix(&A);

    p_vkDestroyFence(A.d, A.fence, NULL); p_vkDestroyCommandPool(A.d, A.pool, NULL); p_vkDestroyDevice(A.d, NULL);
    p_vkDestroyFence(B.d, B.fence, NULL); p_vkDestroyCommandPool(B.d, B.pool, NULL); p_vkDestroyDevice(B.d, NULL);
    LOG("RESULT %s (%d failure(s) in (a)/(d))\n", fails ? "FAIL" : "PASS", fails);
    if (out) fclose(out);
    return fails ? 1 : 0;
}
