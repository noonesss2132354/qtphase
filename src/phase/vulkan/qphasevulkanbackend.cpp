// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qphasevulkanbackend_p.h"
#include "../qphasescenearena_p.h"
#include "../cpu/qphasecpurasterizer_p.h"
#include "../cpu/qphasesharedmath_p.h"

#include <qpa/qplatformwindow.h>
#include <QtCore/qcoreapplication.h>
#include <QtCore/qdir.h>
#include <QtCore/qelapsedtimer.h>
#include <QtCore/qfile.h>
#include <QtCore/qstandardpaths.h>
#include <QtCore/qdebug.h>

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_VULKAN_VERSION 1003000
#include <vulkan/vulkan.h>
#include "../../3rdparty/VulkanMemoryAllocator/vk_mem_alloc.h"

#include "phase_uber_vert_spv.h"
#include "phase_uber_frag_spv.h"
#include "phase_prim_prep_comp_spv.h"
#include "phase_tile_bin_comp_spv.h"
#include "phase_glyph_bake_comp_spv.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#ifdef Q_OS_LINUX
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

QT_BEGIN_NAMESPACE

namespace {

static constexpr uint32_t kFramesInFlight = 2;
static constexpr uint32_t kMaxBindlessTextures = 4096;
static constexpr uint32_t kTileSize = 16;
static constexpr uint32_t kGlyphAtlasWidth = 2048;  // Shared 2048x2048 R8_UNORM = 4 MB process-wide VRAM atlas!
static constexpr uint32_t kGlyphAtlasHeight = 2048;
static constexpr int kMaxCachedGlyphDim = 1024;     // Bakes ALL glyphs & large/rotated SVG icons via 8x8 blocks!
static constexpr uint32_t kDrmFormatXrgb8888 = 0x34325258u; // DRM_FORMAT_XRGB8888 ('X','R','2','4')
static constexpr uint32_t kDrmFormatArgb8888 = 0x34325241u; // DRM_FORMAT_ARGB8888 ('A','R','2','4')
static constexpr uint32_t kMaxDmaBufRingSlots = 4u;

struct GpuGlyphBakeCmd {
    uint32_t curveTexelOffset = 0;
    int32_t atlasX = 0;
    int32_t atlasY = 0;
    int32_t width = 0;
    int32_t height = 0;
    float originDesignX = 0.0f;
    float originDesignY = 0.0f;
    float stepDesignXx = 0.0f;
    float stepDesignXy = 0.0f;
    float stepDesignYx = 0.0f;
    float stepDesignYy = 0.0f;
    float pixelsPerUnitX = 0.0f;
    float pixelsPerUnitY = 0.0f;
    float devicePixelRatio = 1.0f;
    uint32_t pad0 = 0;
    uint32_t pad1 = 0;
};
static_assert(sizeof(GpuGlyphBakeCmd) == 64, "GpuGlyphBakeCmd size mismatch");

struct GpuGlyphBakeBlock {
    uint32_t cmdIdx = 0;
    uint32_t blockOriginX = 0;
    uint32_t blockOriginY = 0;
    uint32_t pad0 = 0;
};
static_assert(sizeof(GpuGlyphBakeBlock) == 16, "GpuGlyphBakeBlock size mismatch");

struct GpuGlyphSlot {
    uint32_t packedOrigXY = 0;
    uint32_t packedAtlasXY = 0;
    uint32_t packedWH = 0;
};
static_assert(sizeof(GpuGlyphSlot) == 12, "GpuGlyphSlot size mismatch");

struct GpuPreparedPrim {
    float effBounds[4]{};
    float opaqueBounds[4]{};
    uint32_t primIdx = 0;
    uint32_t flags = 0;
    float effAnimOp = 1.0f;
    uint32_t pad0 = 0;
};
static_assert(sizeof(GpuPreparedPrim) == 48, "GpuPreparedPrim size mismatch");

struct PhaseGlyphBakePushConstants {
    uint64_t curveArenaAddr = 0;
    uint64_t bakeCmdsAddr = 0;
    uint64_t bakeBlocksAddr = 0;
    uint32_t blockCount = 0;
    uint32_t pad0 = 0;
};
static_assert(sizeof(PhaseGlyphBakePushConstants) == 32, "PhaseGlyphBakePushConstants size mismatch");

struct PhasePrimPrepPushConstants {
    uint64_t transformsAddr = 0;
    uint64_t clipsAddr = 0;
    uint64_t primitivesAddr = 0;
    uint64_t drawOrderAddr = 0;
    uint64_t preparedPrimsAddr = 0;
    uint64_t superTileMasksAddr = 0;
    uint64_t frameTimeNs = 0;
    float viewportWidth = 0.0f;
    float viewportHeight = 0.0f;
    float devicePixelRatio = 1.0f;
    uint32_t drawCount = 0;
    uint32_t numPrimWords = 0;
    uint32_t superMinX = 0;
    uint32_t superMinY = 0;
    uint32_t activeSuperTilesX = 0;
    uint32_t activeSuperTilesY = 0;
    uint32_t pad0 = 0;
};
static_assert(sizeof(PhasePrimPrepPushConstants) == 96, "PhasePrimPrepPushConstants size mismatch");

struct PhaseTileBinPushConstants {
    uint64_t preparedPrimsAddr = 0;
    uint64_t superTileMasksAddr = 0;
    uint64_t tileCounterAddr = 0;
    uint64_t tileHeadersAddr = 0;
    uint64_t tileItemsAddr = 0;
    float viewportWidth = 0.0f;
    float viewportHeight = 0.0f;
    uint32_t numPrimWords = 0;
    uint32_t tilesX = 0;
    uint32_t tilesY = 0;
    uint32_t tileMinX = 0;
    uint32_t tileMinY = 0;
    uint32_t activeTilesX = 0;
    uint32_t activeTilesY = 0;
    uint32_t activeSuperTilesX = 0;
    uint32_t maxTileItems = 0;
    uint32_t pad0 = 0;
};
static_assert(sizeof(PhaseTileBinPushConstants) == 88, "PhaseTileBinPushConstants size mismatch");

struct PhaseUberPushConstants {
    uint64_t transformsAddr = 0;
    uint64_t clipsAddr = 0;
    uint64_t primitivesAddr = 0;
    uint64_t preparedPrimsAddr = 0;
    uint64_t tileHeadersAddr = 0;
    uint64_t tileItemsAddr = 0;
    uint64_t glyphSlotsAddr = 0;
    float clearR = 0.0f;
    float clearG = 0.0f;
    float clearB = 0.0f;
    float clearA = 1.0f;
    float viewportWidth = 0.0f;
    float viewportHeight = 0.0f;
    float devicePixelRatio = 1.0f;
    float textStemDarkening = 0.0f;
    uint32_t tilesX = 0;
    uint32_t forceOpaqueAlpha = 0;
};
static_assert(sizeof(PhaseUberPushConstants) == 96, "PhaseUberPushConstants size mismatch");

struct GpuGlyphCacheKey {
    uint64_t arenaId = 0;
    uint32_t curveTexelOffset = 0;
    int32_t m00Fixed = 0;
    int32_t m01Fixed = 0;
    int32_t m10Fixed = 0;
    int32_t m11Fixed = 0;
    uint16_t quantDpr = 0;
    uint8_t qSubX = 0;
    uint8_t qSubY = 0;

    bool operator==(const GpuGlyphCacheKey &o) const noexcept
    {
        return arenaId == o.arenaId
                && curveTexelOffset == o.curveTexelOffset
                && m00Fixed == o.m00Fixed
                && m01Fixed == o.m01Fixed
                && m10Fixed == o.m10Fixed
                && m11Fixed == o.m11Fixed
                && quantDpr == o.quantDpr
                && qSubX == o.qSubX
                && qSubY == o.qSubY;
    }
};

struct GpuGlyphCacheKeyHash {
    size_t operator()(const GpuGlyphCacheKey &k) const noexcept
    {
        uint64_t h = k.arenaId * 0x517CC1B727220A95ULL;
        h ^= uint64_t(k.curveTexelOffset) * 0x9E3779B97F4A7C15ULL;
        h ^= (uint64_t(uint32_t(k.m00Fixed)) << 32) | uint64_t(uint32_t(k.m11Fixed));
        h *= 0xBF58476D1CE4E5B9ULL;
        h ^= (uint64_t(uint32_t(k.m01Fixed)) << 32) | uint64_t(uint32_t(k.m10Fixed));
        h ^= (uint64_t(k.quantDpr) << 16) | (uint64_t(k.qSubX) << 8) | uint64_t(k.qSubY);
        h ^= (h >> 31);
        return size_t(h);
    }
};

struct GpuGlyphAtlasEntry {
    uint16_t atlasX = 0;
    uint16_t atlasY = 0;
    uint16_t width = 0;
    uint16_t height = 0;
};

inline VkDeviceSize alignUp(VkDeviceSize val, VkDeviceSize alignment) noexcept
{
    return (val + alignment - 1) & ~(alignment - 1);
}

static QString pipelineCacheFilePath()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty())
        base = QDir::tempPath() + QStringLiteral("/qtphase_cache");
    QDir().mkpath(base);
    return base + QStringLiteral("/qtphase_vk13_pipeline_v6_hardened.cache");
}

// =============================================================================
// Process-Wide Shared Vulkan 1.3 Context (VkDevice + VMA + Pipelines + Glyph Atlas)
// =============================================================================
struct QPhaseSharedVulkanDevice {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    uint32_t graphicsQueueFamily = 0;
    std::mutex queueMutex;
    VmaAllocator allocator = VK_NULL_HANDLE;

    double timestampPeriodNs = 0.0;
    bool supportsTimestamps = false;
    bool hasSwapchainExt = false;
    bool hasIncrementalPresent = false;
    bool hasDmaBufExport = false;
    bool hasSyncFdExport = false;
    bool isHardwareGpu = false;
    std::atomic<bool> pipelinesWarmedUp{false};

    PFN_vkGetMemoryFdKHR pfnGetMemoryFdKHR = nullptr;
    PFN_vkGetImageDrmFormatModifierPropertiesEXT pfnGetImageDrmFormatModifierPropertiesEXT = nullptr;
    PFN_vkGetSemaphoreFdKHR pfnGetSemaphoreFdKHR = nullptr;

    VkSampler linearSampler = VK_NULL_HANDLE;
    VkSampler nearestSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout bindlessSetLayout = VK_NULL_HANDLE;

    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    VkPipelineLayout glyphBakeLayout = VK_NULL_HANDLE;
    VkPipeline glyphBakePipeline = VK_NULL_HANDLE;
    VkPipelineLayout primPrepLayout = VK_NULL_HANDLE;
    VkPipeline primPrepPipeline = VK_NULL_HANDLE;
    VkPipelineLayout tileBinLayout = VK_NULL_HANDLE;
    VkPipeline tileBinPipeline = VK_NULL_HANDLE;
    VkPipelineLayout uberLayout = VK_NULL_HANDLE;
    VkPipeline uberPipeline = VK_NULL_HANDLE;

    VkBuffer dummyBuffer = VK_NULL_HANDLE;
    VmaAllocation dummyAlloc = VK_NULL_HANDLE;
    VkDeviceAddress dummyAddr = 0;

    // Shared process-wide 2048x2048 R8_UNORM scratch curve/glyph atlas (4 MB VRAM across ALL windows!)
    VkImage glyphAtlasImage = VK_NULL_HANDLE;
    VmaAllocation glyphAtlasAlloc = VK_NULL_HANDLE;
    VkImageView glyphAtlasView = VK_NULL_HANDLE;
    std::mutex glyphMutex;
    std::unordered_map<GpuGlyphCacheKey, GpuGlyphAtlasEntry, GpuGlyphCacheKeyHash> glyphCacheMap;
    int atlasShelfX = 1;
    int atlasShelfY = 1;
    int atlasRowHeight = 0;
    std::atomic<uint64_t> atlasGeneration{1};

    ~QPhaseSharedVulkanDevice()
    {
        if (device != VK_NULL_HANDLE) {
            {
                std::lock_guard<std::mutex> qlock(queueMutex);
                vkDeviceWaitIdle(device);
            }

            if (pipelineCache != VK_NULL_HANDLE) {
                size_t dataSize = 0;
                if (vkGetPipelineCacheData(device, pipelineCache, &dataSize, nullptr) == VK_SUCCESS && dataSize > 0) {
                    QByteArray bytes(qsizetype(dataSize), Qt::Uninitialized);
                    if (vkGetPipelineCacheData(device, pipelineCache, &dataSize, bytes.data()) == VK_SUCCESS) {
                        QFile f(pipelineCacheFilePath());
                        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                            f.write(bytes);
                    }
                }
            }

            if (glyphAtlasView != VK_NULL_HANDLE)
                vkDestroyImageView(device, glyphAtlasView, nullptr);
            if (glyphAtlasImage != VK_NULL_HANDLE)
                vmaDestroyImage(allocator, glyphAtlasImage, glyphAtlasAlloc);

            if (dummyBuffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(allocator, dummyBuffer, dummyAlloc);

            if (glyphBakePipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, glyphBakePipeline, nullptr);
            if (glyphBakeLayout != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(device, glyphBakeLayout, nullptr);
            if (primPrepPipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, primPrepPipeline, nullptr);
            if (primPrepLayout != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(device, primPrepLayout, nullptr);
            if (tileBinPipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, tileBinPipeline, nullptr);
            if (tileBinLayout != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(device, tileBinLayout, nullptr);
            if (uberPipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, uberPipeline, nullptr);
            if (uberLayout != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(device, uberLayout, nullptr);
            if (pipelineCache != VK_NULL_HANDLE)
                vkDestroyPipelineCache(device, pipelineCache, nullptr);

            if (bindlessSetLayout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(device, bindlessSetLayout, nullptr);
            if (nearestSampler != VK_NULL_HANDLE)
                vkDestroySampler(device, nearestSampler, nullptr);
            if (linearSampler != VK_NULL_HANDLE)
                vkDestroySampler(device, linearSampler, nullptr);

            if (allocator != VK_NULL_HANDLE)
                vmaDestroyAllocator(allocator);

            vkDestroyDevice(device, nullptr);
        }

        if (instance != VK_NULL_HANDLE)
            vkDestroyInstance(instance, nullptr);
    }

    bool initialize(bool requireWaylandWsi)
    {
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "QtPhase";
        appInfo.applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
        appInfo.pEngineName = "QtPhase";
        appInfo.engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_3;

        // Opportunistically enable Wayland WSI extensions whenever supported by the loader so
        // offscreen + windowed QPhaseVulkanBackends always share one VkInstance/VkDevice!
        bool loaderHasSurface = false;
        bool loaderHasWayland = false;
        uint32_t instExtCount = 0;
        if (vkEnumerateInstanceExtensionProperties(nullptr, &instExtCount, nullptr) == VK_SUCCESS && instExtCount > 0) {
            std::vector<VkExtensionProperties> availInst(instExtCount);
            vkEnumerateInstanceExtensionProperties(nullptr, &instExtCount, availInst.data());
            for (const VkExtensionProperties &ep : availInst) {
                if (std::strcmp(ep.extensionName, VK_KHR_SURFACE_EXTENSION_NAME) == 0)
                    loaderHasSurface = true;
                else if (std::strcmp(ep.extensionName, "VK_KHR_wayland_surface") == 0)
                    loaderHasWayland = true;
            }
        }

        if (requireWaylandWsi && (!loaderHasSurface || !loaderHasWayland)) {
            qWarning("QtPhase: VK_KHR_wayland_surface not supported by Vulkan loader");
            return false;
        }

        std::vector<const char *> instanceExts;
        if (loaderHasSurface && loaderHasWayland) {
            instanceExts.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
            instanceExts.push_back("VK_KHR_wayland_surface");
        }

        VkInstanceCreateInfo instInfo{};
        instInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instInfo.pApplicationInfo = &appInfo;
        instInfo.enabledExtensionCount = uint32_t(instanceExts.size());
        instInfo.ppEnabledExtensionNames = instanceExts.data();

        if (vkCreateInstance(&instInfo, nullptr, &instance) != VK_SUCCESS) {
            if (!requireWaylandWsi && !instanceExts.empty()) {
                instInfo.enabledExtensionCount = 0;
                instInfo.ppEnabledExtensionNames = nullptr;
                if (vkCreateInstance(&instInfo, nullptr, &instance) != VK_SUCCESS)
                    return false;
            } else {
                qWarning("QtPhase: vkCreateInstance (Vulkan 1.3) failed");
                return false;
            }
        }

        uint32_t gpuCount = 0;
        vkEnumeratePhysicalDevices(instance, &gpuCount, nullptr);
        if (gpuCount == 0)
            return false;
        std::vector<VkPhysicalDevice> gpus(gpuCount);
        vkEnumeratePhysicalDevices(instance, &gpuCount, gpus.data());

        physDevice = gpus[0];
        VkPhysicalDeviceProperties chosenProps{};
        vkGetPhysicalDeviceProperties(physDevice, &chosenProps);
        for (VkPhysicalDevice candidate : gpus) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(candidate, &props);
            if (props.apiVersion >= VK_API_VERSION_1_3
                && (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
                    || props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)) {
                physDevice = candidate;
                chosenProps = props;
                break;
            }
        }

        isHardwareGpu = (chosenProps.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
                         || chosenProps.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU);

        uint32_t qfCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &qfCount, nullptr);
        std::vector<VkQueueFamilyProperties> qfProps(qfCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &qfCount, qfProps.data());

        bool foundQueue = false;
        for (uint32_t i = 0; i < qfCount; ++i) {
            if ((qfProps[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
                == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
                graphicsQueueFamily = i;
                supportsTimestamps = (qfProps[i].timestampValidBits > 0)
                        && (chosenProps.limits.timestampPeriod > 0.0f);
                timestampPeriodNs = double(chosenProps.limits.timestampPeriod);
                foundQueue = true;
                break;
            }
        }
        if (!foundQueue)
            return false;

        const float queuePriority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = graphicsQueueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &queuePriority;

        VkPhysicalDeviceVulkan13Features feat13{};
        feat13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        feat13.dynamicRendering = VK_TRUE;
        feat13.synchronization2 = VK_TRUE;

        VkPhysicalDeviceVulkan12Features feat12{};
        feat12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        feat12.pNext = &feat13;
        feat12.bufferDeviceAddress = VK_TRUE;
        feat12.scalarBlockLayout = VK_TRUE;
        feat12.descriptorIndexing = VK_TRUE;
        feat12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        feat12.runtimeDescriptorArray = VK_TRUE;
        feat12.descriptorBindingPartiallyBound = VK_TRUE;
        feat12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        feat12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;

        VkPhysicalDeviceFeatures2 feat2{};
        feat2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        feat2.pNext = &feat12;
        feat2.features.shaderInt64 = VK_TRUE;

        std::vector<const char *> deviceExts;
        bool hasExtMemFd = false;
        bool hasExtMemDmaBuf = false;
        bool hasDrmModifier = false;
        bool hasExtSemFd = false;
        {
            uint32_t extCount = 0;
            if (vkEnumerateDeviceExtensionProperties(physDevice, nullptr, &extCount, nullptr) == VK_SUCCESS && extCount > 0) {
                std::vector<VkExtensionProperties> availExts(extCount);
                vkEnumerateDeviceExtensionProperties(physDevice, nullptr, &extCount, availExts.data());
                for (const VkExtensionProperties &ep : availExts) {
                    if (std::strcmp(ep.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0)
                        hasSwapchainExt = true;
                    else if (std::strcmp(ep.extensionName, VK_KHR_INCREMENTAL_PRESENT_EXTENSION_NAME) == 0)
                        hasIncrementalPresent = true;
                    else if (std::strcmp(ep.extensionName, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) == 0)
                        hasExtMemFd = true;
                    else if (std::strcmp(ep.extensionName, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) == 0)
                        hasExtMemDmaBuf = true;
                    else if (std::strcmp(ep.extensionName, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) == 0)
                        hasDrmModifier = true;
                    else if (std::strcmp(ep.extensionName, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME) == 0)
                        hasExtSemFd = true;
                }
            }
            if (!instanceExts.empty() && hasSwapchainExt) {
                deviceExts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
                if (hasIncrementalPresent)
                    deviceExts.push_back(VK_KHR_INCREMENTAL_PRESENT_EXTENSION_NAME);
            }
            if (hasExtMemFd && hasExtMemDmaBuf && hasDrmModifier) {
                deviceExts.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
                deviceExts.push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
                deviceExts.push_back(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
            }
            if (hasExtSemFd)
                deviceExts.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
        }

        if (requireWaylandWsi && !hasSwapchainExt && !(hasExtMemFd && hasExtMemDmaBuf && hasDrmModifier))
            return false;

        VkDeviceCreateInfo devInfo{};
        devInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        devInfo.pNext = &feat2;
        devInfo.queueCreateInfoCount = 1;
        devInfo.pQueueCreateInfos = &queueInfo;
        devInfo.enabledExtensionCount = uint32_t(deviceExts.size());
        devInfo.ppEnabledExtensionNames = deviceExts.data();

        if (vkCreateDevice(physDevice, &devInfo, nullptr, &device) != VK_SUCCESS) {
            qWarning("QtPhase: vkCreateDevice failed");
            return false;
        }

        if (hasExtMemFd && hasExtMemDmaBuf && hasDrmModifier) {
            pfnGetMemoryFdKHR = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
                    vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR"));
            pfnGetImageDrmFormatModifierPropertiesEXT = reinterpret_cast<PFN_vkGetImageDrmFormatModifierPropertiesEXT>(
                    vkGetDeviceProcAddr(device, "vkGetImageDrmFormatModifierPropertiesEXT"));
            hasDmaBufExport = (pfnGetMemoryFdKHR != nullptr && pfnGetImageDrmFormatModifierPropertiesEXT != nullptr);
        }
        if (hasExtSemFd) {
            pfnGetSemaphoreFdKHR = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
                    vkGetDeviceProcAddr(device, "vkGetSemaphoreFdKHR"));
            if (pfnGetSemaphoreFdKHR) {
                VkPhysicalDeviceExternalSemaphoreInfo esi{};
                esi.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO;
                esi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
                VkExternalSemaphoreProperties esp{};
                esp.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES;
                vkGetPhysicalDeviceExternalSemaphoreProperties(physDevice, &esi, &esp);
                hasSyncFdExport = (esp.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) != 0;
            }
        }

        vkGetDeviceQueue(device, graphicsQueueFamily, 0, &graphicsQueue);

        VmaVulkanFunctions vmaFuncs{};
        vmaFuncs.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
        vmaFuncs.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

        VmaAllocatorCreateInfo allocInfo{};
        allocInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        allocInfo.vulkanApiVersion = VK_API_VERSION_1_3;
        allocInfo.physicalDevice = physDevice;
        allocInfo.device = device;
        allocInfo.instance = instance;
        allocInfo.pVulkanFunctions = &vmaFuncs;

        if (vmaCreateAllocator(&allocInfo, &allocator) != VK_SUCCESS)
            return false;

        // Dummy BDA buffer
        {
            VkBufferCreateInfo bufInfo{};
            bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufInfo.size = 256;
            bufInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

            VmaAllocationCreateInfo vmaCi{};
            vmaCi.usage = VMA_MEMORY_USAGE_AUTO;
            vmaCi.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            if (vmaCreateBuffer(allocator, &bufInfo, &vmaCi, &dummyBuffer, &dummyAlloc, &info) != VK_SUCCESS)
                return false;
            std::memset(info.pMappedData, 0, 256);
            vmaFlushAllocation(allocator, dummyAlloc, 0, 256);

            VkBufferDeviceAddressInfo addrInfo{};
            addrInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
            addrInfo.buffer = dummyBuffer;
            dummyAddr = vkGetBufferDeviceAddress(device, &addrInfo);
        }

        // Shared On-GPU Scratch Glyph/Curve Atlas (2048x2048 R8_UNORM = 4 MB VRAM)
        {
            VkImageCreateInfo atlasInfo{};
            atlasInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            atlasInfo.imageType = VK_IMAGE_TYPE_2D;
            atlasInfo.format = VK_FORMAT_R8_UNORM;
            atlasInfo.extent = { kGlyphAtlasWidth, kGlyphAtlasHeight, 1 };
            atlasInfo.mipLevels = 1;
            atlasInfo.arrayLayers = 1;
            atlasInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            atlasInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            atlasInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

            VmaAllocationCreateInfo atlasVma{};
            atlasVma.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            if (vmaCreateImage(allocator, &atlasInfo, &atlasVma, &glyphAtlasImage, &glyphAtlasAlloc, nullptr) != VK_SUCCESS)
                return false;

            VkImageViewCreateInfo ivInfo{};
            ivInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            ivInfo.image = glyphAtlasImage;
            ivInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            ivInfo.format = VK_FORMAT_R8_UNORM;
            ivInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            if (vkCreateImageView(device, &ivInfo, nullptr, &glyphAtlasView) != VK_SUCCESS)
                return false;

            VkCommandPool initPool = VK_NULL_HANDLE;
            VkCommandPoolCreateInfo cpInfo{};
            cpInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            cpInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            cpInfo.queueFamilyIndex = graphicsQueueFamily;
            vkCreateCommandPool(device, &cpInfo, nullptr, &initPool);

            VkCommandBuffer cmd = VK_NULL_HANDLE;
            VkCommandBufferAllocateInfo cbAlloc{};
            cbAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cbAlloc.commandPool = initPool;
            cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cbAlloc.commandBufferCount = 1;
            vkAllocateCommandBuffers(device, &cbAlloc, &cmd);

            VkCommandBufferBeginInfo beginInfo{};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &beginInfo);

            VkImageMemoryBarrier2 initBarrier{};
            initBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            initBarrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            initBarrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            initBarrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            initBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            initBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            initBarrier.image = glyphAtlasImage;
            initBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &initBarrier;
            vkCmdPipelineBarrier2(cmd, &dep);
            vkEndCommandBuffer(cmd);

            VkFence initFence = VK_NULL_HANDLE;
            VkFenceCreateInfo fInfo{};
            fInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            vkCreateFence(device, &fInfo, nullptr, &initFence);

            VkCommandBufferSubmitInfo cmdSub{};
            cmdSub.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
            cmdSub.commandBuffer = cmd;
            VkSubmitInfo2 sub{};
            sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
            sub.commandBufferInfoCount = 1;
            sub.pCommandBufferInfos = &cmdSub;
            {
                std::lock_guard<std::mutex> qlock(queueMutex);
                vkQueueSubmit2(graphicsQueue, 1, &sub, initFence);
            }
            vkWaitForFences(device, 1, &initFence, VK_TRUE, UINT64_MAX);
            vkDestroyFence(device, initFence, nullptr);
            vkDestroyCommandPool(device, initPool, nullptr);

            glyphCacheMap.reserve(4096);
        }

        // Samplers & Bindless Set Layout
        {
            VkSamplerCreateInfo linInfo{};
            linInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            linInfo.magFilter = VK_FILTER_LINEAR;
            linInfo.minFilter = VK_FILTER_LINEAR;
            linInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            linInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            linInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            linInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            if (vkCreateSampler(device, &linInfo, nullptr, &linearSampler) != VK_SUCCESS)
                return false;

            VkSamplerCreateInfo nearInfo = linInfo;
            nearInfo.magFilter = VK_FILTER_NEAREST;
            nearInfo.minFilter = VK_FILTER_NEAREST;
            if (vkCreateSampler(device, &nearInfo, nullptr, &nearestSampler) != VK_SUCCESS)
                return false;

            const VkDescriptorBindingFlags bindFlags[3] = {
                VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT
                        | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT
                        | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
                0,
                0
            };

            VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
            flagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
            flagsInfo.bindingCount = 3;
            flagsInfo.pBindingFlags = bindFlags;

            VkDescriptorSetLayoutBinding bindings[3]{};
            bindings[0].binding = 0;
            bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[0].descriptorCount = kMaxBindlessTextures;
            bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            bindings[1].binding = 1;
            bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[1].descriptorCount = 1;
            bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

            bindings[2].binding = 2;
            bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[2].descriptorCount = 1;
            bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            VkDescriptorSetLayoutCreateInfo dslInfo{};
            dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            dslInfo.pNext = &flagsInfo;
            dslInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
            dslInfo.bindingCount = 3;
            dslInfo.pBindings = bindings;
            if (vkCreateDescriptorSetLayout(device, &dslInfo, nullptr, &bindlessSetLayout) != VK_SUCCESS)
                return false;
        }

        QByteArray cachedData;
        {
            QFile cacheFile(pipelineCacheFilePath());
            if (cacheFile.open(QIODevice::ReadOnly))
                cachedData = cacheFile.readAll();
        }

        VkPipelineCacheCreateInfo cacheInfo{};
        cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        cacheInfo.initialDataSize = size_t(cachedData.size());
        cacheInfo.pInitialData = cachedData.isEmpty() ? nullptr : cachedData.constData();
        vkCreatePipelineCache(device, &cacheInfo, nullptr, &pipelineCache);

        auto makePipelineLayout = [&](VkShaderStageFlags stages, uint32_t pcBytes, VkPipelineLayout *outLayout) -> bool {
            VkPushConstantRange pcRange{};
            pcRange.stageFlags = stages;
            pcRange.offset = 0;
            pcRange.size = pcBytes;

            VkPipelineLayoutCreateInfo plInfo{};
            plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            plInfo.setLayoutCount = 1;
            plInfo.pSetLayouts = &bindlessSetLayout;
            plInfo.pushConstantRangeCount = 1;
            plInfo.pPushConstantRanges = &pcRange;
            return vkCreatePipelineLayout(device, &plInfo, nullptr, outLayout) == VK_SUCCESS;
        };

        if (!makePipelineLayout(VK_SHADER_STAGE_COMPUTE_BIT, sizeof(PhaseGlyphBakePushConstants), &glyphBakeLayout))
            return false;
        if (!makePipelineLayout(VK_SHADER_STAGE_COMPUTE_BIT, sizeof(PhasePrimPrepPushConstants), &primPrepLayout))
            return false;
        if (!makePipelineLayout(VK_SHADER_STAGE_COMPUTE_BIT, sizeof(PhaseTileBinPushConstants), &tileBinLayout))
            return false;
        if (!makePipelineLayout(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                sizeof(PhaseUberPushConstants), &uberLayout)) {
            return false;
        }

        // Batched creation of all 3 compute pipelines: glyph_bake + prim_prep + tile_bin!
        {
            VkShaderModule bakeMod = VK_NULL_HANDLE;
            VkShaderModule prepMod = VK_NULL_HANDLE;
            VkShaderModule binMod = VK_NULL_HANDLE;

            VkShaderModuleCreateInfo smBake{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
                                             kPhaseGlyphBakeCompSpirvSizeBytes, kPhaseGlyphBakeCompSpirv };
            VkShaderModuleCreateInfo smPrep{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
                                             kPhasePrimPrepCompSpirvSizeBytes, kPhasePrimPrepCompSpirv };
            VkShaderModuleCreateInfo smBin{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
                                            kPhaseTileBinCompSpirvSizeBytes, kPhaseTileBinCompSpirv };

            const bool modsOk = (vkCreateShaderModule(device, &smBake, nullptr, &bakeMod) == VK_SUCCESS)
                    && (vkCreateShaderModule(device, &smPrep, nullptr, &prepMod) == VK_SUCCESS)
                    && (vkCreateShaderModule(device, &smBin, nullptr, &binMod) == VK_SUCCESS);
            if (!modsOk) {
                if (binMod) vkDestroyShaderModule(device, binMod, nullptr);
                if (prepMod) vkDestroyShaderModule(device, prepMod, nullptr);
                if (bakeMod) vkDestroyShaderModule(device, bakeMod, nullptr);
                return false;
            }

            VkComputePipelineCreateInfo cpInfos[3]{};
            cpInfos[0].sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            cpInfos[0].stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                 VK_SHADER_STAGE_COMPUTE_BIT, bakeMod, "main", nullptr };
            cpInfos[0].layout = glyphBakeLayout;

            cpInfos[1].sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            cpInfos[1].stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                 VK_SHADER_STAGE_COMPUTE_BIT, prepMod, "main", nullptr };
            cpInfos[1].layout = primPrepLayout;

            cpInfos[2].sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            cpInfos[2].stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                 VK_SHADER_STAGE_COMPUTE_BIT, binMod, "main", nullptr };
            cpInfos[2].layout = tileBinLayout;

            VkPipeline builtComp[3]{ VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
            const VkResult compRes = vkCreateComputePipelines(device, pipelineCache, 3, cpInfos, nullptr, builtComp);
            glyphBakePipeline = builtComp[0];
            primPrepPipeline = builtComp[1];
            tileBinPipeline = builtComp[2];

            vkDestroyShaderModule(device, binMod, nullptr);
            vkDestroyShaderModule(device, prepMod, nullptr);
            vkDestroyShaderModule(device, bakeMod, nullptr);
            if (compRes != VK_SUCCESS)
                return false;
        }

        // Single-Pass Fullscreen Tile Graphics Pipeline (0 Bezier loops, 0 MB depth buffer!)
        {
            VkShaderModule vertMod = VK_NULL_HANDLE;
            VkShaderModule fragMod = VK_NULL_HANDLE;

            VkShaderModuleCreateInfo smVert{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
                                             kPhaseVertSpirvSizeBytes, kPhaseVertSpirv };
            VkShaderModuleCreateInfo smFrag{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
                                             kPhaseFragSpirvSizeBytes, kPhaseFragSpirv };
            if (vkCreateShaderModule(device, &smVert, nullptr, &vertMod) != VK_SUCCESS)
                return false;
            if (vkCreateShaderModule(device, &smFrag, nullptr, &fragMod) != VK_SUCCESS) {
                vkDestroyShaderModule(device, vertMod, nullptr);
                return false;
            }

            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                          VK_SHADER_STAGE_VERTEX_BIT, vertMod, "main", nullptr };
            stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                          VK_SHADER_STAGE_FRAGMENT_BIT, fragMod, "main", nullptr };

            VkPipelineVertexInputStateCreateInfo viState{};
            viState.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

            VkPipelineInputAssemblyStateCreateInfo iaState{};
            iaState.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            iaState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

            VkPipelineViewportStateCreateInfo vpState{};
            vpState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            vpState.viewportCount = 1;
            vpState.scissorCount = 1;

            VkPipelineRasterizationStateCreateInfo rsState{};
            rsState.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rsState.polygonMode = VK_POLYGON_MODE_FILL;
            rsState.cullMode = VK_CULL_MODE_NONE;
            rsState.frontFace = VK_FRONT_FACE_CLOCKWISE;
            rsState.lineWidth = 1.0f;

            VkPipelineMultisampleStateCreateInfo msState{};
            msState.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            msState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

            VkPipelineDepthStencilStateCreateInfo dsState{};
            dsState.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

            VkPipelineColorBlendAttachmentState cbAtt{};
            cbAtt.blendEnable = VK_FALSE;
            cbAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                    | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

            VkPipelineColorBlendStateCreateInfo cbState{};
            cbState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            cbState.attachmentCount = 1;
            cbState.pAttachments = &cbAtt;

            const VkDynamicState dynStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
            VkPipelineDynamicStateCreateInfo dynInfo{};
            dynInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            dynInfo.dynamicStateCount = 2;
            dynInfo.pDynamicStates = dynStates;

            const VkFormat colorFormat = VK_FORMAT_B8G8R8A8_UNORM;
            VkPipelineRenderingCreateInfo renderingInfo{};
            renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
            renderingInfo.colorAttachmentCount = 1;
            renderingInfo.pColorAttachmentFormats = &colorFormat;
            renderingInfo.depthAttachmentFormat = VK_FORMAT_UNDEFINED;

            VkGraphicsPipelineCreateInfo gpInfo{};
            gpInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            gpInfo.pNext = &renderingInfo;
            gpInfo.stageCount = 2;
            gpInfo.pStages = stages;
            gpInfo.pVertexInputState = &viState;
            gpInfo.pInputAssemblyState = &iaState;
            gpInfo.pViewportState = &vpState;
            gpInfo.pRasterizationState = &rsState;
            gpInfo.pMultisampleState = &msState;
            gpInfo.pDepthStencilState = &dsState;
            gpInfo.pColorBlendState = &cbState;
            gpInfo.pDynamicState = &dynInfo;
            gpInfo.layout = uberLayout;

            const VkResult pipeRes = vkCreateGraphicsPipelines(
                    device, pipelineCache, 1, &gpInfo, nullptr, &uberPipeline);

            vkDestroyShaderModule(device, fragMod, nullptr);
            vkDestroyShaderModule(device, vertMod, nullptr);
            if (pipeRes != VK_SUCCESS)
                return false;
        }

        return true;
    }

    static std::mutex &sharedMutex()
    {
        static std::mutex s_mutex;
        return s_mutex;
    }

    static std::weak_ptr<QPhaseSharedVulkanDevice> &sharedWeak()
    {
        static std::weak_ptr<QPhaseSharedVulkanDevice> s_weak;
        return s_weak;
    }

    static bool isHardwareWsiWarm() noexcept
    {
        std::lock_guard<std::mutex> lock(sharedMutex());
        if (auto existing = sharedWeak().lock())
            return (existing->hasSwapchainExt || existing->hasDmaBufExport) && existing->isHardwareGpu;
        return false;
    }

    static std::shared_ptr<QPhaseSharedVulkanDevice> acquire(bool requireWaylandWsi, bool *outCacheHit)
    {
        std::lock_guard<std::mutex> lock(sharedMutex());
        if (auto existing = sharedWeak().lock()) {
            if (!requireWaylandWsi || existing->hasSwapchainExt || existing->hasDmaBufExport) {
                if (outCacheHit)
                    *outCacheHit = true;
                return existing;
            }
        }

        if (outCacheHit)
            *outCacheHit = false;
        auto fresh = std::make_shared<QPhaseSharedVulkanDevice>();
        if (!fresh->initialize(requireWaylandWsi))
            return nullptr;
        sharedWeak() = fresh;
        return fresh;
    }
};

struct GpuUploadedTexture {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImage depthImage = VK_NULL_HANDLE;
    VmaAllocation depthAlloc = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;
    uint64_t generation = 0;
    uint64_t renderedGpuRev = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool hasDepth = false;
};

struct PendingGpuSurfacePass {
    VkImage colorImage = VK_NULL_HANDLE;
    VkImageView colorView = VK_NULL_HANDLE;
    VkImage depthImage = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    std::shared_ptr<PhaseGpuSurfaceCallback> callback;
};

struct RetiredGpuBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
    uint64_t retireAfterSeq = 0;
};

struct RetiredGpuImage {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint64_t retireAfterSeq = 0;
};

struct PendingGpuImageCopy {
    VkImage dstImage = VK_NULL_HANDLE;
    VkDeviceSize stagingOffset = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct FrameSlot {
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
    VkSemaphore imageAvailableSem = VK_NULL_HANDLE;
    VkFence inFlightFence = VK_NULL_HANDLE;
    uint64_t submittedSeq = 0;

    // Per-slot mapped staging buffer for zero-stall batched image uploads inside f.cmdBuf!
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = VK_NULL_HANDLE;
    VmaAllocationInfo stagingAllocInfo{};
    VkDeviceSize stagingCapacity = 0;
    std::vector<PendingGpuImageCopy> pendingImageCopies;
    std::vector<PendingGpuSurfacePass> pendingGpuSurfacePasses;

    // Persistently mapped UMA / ReBAR scene buffer with granular sub-table capacities
    VkBuffer sceneBuffer = VK_NULL_HANDLE;
    VmaAllocation sceneAlloc = VK_NULL_HANDLE;
    VmaAllocationInfo sceneAllocInfo{};
    VkDeviceSize sceneCapacity = 0;
    VkDeviceAddress sceneBaseAddr = 0;

    VkDeviceSize txOffset = 0, txCapBytes = 0;
    VkDeviceSize clOffset = 0, clCapBytes = 0;
    VkDeviceSize prOffset = 0, prCapBytes = 0;
    VkDeviceSize doOffset = 0, doCapBytes = 0;
    VkDeviceSize gsOffset = 0, gsCapBytes = 0;
    VkDeviceSize bkOffset = 0, bkCapBytes = 0;
    VkDeviceSize bbOffset = 0, bbCapBytes = 0;

    VkDeviceAddress transformsAddr = 0;
    VkDeviceAddress clipsAddr = 0;
    VkDeviceAddress primitivesAddr = 0;
    VkDeviceAddress drawOrderAddr = 0;
    VkDeviceAddress glyphSlotsAddr = 0;
    VkDeviceAddress bakeCmdsAddr = 0;
    VkDeviceAddress bakeBlocksAddr = 0;

    uint32_t drawCount = 0;
    uint32_t bakeBlockCount = 0;

    uint64_t syncedRevision = 0;
    uint64_t syncedTxClipRev = 0;
    uint64_t syncedPrimRev = 0;
    uint64_t syncedOrderRev = 0;
    uint64_t syncedAtlasGen = 0;
    qreal syncedDpr = 0.0;

    // Dynamically sized pure DEVICE_LOCAL 2-stage binning work buffer (~79 KB on popups!)
    VkBuffer workBuffer = VK_NULL_HANDLE;
    VmaAllocation workAlloc = VK_NULL_HANDLE;
    VkDeviceSize counterOffset = 0;
    VkDeviceSize superTileMasksOffset = 0;
    VkDeviceAddress tileCounterAddr = 0;
    VkDeviceAddress tileHeadersAddr = 0;
    VkDeviceAddress tileItemsAddr = 0;
    VkDeviceAddress preparedPrimsAddr = 0;
    VkDeviceAddress superTileMasksAddr = 0;

    uint32_t capTiles = 0;
    uint32_t capSuperTiles = 0;
    uint32_t capDrawCount = 0;
    uint32_t capMaskWords = 0;
    uint32_t capTileItems = 0;

    bool hasValidTimestamps = false;
};

} // namespace

struct QPhaseVulkanBackendData {
    std::shared_ptr<QPhaseSharedVulkanDevice> shared;
    bool usedSharedCache = false;

    VkQueryPool timestampQueryPool = VK_NULL_HANDLE;
    qint64 lastCompletedGpuHwUs = 0;

    // Per-backend bindless descriptor set (isolated 4096 image slots + shared glyph atlas bindings 1 & 2!)
    VkDescriptorPool bindlessPool = VK_NULL_HANDLE;
    VkDescriptorSet bindlessSet = VK_NULL_HANDLE;
    std::vector<GpuUploadedTexture> uploadedTextures;
    uint64_t syncedImageTableRev = 0;
    uint64_t maxUploadedImageGen = 0;

    // Fence-timeline retirement queues (zero steady-state vkDeviceWaitIdle!)
    struct RetiredSwapchain {
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        std::vector<VkImageView> views;
        std::vector<VkSemaphore> sems;
        uint64_t retireAfterSeq = 0;
    };
    struct DmaBufRingSlot {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkSemaphore exportSyncSem = VK_NULL_HANDLE;
        int dmabufFd = -1;
        void *wlBufferHandle = nullptr;
        QSize bucketSize;
        bool hasAlpha = false;
        bool initializedLayout = false;
        uint64_t lastSubmitSeq = 0;
        QPhaseDamageTracker damageTracker;
    };
    struct RetiredDmaBuf {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkSemaphore exportSyncSem = VK_NULL_HANDLE;
        int dmabufFd = -1;
        uint64_t retireAfterSeq = 0;
    };
    uint64_t nextSubmitSeq = 0;
    uint64_t completedSubmitSeq = 0;
    std::vector<RetiredGpuBuffer> retiredBuffers;
    std::vector<RetiredGpuImage> retiredImages;
    std::vector<RetiredSwapchain> retiredSwapchains;
    std::vector<RetiredDmaBuf> retiredDmaBufs;

    // Zero-swapchain bucketed DMA-BUF ring for atomic main-thread Wayland presentation!
    std::vector<DmaBufRingSlot> dmabufRing;
    QSize dmabufBucketSize;
    bool dmabufHasAlpha = false;
    bool dmabufModifierCached = false;
    bool dmabufModifierHasAlpha = false;
    uint64_t dmabufChosenModifier = 0;

    std::atomic<quint64> glyphCacheHits{0};
    std::atomic<quint64> glyphCacheMisses{0};
    std::vector<GpuGlyphSlot> scratchGlyphSlots;
    std::vector<GpuGlyphBakeCmd> scratchBakeCmds;
    std::vector<GpuGlyphBakeBlock> scratchBakeBlocks;
    std::unique_ptr<QPhaseCpuRasterizer> cpuBridgeRasterizer;

    // Persistent append-only hb-gpu curve BDA buffer
    VkBuffer curveBuffer = VK_NULL_HANDLE;
    VmaAllocation curveAlloc = VK_NULL_HANDLE;
    VmaAllocationInfo curveAllocInfo{};
    VkDeviceSize curveCapacityBytes = 0;
    VkDeviceAddress curveBaseAddr = 0;
    size_t syncedCurveTexelCount = 0;
    uint64_t boundArenaId = 0;

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkExtent2D swapchainExtent{0, 0};
    bool swapchainHasAlpha = false;
    bool surfaceCapsCached = false;
    bool swapchainNeedsRecreate = false;
    VkSurfaceCapabilitiesKHR cachedSurfaceCaps{};
    VkPresentModeKHR cachedPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainViews;
    std::vector<VkSemaphore> renderFinishedSems;
    std::vector<QPhaseDamageTracker> swapchainDamageTrackers;
    QPhaseDamageTracker presentSurfaceTracker;

    // Reusable headless VRAM render target used during pre-surface worker race
    VkImage raceImage = VK_NULL_HANDLE;
    VmaAllocation raceAlloc = VK_NULL_HANDLE;
    VkImageView raceView = VK_NULL_HANDLE;
    VkExtent2D raceExtent{0, 0};
    QPhaseDamageTracker raceDamageTracker;

    FrameSlot frames[kFramesInFlight]{};
    uint32_t currentFrame = 0;
    bool deviceReady = false;
};

static void reclaimCompletedResources(QPhaseVulkanBackendData *d)
{
    if (!d->shared)
        return;
    VkDevice dev = d->shared->device;
    VmaAllocator alloc = d->shared->allocator;

    auto bufIt = d->retiredBuffers.begin();
    while (bufIt != d->retiredBuffers.end()) {
        if (bufIt->retireAfterSeq <= d->completedSubmitSeq) {
            if (bufIt->buffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(alloc, bufIt->buffer, bufIt->alloc);
            bufIt = d->retiredBuffers.erase(bufIt);
        } else {
            ++bufIt;
        }
    }

    auto imgIt = d->retiredImages.begin();
    while (imgIt != d->retiredImages.end()) {
        if (imgIt->retireAfterSeq <= d->completedSubmitSeq) {
            if (imgIt->view != VK_NULL_HANDLE)
                vkDestroyImageView(dev, imgIt->view, nullptr);
            if (imgIt->image != VK_NULL_HANDLE)
                vmaDestroyImage(alloc, imgIt->image, imgIt->alloc);
            imgIt = d->retiredImages.erase(imgIt);
        } else {
            ++imgIt;
        }
    }

    auto scIt = d->retiredSwapchains.begin();
    while (scIt != d->retiredSwapchains.end()) {
        if (scIt->retireAfterSeq <= d->completedSubmitSeq) {
            for (VkImageView v : scIt->views) {
                if (v != VK_NULL_HANDLE)
                    vkDestroyImageView(dev, v, nullptr);
            }
            for (VkSemaphore sem : scIt->sems) {
                if (sem != VK_NULL_HANDLE)
                    vkDestroySemaphore(dev, sem, nullptr);
            }
            if (scIt->swapchain != VK_NULL_HANDLE)
                vkDestroySwapchainKHR(dev, scIt->swapchain, nullptr);
            scIt = d->retiredSwapchains.erase(scIt);
        } else {
            ++scIt;
        }
    }

    auto dbIt = d->retiredDmaBufs.begin();
    while (dbIt != d->retiredDmaBufs.end()) {
        if (dbIt->retireAfterSeq <= d->completedSubmitSeq) {
            if (dbIt->view != VK_NULL_HANDLE)
                vkDestroyImageView(dev, dbIt->view, nullptr);
            if (dbIt->image != VK_NULL_HANDLE)
                vkDestroyImage(dev, dbIt->image, nullptr);
            if (dbIt->memory != VK_NULL_HANDLE)
                vkFreeMemory(dev, dbIt->memory, nullptr);
            if (dbIt->exportSyncSem != VK_NULL_HANDLE)
                vkDestroySemaphore(dev, dbIt->exportSyncSem, nullptr);
#ifdef Q_OS_LINUX
            if (dbIt->dmabufFd >= 0)
                ::close(dbIt->dmabufFd);
#endif
            dbIt = d->retiredDmaBufs.erase(dbIt);
        } else {
            ++dbIt;
        }
    }
}

static void waitAllBackendFramesIdle(QPhaseVulkanBackendData *d)
{
    if (!d->shared || !d->deviceReady)
        return;
    VkFence fences[kFramesInFlight]{};
    uint32_t count = 0;
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (d->frames[i].inFlightFence != VK_NULL_HANDLE)
            fences[count++] = d->frames[i].inFlightFence;
    }
    if (count > 0)
        vkWaitForFences(d->shared->device, count, fences, VK_TRUE, UINT64_MAX);
    d->completedSubmitSeq = d->nextSubmitSeq;
    reclaimCompletedResources(d);
}

QPhaseVulkanBackend::QPhaseVulkanBackend()
    : d(std::make_unique<QPhaseVulkanBackendData>())
{
}

QPhaseVulkanBackend::~QPhaseVulkanBackend()
{
    if (d->shared && d->shared->device != VK_NULL_HANDLE) {
        waitAllBackendFramesIdle(d.get());
        VkDevice dev = d->shared->device;
        VmaAllocator alloc = d->shared->allocator;

        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            FrameSlot &f = d->frames[i];
            if (f.workBuffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(alloc, f.workBuffer, f.workAlloc);
            if (f.sceneBuffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(alloc, f.sceneBuffer, f.sceneAlloc);
            if (f.stagingBuffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(alloc, f.stagingBuffer, f.stagingAlloc);
            if (f.imageAvailableSem != VK_NULL_HANDLE)
                vkDestroySemaphore(dev, f.imageAvailableSem, nullptr);
            if (f.inFlightFence != VK_NULL_HANDLE)
                vkDestroyFence(dev, f.inFlightFence, nullptr);
            if (f.cmdPool != VK_NULL_HANDLE)
                vkDestroyCommandPool(dev, f.cmdPool, nullptr);
        }

        if (d->timestampQueryPool != VK_NULL_HANDLE)
            vkDestroyQueryPool(dev, d->timestampQueryPool, nullptr);

        for (const GpuUploadedTexture &tex : d->uploadedTextures) {
            if (tex.depthView != VK_NULL_HANDLE)
                vkDestroyImageView(dev, tex.depthView, nullptr);
            if (tex.depthImage != VK_NULL_HANDLE)
                vmaDestroyImage(alloc, tex.depthImage, tex.depthAlloc);
            if (tex.view != VK_NULL_HANDLE)
                vkDestroyImageView(dev, tex.view, nullptr);
            if (tex.image != VK_NULL_HANDLE)
                vmaDestroyImage(alloc, tex.image, tex.alloc);
        }
        d->uploadedTextures.clear();

        if (d->curveBuffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(alloc, d->curveBuffer, d->curveAlloc);

        if (d->bindlessPool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(dev, d->bindlessPool, nullptr);

        for (VkSemaphore sem : d->renderFinishedSems)
            vkDestroySemaphore(dev, sem, nullptr);
        for (VkImageView view : d->swapchainViews)
            vkDestroyImageView(dev, view, nullptr);
        if (d->swapchain != VK_NULL_HANDLE)
            vkDestroySwapchainKHR(dev, d->swapchain, nullptr);

        if (d->raceView != VK_NULL_HANDLE)
            vkDestroyImageView(dev, d->raceView, nullptr);
        if (d->raceImage != VK_NULL_HANDLE)
            vmaDestroyImage(alloc, d->raceImage, d->raceAlloc);

        for (auto &slot : d->dmabufRing) {
            if (slot.view != VK_NULL_HANDLE)
                vkDestroyImageView(dev, slot.view, nullptr);
            if (slot.image != VK_NULL_HANDLE)
                vkDestroyImage(dev, slot.image, nullptr);
            if (slot.memory != VK_NULL_HANDLE)
                vkFreeMemory(dev, slot.memory, nullptr);
            if (slot.exportSyncSem != VK_NULL_HANDLE)
                vkDestroySemaphore(dev, slot.exportSyncSem, nullptr);
#ifdef Q_OS_LINUX
            if (slot.dmabufFd >= 0)
                ::close(slot.dmabufFd);
#endif
        }
        d->dmabufRing.clear();

        if (d->surface != VK_NULL_HANDLE)
            vkDestroySurfaceKHR(d->shared->instance, d->surface, nullptr);
    }
}

bool QPhaseVulkanBackend::isSharedHardwareDeviceReady() noexcept
{
    return QPhaseSharedVulkanDevice::isHardwareWsiWarm();
}

bool QPhaseVulkanBackend::isDeviceReady() const noexcept
{
    return d->deviceReady;
}

bool QPhaseVulkanBackend::isHardwareGpu() const noexcept
{
    return d->shared && d->shared->isHardwareGpu;
}

bool QPhaseVulkanBackend::supportsDmaBufExport() const noexcept
{
    return d->shared && d->shared->hasDmaBufExport;
}

bool QPhaseVulkanBackend::usedSharedDeviceCache() const noexcept
{
    return d->usedSharedCache;
}

quint64 QPhaseVulkanBackend::sharedDeviceHandle() const noexcept
{
    return d->shared ? quint64(reinterpret_cast<uintptr_t>(d->shared->device)) : 0;
}

uint32_t QPhaseVulkanBackend::sharedDeviceUseCount() const noexcept
{
    return d->shared ? uint32_t(d->shared.use_count()) : 0;
}

quint64 QPhaseVulkanBackend::glyphCacheHits() const noexcept
{
    return d->glyphCacheHits.load(std::memory_order_relaxed);
}

quint64 QPhaseVulkanBackend::glyphCacheMisses() const noexcept
{
    return d->glyphCacheMisses.load(std::memory_order_relaxed);
}

bool QPhaseVulkanBackend::initializeDevice(bool enableWaylandWsi)
{
    if (d->deviceReady)
        return true;

    bool cacheHit = false;
    d->shared = QPhaseSharedVulkanDevice::acquire(enableWaylandWsi, &cacheHit);
    if (!d->shared)
        return false;
    d->usedSharedCache = cacheHit;

    VkDevice dev = d->shared->device;

    if (d->shared->supportsTimestamps) {
        VkQueryPoolCreateInfo qpInfo{};
        qpInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qpInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpInfo.queryCount = kFramesInFlight * 2;
        vkCreateQueryPool(dev, &qpInfo, nullptr, &d->timestampQueryPool);
    }

    // Per-backend descriptor set: isolated bindless texture table + shared process-wide glyph atlas!
    {
        VkDescriptorPoolSize poolSizes[2]{};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[0].descriptorCount = kMaxBindlessTextures + 1;
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        poolSizes[1].descriptorCount = 1;

        VkDescriptorPoolCreateInfo dpInfo{};
        dpInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        dpInfo.maxSets = 1;
        dpInfo.poolSizeCount = 2;
        dpInfo.pPoolSizes = poolSizes;
        if (vkCreateDescriptorPool(dev, &dpInfo, nullptr, &d->bindlessPool) != VK_SUCCESS)
            return false;

        VkDescriptorSetAllocateInfo dsAlloc{};
        dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsAlloc.descriptorPool = d->bindlessPool;
        dsAlloc.descriptorSetCount = 1;
        dsAlloc.pSetLayouts = &d->shared->bindlessSetLayout;
        if (vkAllocateDescriptorSets(dev, &dsAlloc, &d->bindlessSet) != VK_SUCCESS)
            return false;

        VkDescriptorImageInfo storageAtlasInfo{};
        storageAtlasInfo.imageView = d->shared->glyphAtlasView;
        storageAtlasInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo sampledAtlasInfo{};
        sampledAtlasInfo.sampler = d->shared->nearestSampler;
        sampledAtlasInfo.imageView = d->shared->glyphAtlasView;
        sampledAtlasInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet atlasWrites[2]{};
        atlasWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        atlasWrites[0].dstSet = d->bindlessSet;
        atlasWrites[0].dstBinding = 1;
        atlasWrites[0].descriptorCount = 1;
        atlasWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        atlasWrites[0].pImageInfo = &storageAtlasInfo;

        atlasWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        atlasWrites[1].dstSet = d->bindlessSet;
        atlasWrites[1].dstBinding = 2;
        atlasWrites[1].descriptorCount = 1;
        atlasWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        atlasWrites[1].pImageInfo = &sampledAtlasInfo;

        vkUpdateDescriptorSets(dev, 2, atlasWrites, 0, nullptr);
    }

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        FrameSlot &f = d->frames[i];
        VkCommandPoolCreateInfo cpInfo{};
        cpInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpInfo.queueFamilyIndex = d->shared->graphicsQueueFamily;
        if (vkCreateCommandPool(dev, &cpInfo, nullptr, &f.cmdPool) != VK_SUCCESS)
            return false;

        VkCommandBufferAllocateInfo cbAlloc{};
        cbAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbAlloc.commandPool = f.cmdPool;
        cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbAlloc.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(dev, &cbAlloc, &f.cmdBuf) != VK_SUCCESS)
            return false;

        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(dev, &semInfo, nullptr, &f.imageAvailableSem);

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(dev, &fenceInfo, nullptr, &f.inFlightFence);
    }

    d->deviceReady = true;

    // Warm up compute & graphics pipelines ONCE per process (secondary windows skip warmup -> < 0.2 ms init!)
    if (!d->shared->pipelinesWarmedUp.exchange(true, std::memory_order_acq_rel)) {
        QPhaseSceneArena warmupArena;
        PhaseGpuPrimitive warmPrim{};
        warmPrim.kind = uint32_t(PhasePrimKind::RoundedBox);
        warmPrim.w = 8.0f;
        warmPrim.h = 8.0f;
        warmPrim.c0a = 1.0f;
        const uint32_t pid = warmupArena.allocatePrimitive(warmPrim);
        warmupArena.setDrawOrder({pid});
        benchmarkFrameOffscreen(warmupArena, QSize(16, 16), 1.0, false, 0, nullptr);
        releaseRaceResources();
        d->boundArenaId = 0;
        d->lastCompletedGpuHwUs = 0;
        for (uint32_t i = 0; i < kFramesInFlight; ++i)
            d->frames[i].hasValidTimestamps = false;
    }

    return true;
}

static bool syncArenaImagesAndCurves(QPhaseVulkanBackendData *d,
                                     FrameSlot &f,
                                     QPhaseSceneArena &arena,
                                     qreal devicePixelRatio)
{
    VkDevice dev = d->shared->device;
    VmaAllocator alloc = d->shared->allocator;

    if (d->boundArenaId != arena.arenaId()) {
        for (GpuUploadedTexture &tex : d->uploadedTextures) {
            if (tex.depthImage != VK_NULL_HANDLE || tex.depthView != VK_NULL_HANDLE)
                d->retiredImages.push_back({ tex.depthImage, tex.depthAlloc, tex.depthView, d->nextSubmitSeq });
            if (tex.image != VK_NULL_HANDLE || tex.view != VK_NULL_HANDLE)
                d->retiredImages.push_back({ tex.image, tex.alloc, tex.view, d->nextSubmitSeq });
        }
        d->uploadedTextures.clear();
        d->syncedCurveTexelCount = 0;
        d->syncedImageTableRev = 0;
        d->maxUploadedImageGen = 0;
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            FrameSlot &slot = d->frames[i];
            slot.syncedRevision = 0;
            slot.syncedTxClipRev = 0;
            slot.syncedPrimRev = 0;
            slot.syncedOrderRev = 0;
            slot.syncedAtlasGen = 0;
        }
        d->boundArenaId = arena.arenaId();
    }

    // 1. Zero-stall append-only sync of HarfBuzz 15 hb-gpu curve arena texels (deferred buffer retirement!)
    const auto &curveTexels = arena.curveTexels();
    if (curveTexels.size() > d->syncedCurveTexelCount) {
        const VkDeviceSize neededBytes = VkDeviceSize(curveTexels.size()) * sizeof(uint64_t);
        if (neededBytes > d->curveCapacityBytes) {
            if (d->curveBuffer != VK_NULL_HANDLE) {
                d->retiredBuffers.push_back({ d->curveBuffer, d->curveAlloc, d->nextSubmitSeq });
                d->curveBuffer = VK_NULL_HANDLE;
            }
            const VkDeviceSize newCap = std::max<VkDeviceSize>(neededBytes * 2, 256 * 1024);

            VkBufferCreateInfo bInfo{};
            bInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bInfo.size = newCap;
            bInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

            VmaAllocationCreateInfo vmaCi{};
            vmaCi.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            vmaCi.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                    | VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT
                    | VMA_ALLOCATION_CREATE_MAPPED_BIT;

            if (vmaCreateBuffer(alloc, &bInfo, &vmaCi, &d->curveBuffer, &d->curveAlloc, &d->curveAllocInfo) != VK_SUCCESS)
                return false;

            d->curveCapacityBytes = newCap;
            d->syncedCurveTexelCount = 0;

            VkBufferDeviceAddressInfo addrInfo{};
            addrInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
            addrInfo.buffer = d->curveBuffer;
            d->curveBaseAddr = vkGetBufferDeviceAddress(dev, &addrInfo);
        }

        const size_t startTexel = d->syncedCurveTexelCount;
        const size_t deltaTexels = curveTexels.size() - startTexel;
        auto *dst = static_cast<uint64_t *>(d->curveAllocInfo.pMappedData);
        std::memcpy(dst + startTexel, curveTexels.data() + startTexel, deltaTexels * sizeof(uint64_t));
        vmaFlushAllocation(alloc, d->curveAlloc,
                           VkDeviceSize(startTexel * sizeof(uint64_t)),
                           VkDeviceSize(deltaTexels * sizeof(uint64_t)));
        d->syncedCurveTexelCount = curveTexels.size();
    }

    // 2. Bindless image free-list retirement, batched staging uploads & custom GPU 3D/shader surfaces!
    if (d->syncedImageTableRev == arena.imageTableRevision()
        && qFuzzyCompare(f.syncedDpr, devicePixelRatio)) {
        return true;
    }

    auto &images = arena.images();
    if (d->uploadedTextures.size() < images.size())
        d->uploadedTextures.resize(images.size());

    bool overwritingActiveSlotWhileInFlight = false;
    VkDeviceSize totalUploadBytes = 0;

    for (size_t i = 0; i < d->uploadedTextures.size() && i < kMaxBindlessTextures; ++i) {
        GpuUploadedTexture &gpuTex = d->uploadedTextures[i];
        const bool slotValidInArena = (i < images.size()) && images[i].active;
        bool needRetire = (gpuTex.image != VK_NULL_HANDLE)
                && (!slotValidInArena || gpuTex.generation != images[i].generation);
        if (!needRetire && gpuTex.image != VK_NULL_HANDLE && slotValidInArena && images[i].isGpuSurface) {
            const uint32_t pxW = std::max(1u, uint32_t(std::lround(double(std::max(1, images[i].width)) * double(devicePixelRatio))));
            const uint32_t pxH = std::max(1u, uint32_t(std::lround(double(std::max(1, images[i].height)) * double(devicePixelRatio))));
            if (gpuTex.width != pxW || gpuTex.height != pxH || gpuTex.hasDepth != images[i].gpuNeedsDepth)
                needRetire = true;
        }
        if (needRetire) {
            if (gpuTex.depthImage != VK_NULL_HANDLE || gpuTex.depthView != VK_NULL_HANDLE)
                d->retiredImages.push_back({ gpuTex.depthImage, gpuTex.depthAlloc, gpuTex.depthView, d->nextSubmitSeq });
            d->retiredImages.push_back({ gpuTex.image, gpuTex.alloc, gpuTex.view, d->nextSubmitSeq });
            gpuTex = {};
            if (slotValidInArena && d->completedSubmitSeq < d->nextSubmitSeq)
                overwritingActiveSlotWhileInFlight = true;
        }
    }

    for (size_t i = 0; i < images.size() && i < kMaxBindlessTextures; ++i) {
        const PhaseImageEntry &entry = images[i];
        if (!entry.active || entry.isGpuSurface || entry.uploadedToGpu)
            continue;
        if (entry.cpuRgbaPremul.isNull()) {
            qWarning("QtPhase: bindless slot %zu (gen %llu, %dx%d) missing cpuRgbaPremul before GPU upload!",
                     i, static_cast<unsigned long long>(entry.generation), entry.width, entry.height);
            continue;
        }
        totalUploadBytes = alignUp(totalUploadBytes, 16)
                + VkDeviceSize(entry.width) * VkDeviceSize(entry.height) * 4;
    }

    static const bool sImgProfile = qEnvironmentVariableIntValue("QT_WIDGETS_PHASE_PROFILE") > 1;
    QElapsedTimer imgProfTimer;
    if (sImgProfile)
        imgProfTimer.start();

    if (overwritingActiveSlotWhileInFlight)
        waitAllBackendFramesIdle(d);
    const qint64 waitIdleUs = sImgProfile ? (imgProfTimer.nsecsElapsed() / 1000) : 0;

    std::vector<VkDescriptorImageInfo> descInfos;
    std::vector<VkWriteDescriptorSet> descWrites;
    descInfos.reserve(8);
    descWrites.reserve(8);

    // 2a. Allocate/resize custom GPU 3D/compute surfaces & queue dirty render callbacks!
    for (size_t i = 0; i < images.size() && i < kMaxBindlessTextures; ++i) {
        PhaseImageEntry &entry = images[i];
        if (!entry.active || !entry.isGpuSurface)
            continue;

        const uint32_t pxW = std::max(1u, uint32_t(std::lround(double(std::max(1, entry.width)) * double(devicePixelRatio))));
        const uint32_t pxH = std::max(1u, uint32_t(std::lround(double(std::max(1, entry.height)) * double(devicePixelRatio))));
        GpuUploadedTexture &gpuTex = d->uploadedTextures[i];

        if (gpuTex.image == VK_NULL_HANDLE) {
            VkImageCreateInfo imgInfo{};
            imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            imgInfo.imageType = VK_IMAGE_TYPE_2D;
            imgInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
            imgInfo.extent = { pxW, pxH, 1 };
            imgInfo.mipLevels = 1;
            imgInfo.arrayLayers = 1;
            imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imgInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                    | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                    | VK_IMAGE_USAGE_SAMPLED_BIT;

            VmaAllocationCreateInfo imgVma{};
            imgVma.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            if (vmaCreateImage(alloc, &imgInfo, &imgVma, &gpuTex.image, &gpuTex.alloc, nullptr) != VK_SUCCESS)
                return false;

            VkImageViewCreateInfo ivInfo{};
            ivInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            ivInfo.image = gpuTex.image;
            ivInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            ivInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
            ivInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            if (vkCreateImageView(dev, &ivInfo, nullptr, &gpuTex.view) != VK_SUCCESS) {
                vmaDestroyImage(alloc, gpuTex.image, gpuTex.alloc);
                gpuTex = {};
                return false;
            }

            if (entry.gpuNeedsDepth) {
                VkImageCreateInfo dInfo = imgInfo;
                dInfo.format = VK_FORMAT_D32_SFLOAT;
                dInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
                if (vmaCreateImage(alloc, &dInfo, &imgVma, &gpuTex.depthImage, &gpuTex.depthAlloc, nullptr) != VK_SUCCESS)
                    return false;

                VkImageViewCreateInfo divInfo{};
                divInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                divInfo.image = gpuTex.depthImage;
                divInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
                divInfo.format = VK_FORMAT_D32_SFLOAT;
                divInfo.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
                if (vkCreateImageView(dev, &divInfo, nullptr, &gpuTex.depthView) != VK_SUCCESS)
                    return false;
            }

            gpuTex.generation = entry.generation;
            gpuTex.width = pxW;
            gpuTex.height = pxH;
            gpuTex.hasDepth = entry.gpuNeedsDepth;
            gpuTex.renderedGpuRev = 0;
            entry.uploadedToGpu = true;

            VkDescriptorImageInfo di{};
            di.sampler = d->shared->linearSampler;
            di.imageView = gpuTex.view;
            di.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            descInfos.push_back(di);

            VkWriteDescriptorSet wr{};
            wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr.dstSet = d->bindlessSet;
            wr.dstBinding = 0;
            wr.dstArrayElement = uint32_t(i);
            wr.descriptorCount = 1;
            wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            descWrites.push_back(wr);
        }

        if (gpuTex.renderedGpuRev != entry.gpuContentRevision) {
            gpuTex.renderedGpuRev = entry.gpuContentRevision;
            f.pendingGpuSurfacePasses.push_back({
                gpuTex.image, gpuTex.view, gpuTex.depthImage, gpuTex.depthView,
                pxW, pxH, entry.gpuSurfaceCallback
            });
        }
    }

    // Release oversized transient staging buffers (> 4 MB) once this frame slot is idle and no longer needs them
    constexpr VkDeviceSize kSteadyStagingLimit = 4 * 1024 * 1024;
    if (totalUploadBytes == 0 && f.stagingBuffer != VK_NULL_HANDLE && f.stagingCapacity > kSteadyStagingLimit) {
        vmaDestroyBuffer(alloc, f.stagingBuffer, f.stagingAlloc);
        f.stagingBuffer = VK_NULL_HANDLE;
        f.stagingAlloc = VK_NULL_HANDLE;
        f.stagingAllocInfo = {};
        f.stagingCapacity = 0;
    }

    qint64 stgAllocUs = 0;
    qint64 memcpyUs = 0;
    qint64 vkImgUs = 0;
    uint32_t uploadedCount = 0;

    if (totalUploadBytes > 0) {
        const qint64 t0 = sImgProfile ? (imgProfTimer.nsecsElapsed() / 1000) : 0;
        if (totalUploadBytes > f.stagingCapacity) {
            if (f.stagingBuffer != VK_NULL_HANDLE) {
                vmaDestroyBuffer(alloc, f.stagingBuffer, f.stagingAlloc);
                f.stagingBuffer = VK_NULL_HANDLE;
            }
            const VkDeviceSize newCap = std::max<VkDeviceSize>(
                    alignUp(totalUploadBytes, 512 * 1024), 256 * 1024);

            VkBufferCreateInfo stgInfo{};
            stgInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            stgInfo.size = newCap;
            stgInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

            VmaAllocationCreateInfo stgVma{};
            // Keep staging buffers in host system RAM (GTT) so multi-megabyte document page
            // uploads never thrash integrated GPU BAR VRAM or trigger slow eviction.
            stgVma.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
            stgVma.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

            if (vmaCreateBuffer(alloc, &stgInfo, &stgVma, &f.stagingBuffer, &f.stagingAlloc, &f.stagingAllocInfo) != VK_SUCCESS)
                return false;
            f.stagingCapacity = newCap;
        }
        if (sImgProfile)
            stgAllocUs = (imgProfTimer.nsecsElapsed() / 1000) - t0;

        auto *stgBase = static_cast<uchar *>(f.stagingAllocInfo.pMappedData);
        VkDeviceSize writeCursor = 0;

        for (size_t i = 0; i < images.size() && i < kMaxBindlessTextures; ++i) {
            PhaseImageEntry &entry = images[i];
            if (!entry.active || entry.isGpuSurface || entry.uploadedToGpu || entry.cpuRgbaPremul.isNull())
                continue;

            const uint32_t w = uint32_t(entry.width);
            const uint32_t h = uint32_t(entry.height);
            const VkDeviceSize imgBytes = VkDeviceSize(w) * VkDeviceSize(h) * 4;
            writeCursor = alignUp(writeCursor, 16);

            const qint64 tm0 = sImgProfile ? (imgProfTimer.nsecsElapsed() / 1000) : 0;
            uchar *dst = stgBase + writeCursor;
            const qsizetype srcStride = entry.cpuRgbaPremul.bytesPerLine();
            const size_t rowBytes = size_t(w) * 4;
            if (size_t(srcStride) == rowBytes) {
                std::memcpy(dst, entry.cpuRgbaPremul.constBits(), size_t(imgBytes));
            } else {
                const uchar *src = entry.cpuRgbaPremul.constBits();
                for (uint32_t row = 0; row < h; ++row)
                    std::memcpy(dst + size_t(row) * rowBytes, src + size_t(row) * size_t(srcStride), rowBytes);
            }
            if (sImgProfile)
                memcpyUs += (imgProfTimer.nsecsElapsed() / 1000) - tm0;

            const qint64 ti0 = sImgProfile ? (imgProfTimer.nsecsElapsed() / 1000) : 0;
            const QImage::Format srcFmt = entry.cpuRgbaPremul.format();
            const bool isBgra = (srcFmt == QImage::Format_ARGB32_Premultiplied || srcFmt == QImage::Format_RGB32);
            const VkFormat vkFmt = isBgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;

            VkImageCreateInfo imgInfo{};
            imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            imgInfo.imageType = VK_IMAGE_TYPE_2D;
            imgInfo.format = vkFmt;
            imgInfo.extent = { w, h, 1 };
            imgInfo.mipLevels = 1;
            imgInfo.arrayLayers = 1;
            imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imgInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

            VmaAllocationCreateInfo imgVma{};
            imgVma.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

            GpuUploadedTexture gpuTex{};
            gpuTex.generation = entry.generation;
            gpuTex.width = w;
            gpuTex.height = h;
            if (vmaCreateImage(alloc, &imgInfo, &imgVma, &gpuTex.image, &gpuTex.alloc, nullptr) != VK_SUCCESS)
                return false;

            VkImageViewCreateInfo ivInfo{};
            ivInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            ivInfo.image = gpuTex.image;
            ivInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            ivInfo.format = vkFmt;
            if (srcFmt == QImage::Format_RGB32)
                ivInfo.components.a = VK_COMPONENT_SWIZZLE_ONE;
            ivInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            if (vkCreateImageView(dev, &ivInfo, nullptr, &gpuTex.view) != VK_SUCCESS) {
                vmaDestroyImage(alloc, gpuTex.image, gpuTex.alloc);
                return false;
            }
            if (sImgProfile) {
                vkImgUs += (imgProfTimer.nsecsElapsed() / 1000) - ti0;
                if (imgBytes >= 1024 * 1024) {
                    std::fprintf(stderr, "  [IMG_BIG] slot=%zu gen=%llu %ux%u fmt=%d key=0x%llx\n",
                                 i, static_cast<unsigned long long>(entry.generation),
                                 w, h, int(srcFmt),
                                 static_cast<unsigned long long>(entry.sourceCacheKey));
                }
            }
            ++uploadedCount;

            f.pendingImageCopies.push_back({ gpuTex.image, writeCursor, w, h });
            writeCursor += imgBytes;

            d->uploadedTextures[i] = gpuTex;
            entry.uploadedToGpu = true;
            d->maxUploadedImageGen = std::max(d->maxUploadedImageGen, entry.generation);

            VkDescriptorImageInfo di{};
            di.sampler = d->shared->linearSampler;
            di.imageView = gpuTex.view;
            di.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            descInfos.push_back(di);

            VkWriteDescriptorSet wr{};
            wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr.dstSet = d->bindlessSet;
            wr.dstBinding = 0;
            wr.dstArrayElement = uint32_t(i);
            wr.descriptorCount = 1;
            wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            descWrites.push_back(wr);
        }

        if (writeCursor > 0)
            vmaFlushAllocation(alloc, f.stagingAlloc, 0, writeCursor);

        if (sImgProfile && uploadedCount > 0) {
            std::fprintf(stderr, "[PhaseProfile IMG] n=%u bytes=%.2fMB waitIdle=%.2fms stgAlloc=%.2fms memcpy=%.2fms vkImg=%.2fms\n",
                         uploadedCount, double(totalUploadBytes) / (1024.0 * 1024.0),
                         double(waitIdleUs) / 1000.0, double(stgAllocUs) / 1000.0,
                         double(memcpyUs) / 1000.0, double(vkImgUs) / 1000.0);
        }
    }

    if (!descWrites.empty()) {
        for (size_t wi = 0; wi < descWrites.size(); ++wi)
            descWrites[wi].pImageInfo = &descInfos[wi];
        vkUpdateDescriptorSets(dev, uint32_t(descWrites.size()), descWrites.data(), 0, nullptr);
    }

    arena.releaseUploadedCpuImagePixels();
    d->syncedImageTableRev = arena.imageTableRevision();
    return true;
}

static bool ensureFrameWorkBuffer(QPhaseVulkanBackendData *d, FrameSlot &f, VkExtent2D extent, uint32_t drawCount)
{
    const uint32_t tilesX = (extent.width + kTileSize - 1u) / kTileSize;
    const uint32_t tilesY = (extent.height + kTileSize - 1u) / kTileSize;
    const uint32_t neededTiles = std::max(1u, tilesX * tilesY);
    const uint32_t superX = (tilesX + 7u) / 8u;
    const uint32_t superY = (tilesY + 7u) / 8u;
    const uint32_t neededSuperTiles = std::max(1u, superX * superY);
    const uint32_t neededDraws = std::max(1u, drawCount);
    const uint32_t neededWords = (neededDraws + 31u) / 32u;

    if (f.workBuffer != VK_NULL_HANDLE
        && f.capTiles >= neededTiles
        && f.capSuperTiles >= neededSuperTiles
        && f.capDrawCount >= neededDraws
        && f.capMaskWords >= neededWords) {
        return true;
    }

    if (f.workBuffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(d->shared->allocator, f.workBuffer, f.workAlloc);
        f.workBuffer = VK_NULL_HANDLE;
    }

    // Dynamic per-surface sizing with growth hysteresis: ~79 KB for popups/menus,
    // avoiding per-pixel reallocation during interactive window drag-resize!
    const uint32_t capTiles = uint32_t(alignUp(std::max(neededTiles + neededTiles / 2u, 256u), 512u));
    const uint32_t capSuperTiles = uint32_t(alignUp(std::max(neededSuperTiles + neededSuperTiles / 2u, 16u), 16u));
    const uint32_t capDraws = std::max(neededDraws * 2u, 256u);
    const uint32_t capWords = (capDraws + 31u) / 32u;
    const uint32_t capTileItems = std::clamp(capTiles * 64u, 16384u, 2097152u);

    const VkDeviceSize counterOff = 0;
    const VkDeviceSize headersOff = 64;
    const VkDeviceSize headersBytes = VkDeviceSize(capTiles) * 8;
    const VkDeviceSize itemsOff = alignUp(headersOff + headersBytes, 64);
    const VkDeviceSize itemsBytes = VkDeviceSize(capTileItems) * sizeof(uint32_t);
    const VkDeviceSize prepOff = alignUp(itemsOff + itemsBytes, 64);
    const VkDeviceSize prepBytes = VkDeviceSize(capDraws) * sizeof(GpuPreparedPrim);
    const VkDeviceSize masksOff = alignUp(prepOff + prepBytes, 64);
    const VkDeviceSize masksBytes = VkDeviceSize(capSuperTiles) * VkDeviceSize(capWords) * sizeof(uint32_t);
    const VkDeviceSize totalBytes = alignUp(masksOff + masksBytes, 256);

    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = totalBytes;
    bufInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
            | VK_BUFFER_USAGE_TRANSFER_DST_BIT
            | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    VmaAllocationCreateInfo vmaCi{};
    vmaCi.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateBuffer(d->shared->allocator, &bufInfo, &vmaCi, &f.workBuffer, &f.workAlloc, nullptr) != VK_SUCCESS)
        return false;

    VkBufferDeviceAddressInfo addrInfo{};
    addrInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addrInfo.buffer = f.workBuffer;
    const VkDeviceAddress baseAddr = vkGetBufferDeviceAddress(d->shared->device, &addrInfo);

    f.counterOffset = counterOff;
    f.superTileMasksOffset = masksOff;
    f.tileCounterAddr = baseAddr + counterOff;
    f.tileHeadersAddr = baseAddr + headersOff;
    f.tileItemsAddr = baseAddr + itemsOff;
    f.preparedPrimsAddr = baseAddr + prepOff;
    f.superTileMasksAddr = baseAddr + masksOff;

    f.capTiles = capTiles;
    f.capSuperTiles = capSuperTiles;
    f.capDrawCount = capDraws;
    f.capMaskWords = capWords;
    f.capTileItems = capTileItems;
    return true;
}

bool QPhaseVulkanBackend::syncArenaToGpu(QPhaseSceneArena &arena,
                                         uint32_t frameSlotIdx,
                                         qreal devicePixelRatio,
                                         uint64_t frameTimeNs,
                                         quint64 *outSyncedBytes)
{
    if (!arena.cpuBridges().empty()) {
        if (!d->cpuBridgeRasterizer)
            d->cpuBridgeRasterizer = std::make_unique<QPhaseCpuRasterizer>();
        arena.rasterizeDirtyCpuBridges(*d->cpuBridgeRasterizer, devicePixelRatio, frameTimeNs);
    }
    arena.resolveHierarchy();

    FrameSlot &f = d->frames[frameSlotIdx];
    if (!syncArenaImagesAndCurves(d.get(), f, arena, devicePixelRatio))
        return false;

    const auto &transforms = arena.resolvedTransforms();
    const auto &clips = arena.resolvedClips();
    const auto &prims = arena.primitives();
    const auto &order = arena.drawOrder();

    for (uint32_t primIdx : order) {
        if (primIdx >= prims.size())
            continue;
        const PhaseGpuPrimitive &p = prims[primIdx];
        if (p.kind == uint32_t(PhasePrimKind::Image)) {
            const uint32_t slot = p.resourceIndex;
            if (slot >= d->uploadedTextures.size() || d->uploadedTextures[slot].image == VK_NULL_HANDLE) {
                const auto &imgs = arena.images();
                const bool inRange = slot < imgs.size();
                qWarning("QtPhase: Image prim %u references unuploaded bindless slot %u (active=%d, uploaded=%d, cpuNull=%d, imgTableRev=%llu, syncedRev=%llu)",
                         primIdx, slot,
                         inRange ? int(imgs[slot].active) : -1,
                         inRange ? int(imgs[slot].uploadedToGpu) : -1,
                         inRange ? int(imgs[slot].cpuRgbaPremul.isNull()) : -1,
                         static_cast<unsigned long long>(arena.imageTableRevision()),
                         static_cast<unsigned long long>(d->syncedImageTableRev));
            }
        }
    }

    f.drawCount = uint32_t(order.size());
    f.bakeBlockCount = 0;

    const uint64_t curAtlasGen = d->shared->atlasGeneration.load(std::memory_order_acquire);

    // Fast-path: Zero-copy animation frame (scene revision, DPR & shared atlas generation unchanged -> 0 B synced!)
    if (f.syncedRevision == arena.revision()
        && f.syncedAtlasGen == curAtlasGen
        && qFuzzyCompare(f.syncedDpr, devicePixelRatio)
        && f.sceneBuffer != VK_NULL_HANDLE) {
        if (outSyncedBytes)
            *outSyncedBytes = 0;
        return true;
    }

    const float dpr = float(devicePixelRatio);
    const auto quantDpr = uint16_t(std::clamp(int(std::lround(dpr * 256.0f)), 1, 65535));
    const int subDivX = (dpr >= 1.35f) ? 32 : 64;
    const int subDivY = (dpr >= 1.35f) ? 8 : 64;

    quint64 hitsThisFrame = 0;
    quint64 missesThisFrame = 0;
    uint64_t bakedAtlasGen = curAtlasGen;

    {
        std::lock_guard<std::mutex> glock(d->shared->glyphMutex);

        auto allocateAtlasRect = [&](int w, int h, int *outX, int *outY) -> bool {
            const int padW = w + 1;
            const int padH = h + 1;
            if (d->shared->atlasShelfX + padW > int(kGlyphAtlasWidth) - 1) {
                d->shared->atlasShelfX = 1;
                d->shared->atlasShelfY += d->shared->atlasRowHeight;
                d->shared->atlasRowHeight = 0;
            }
            if (d->shared->atlasShelfY + padH > int(kGlyphAtlasHeight) - 1)
                return false;
            *outX = d->shared->atlasShelfX;
            *outY = d->shared->atlasShelfY;
            d->shared->atlasShelfX += padW;
            d->shared->atlasRowHeight = std::max(d->shared->atlasRowHeight, padH);
            return true;
        };

        for (int passAttempt = 0; passAttempt < 2; ++passAttempt) {
            d->scratchGlyphSlots.assign(order.size(), GpuGlyphSlot{});
            d->scratchBakeCmds.clear();
            d->scratchBakeBlocks.clear();
            hitsThisFrame = 0;
            missesThisFrame = 0;
            bakedAtlasGen = d->shared->atlasGeneration.load(std::memory_order_relaxed);
            bool overflowedAtlas = false;

            for (size_t oi = 0; oi < order.size(); ++oi) {
                const uint32_t primIdx = order[oi];
                if (primIdx >= prims.size())
                    continue;
                const PhaseGpuPrimitive &p = prims[primIdx];
                if (p.kind != uint32_t(PhasePrimKind::HbGpuCurve))
                    continue;

                const PhaseResolvedTransform &tx = transforms[p.transformIndex];
                if (tx.opacity <= 0.0f)
                    continue;

                const float designW = std::max(p.p2 - p.p0, 1e-4f);
                const float designH = std::max(p.p3 - p.p1, 1e-4f);
                const float locScaleX = p.w / designW;
                const float locScaleY = -p.h / designH;
                const float locOffX = p.x - p.p0 * locScaleX;
                const float locOffY = p.y - p.p3 * locScaleY;

                const float xx = tx.m00 * locScaleX * dpr;
                const float xy = tx.m01 * locScaleY * dpr;
                const float yx = tx.m10 * locScaleX * dpr;
                const float yy = tx.m11 * locScaleY * dpr;
                const float det = xx * yy - xy * yx;
                if (std::abs(det) <= 1e-10f)
                    continue;

                float devBoxMinX = 0.0f, devBoxMinY = 0.0f, devBoxMaxX = 0.0f, devBoxMaxY = 0.0f;
                if ((tx.flags & 1u) == 0u) {
                    const float x0 = (p.x * tx.m00 + tx.tx) * dpr;
                    const float y0 = (p.y * tx.m11 + tx.ty) * dpr;
                    const float x1 = ((p.x + p.w) * tx.m00 + tx.tx) * dpr;
                    const float y1 = ((p.y + p.h) * tx.m11 + tx.ty) * dpr;
                    devBoxMinX = std::min(x0, x1);
                    devBoxMaxX = std::max(x0, x1);
                    devBoxMinY = std::min(y0, y1);
                    devBoxMaxY = std::max(y0, y1);
                } else {
                    auto txDev = [&](float lx, float ly, float &ox, float &oy) noexcept {
                        ox = (tx.m00 * lx + tx.m01 * ly + tx.tx) * dpr;
                        oy = (tx.m10 * lx + tx.m11 * ly + tx.ty) * dpr;
                    };
                    float cx[4], cy[4];
                    txDev(p.x,       p.y,       cx[0], cy[0]);
                    txDev(p.x + p.w, p.y,       cx[1], cy[1]);
                    txDev(p.x,       p.y + p.h, cx[2], cy[2]);
                    txDev(p.x + p.w, p.y + p.h, cx[3], cy[3]);
                    devBoxMinX = std::min({cx[0], cx[1], cx[2], cx[3]});
                    devBoxMaxX = std::max({cx[0], cx[1], cx[2], cx[3]});
                    devBoxMinY = std::min({cy[0], cy[1], cy[2], cy[3]});
                    devBoxMaxY = std::max({cy[0], cy[1], cy[2], cy[3]});
                }

                const float devBoxW = devBoxMaxX - devBoxMinX;
                const float devBoxH = devBoxMaxY - devBoxMinY;
                if (devBoxW <= 0.25f || devBoxH <= 0.25f
                    || devBoxW > float(kMaxCachedGlyphDim - 3)
                    || devBoxH > float(kMaxCachedGlyphDim - 3)) {
                    continue;
                }

                int ix0 = int(std::floor(devBoxMinX));
                int iy0 = int(std::floor(devBoxMinY));
                int qx = int(std::lround((devBoxMinX - float(ix0)) * float(subDivX)));
                int qy = int(std::lround((devBoxMinY - float(iy0)) * float(subDivY)));
                if (qx >= subDivX) {
                    qx = 0;
                    ++ix0;
                }
                if (qy >= subDivY) {
                    qy = 0;
                    ++iy0;
                }

                const float subOffX = float(qx) / float(subDivX);
                const float subOffY = float(qy) / float(subDivY);
                const int gw = int(std::ceil(subOffX + devBoxW)) + 2;
                const int gh = int(std::ceil(subOffY + devBoxH)) + 2;
                if (gw < 3 || gw > kMaxCachedGlyphDim || gh < 3 || gh > kMaxCachedGlyphDim)
                    continue;

                const GpuGlyphCacheKey key{
                    arena.arenaId(),
                    p.resourceIndex,
                    int32_t(std::lround(xx * 4096.0f)),
                    int32_t(std::lround(xy * 4096.0f)),
                    int32_t(std::lround(yx * 4096.0f)),
                    int32_t(std::lround(yy * 4096.0f)),
                    quantDpr,
                    uint8_t(qx),
                    uint8_t(qy)
                };

                auto [it, inserted] = d->shared->glyphCacheMap.try_emplace(key);
                GpuGlyphAtlasEntry &ce = it->second;

                if (ce.width == 0) {
                    int ax = 0, ay = 0;
                    if (!allocateAtlasRect(gw, gh, &ax, &ay)) {
                        if (passAttempt == 0) {
                            {
                                std::lock_guard<std::mutex> qlock(d->shared->queueMutex);
                                vkDeviceWaitIdle(d->shared->device);
                            }
                            d->shared->glyphCacheMap.clear();
                            d->shared->atlasShelfX = 1;
                            d->shared->atlasShelfY = 1;
                            d->shared->atlasRowHeight = 0;
                            d->shared->atlasGeneration.fetch_add(1, std::memory_order_acq_rel);
                            overflowedAtlas = true;
                            break;
                        }
                        continue;
                    }

                    ce.atlasX = uint16_t(ax);
                    ce.atlasY = uint16_t(ay);
                    ce.width = uint16_t(gw);
                    ce.height = uint16_t(gh);

                    const float originDevX = (tx.m00 * locOffX + tx.m01 * locOffY + tx.tx) * dpr;
                    const float originDevY = (tx.m10 * locOffX + tx.m11 * locOffY + tx.ty) * dpr;
                    const float qOriginDevX = originDevX + (float(ix0) + subOffX - devBoxMinX);
                    const float qOriginDevY = originDevY + (float(iy0) + subOffY - devBoxMinY);
                    const float vx0 = (float(ix0 - 1) + 0.5f) - qOriginDevX;
                    const float vy0 = (float(iy0 - 1) + 0.5f) - qOriginDevY;
                    const float invDet = 1.0f / det;

                    const uint32_t cmdIdx = uint32_t(d->scratchBakeCmds.size());
                    GpuGlyphBakeCmd cmd{};
                    cmd.curveTexelOffset = p.resourceIndex;
                    cmd.atlasX = ax;
                    cmd.atlasY = ay;
                    cmd.width = gw;
                    cmd.height = gh;
                    cmd.originDesignX = (yy * vx0 - xy * vy0) * invDet;
                    cmd.originDesignY = (-yx * vx0 + xx * vy0) * invDet;
                    cmd.stepDesignXx = yy * invDet;
                    cmd.stepDesignYx = -yx * invDet;
                    cmd.stepDesignXy = -xy * invDet;
                    cmd.stepDesignYy = xx * invDet;
                    cmd.pixelsPerUnitX = std::hypot(xx, yx);
                    cmd.pixelsPerUnitY = std::hypot(xy, yy);
                    cmd.devicePixelRatio = dpr;
                    d->scratchBakeCmds.push_back(cmd);

                    for (int by = 0; by < gh; by += 8) {
                        for (int bx = 0; bx < gw; bx += 8) {
                            d->scratchBakeBlocks.push_back({ cmdIdx, uint32_t(bx), uint32_t(by), 0u });
                        }
                    }
                    ++missesThisFrame;
                } else {
                    ++hitsThisFrame;
                }

                const int origX = std::clamp(ix0 - 1, -32768, 32767);
                const int origY = std::clamp(iy0 - 1, -32768, 32767);
                GpuGlyphSlot &gs = d->scratchGlyphSlots[oi];
                gs.packedOrigXY = uint32_t(uint16_t(int16_t(origX)))
                        | (uint32_t(uint16_t(int16_t(origY))) << 16u);
                gs.packedAtlasXY = uint32_t(ce.atlasX) | (uint32_t(ce.atlasY) << 16u);
                gs.packedWH = uint32_t(ce.width) | (uint32_t(ce.height) << 16u);
            }

            if (!overflowedAtlas)
                break;
        }
    }

    if (hitsThisFrame > 0)
        d->glyphCacheHits.fetch_add(hitsThisFrame, std::memory_order_relaxed);
    if (missesThisFrame > 0)
        d->glyphCacheMisses.fetch_add(missesThisFrame, std::memory_order_relaxed);

    f.bakeBlockCount = uint32_t(d->scratchBakeBlocks.size());

    const VkDeviceSize txBytes = transforms.size() * sizeof(PhaseResolvedTransform);
    const VkDeviceSize clBytes = clips.size() * sizeof(PhaseResolvedClip);
    const VkDeviceSize prBytes = prims.size() * sizeof(PhaseGpuPrimitive);
    const VkDeviceSize doBytes = order.size() * sizeof(uint32_t);
    const VkDeviceSize gsBytes = d->scratchGlyphSlots.size() * sizeof(GpuGlyphSlot);
    const VkDeviceSize bkBytes = d->scratchBakeCmds.size() * sizeof(GpuGlyphBakeCmd);
    const VkDeviceSize bbBytes = d->scratchBakeBlocks.size() * sizeof(GpuGlyphBakeBlock);

    const bool needRealloc = (f.sceneBuffer == VK_NULL_HANDLE)
            || (txBytes > f.txCapBytes)
            || (clBytes > f.clCapBytes)
            || (prBytes > f.prCapBytes)
            || (doBytes > f.doCapBytes)
            || (gsBytes > f.gsCapBytes)
            || (bkBytes > f.bkCapBytes)
            || (bbBytes > f.bbCapBytes);

    if (needRealloc) {
        if (f.sceneBuffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(d->shared->allocator, f.sceneBuffer, f.sceneAlloc);
            f.sceneBuffer = VK_NULL_HANDLE;
        }

        f.txCapBytes = alignUp(std::max<VkDeviceSize>(txBytes * 2, 128 * sizeof(PhaseResolvedTransform)), 64);
        f.clCapBytes = alignUp(std::max<VkDeviceSize>(clBytes * 2, 128 * sizeof(PhaseResolvedClip)), 64);
        f.prCapBytes = alignUp(std::max<VkDeviceSize>(prBytes * 2, 512 * sizeof(PhaseGpuPrimitive)), 64);
        f.doCapBytes = alignUp(std::max<VkDeviceSize>(doBytes * 2, 512 * sizeof(uint32_t)), 64);
        f.gsCapBytes = alignUp(std::max<VkDeviceSize>(gsBytes * 2, 512 * sizeof(GpuGlyphSlot)), 64);
        f.bkCapBytes = alignUp(std::max<VkDeviceSize>(bkBytes * 2, 256 * sizeof(GpuGlyphBakeCmd)), 64);
        f.bbCapBytes = alignUp(std::max<VkDeviceSize>(bbBytes * 2, 1024 * sizeof(GpuGlyphBakeBlock)), 64);

        f.txOffset = 0;
        f.clOffset = f.txOffset + f.txCapBytes;
        f.prOffset = f.clOffset + f.clCapBytes;
        f.doOffset = f.prOffset + f.prCapBytes;
        f.gsOffset = f.doOffset + f.doCapBytes;
        f.bkOffset = f.gsOffset + f.gsCapBytes;
        f.bbOffset = f.bkOffset + f.bkCapBytes;
        f.sceneCapacity = alignUp(f.bbOffset + f.bbCapBytes, 256);

        VkBufferCreateInfo bufInfo{};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = f.sceneCapacity;
        bufInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

        VmaAllocationCreateInfo vmaCi{};
        vmaCi.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        vmaCi.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                | VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT
                | VMA_ALLOCATION_CREATE_MAPPED_BIT;

        if (vmaCreateBuffer(d->shared->allocator, &bufInfo, &vmaCi, &f.sceneBuffer, &f.sceneAlloc, &f.sceneAllocInfo) != VK_SUCCESS)
            return false;

        VkBufferDeviceAddressInfo addrInfo{};
        addrInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        addrInfo.buffer = f.sceneBuffer;
        f.sceneBaseAddr = vkGetBufferDeviceAddress(d->shared->device, &addrInfo);

        f.transformsAddr = f.sceneBaseAddr + f.txOffset;
        f.clipsAddr = f.sceneBaseAddr + f.clOffset;
        f.primitivesAddr = f.sceneBaseAddr + f.prOffset;
        f.drawOrderAddr = f.sceneBaseAddr + f.doOffset;
        f.glyphSlotsAddr = f.sceneBaseAddr + f.gsOffset;
        f.bakeCmdsAddr = f.sceneBaseAddr + f.bkOffset;
        f.bakeBlocksAddr = f.sceneBaseAddr + f.bbOffset;

        f.syncedTxClipRev = 0;
        f.syncedPrimRev = 0;
        f.syncedOrderRev = 0;
    }

    auto *dst = static_cast<uint8_t *>(f.sceneAllocInfo.pMappedData);
    if (!dst)
        return false;

    VmaAllocator alloc = d->shared->allocator;
    quint64 syncedBytes = 0;

    // 1. Transform & Clip sub-table
    if (f.syncedTxClipRev != arena.transformClipRevision()) {
        if (txBytes > 0) {
            std::memcpy(dst + f.txOffset, transforms.data(), txBytes);
            vmaFlushAllocation(alloc, f.sceneAlloc, f.txOffset, txBytes);
            syncedBytes += txBytes;
        }
        if (clBytes > 0) {
            std::memcpy(dst + f.clOffset, clips.data(), clBytes);
            vmaFlushAllocation(alloc, f.sceneAlloc, f.clOffset, clBytes);
            syncedBytes += clBytes;
        }
        f.syncedTxClipRev = arena.transformClipRevision();
    }

    // 2. Primitive table (64-primitive dirty-page UMA sync)
    if (f.syncedPrimRev != arena.primitiveRevision()) {
        if (prBytes > 0) {
            const auto &pageRevs = arena.primitivePageRevisions();
            if (f.syncedPrimRev == 0 || pageRevs.empty()) {
                std::memcpy(dst + f.prOffset, prims.data(), prBytes);
                vmaFlushAllocation(alloc, f.sceneAlloc, f.prOffset, prBytes);
                syncedBytes += prBytes;
            } else {
                for (size_t page = 0; page < pageRevs.size(); ++page) {
                    if (pageRevs[page] > f.syncedPrimRev) {
                        const size_t startIdx = page * 64;
                        if (startIdx >= prims.size())
                            break;
                        const size_t count = std::min<size_t>(64, prims.size() - startIdx);
                        const VkDeviceSize pageByteOff = f.prOffset + startIdx * sizeof(PhaseGpuPrimitive);
                        const VkDeviceSize pageBytes = count * sizeof(PhaseGpuPrimitive);
                        std::memcpy(dst + pageByteOff, prims.data() + startIdx, pageBytes);
                        vmaFlushAllocation(alloc, f.sceneAlloc, pageByteOff, pageBytes);
                        syncedBytes += pageBytes;
                    }
                }
            }
        }
        f.syncedPrimRev = arena.primitiveRevision();
    }

    // 3. Draw-order index table
    if (f.syncedOrderRev != arena.drawOrderRevision()) {
        if (doBytes > 0) {
            std::memcpy(dst + f.doOffset, order.data(), doBytes);
            vmaFlushAllocation(alloc, f.sceneAlloc, f.doOffset, doBytes);
            syncedBytes += doBytes;
        }
        f.syncedOrderRev = arena.drawOrderRevision();
    }

    // 4. Compact glyph atlas instance slots & 1D 8x8 bake blocks
    if (gsBytes > 0) {
        std::memcpy(dst + f.gsOffset, d->scratchGlyphSlots.data(), gsBytes);
        vmaFlushAllocation(alloc, f.sceneAlloc, f.gsOffset, gsBytes);
        syncedBytes += gsBytes;
    }
    if (bkBytes > 0) {
        std::memcpy(dst + f.bkOffset, d->scratchBakeCmds.data(), bkBytes);
        vmaFlushAllocation(alloc, f.sceneAlloc, f.bkOffset, bkBytes);
        syncedBytes += bkBytes;
    }
    if (bbBytes > 0) {
        std::memcpy(dst + f.bbOffset, d->scratchBakeBlocks.data(), bbBytes);
        vmaFlushAllocation(alloc, f.sceneAlloc, f.bbOffset, bbBytes);
        syncedBytes += bbBytes;
    }

    f.syncedRevision = arena.revision();
    f.syncedAtlasGen = bakedAtlasGen;
    f.syncedDpr = devicePixelRatio;
    if (outSyncedBytes)
        *outSyncedBytes = syncedBytes;
    return true;
}

bool QPhaseVulkanBackend::attachWindow(QPlatformWindow *platformWindow, const QSize &pixelSize, bool hasAlpha)
{
    if (!d->deviceReady || !d->shared || !platformWindow)
        return false;

    if (d->surface == VK_NULL_HANDLE) {
        if (!platformWindow->createPhasedVulkanSurface(
                    reinterpret_cast<void *>(d->shared->instance),
                    reinterpret_cast<void *>(vkGetInstanceProcAddr),
                    &d->surface)) {
            qWarning("QtPhase: createPhasedVulkanSurface failed");
            return false;
        }
    }

    return recreateSwapchain(pixelSize, hasAlpha);
}

bool QPhaseVulkanBackend::recreateSwapchain(const QSize &pixelSize, bool hasAlpha)
{
    if (!d->shared || d->surface == VK_NULL_HANDLE || pixelSize.isEmpty())
        return false;

    static const bool sProfile = qEnvironmentVariableIntValue("QT_WIDGETS_PHASE_PROFILE") > 0;
    QElapsedTimer scProf;
    if (sProfile)
        scProf.start();

    // Wait only on THIS backend's 2 in-flight fences (~0.3 ms, no global vkDeviceWaitIdle!)
    // so oldSwapchain + its Wayland syncobj surface can be destroyed cleanly without ever
    // accumulating zombie DMA-BUF swapchains or leaving a window black under 4-window resize!
    waitAllBackendFramesIdle(d.get());
    const qint64 tAfterWait = sProfile ? (scProf.nsecsElapsed() / 1000) : 0;
    VkDevice dev = d->shared->device;
    VkPhysicalDevice phys = d->shared->physDevice;

    // Cache surface capabilities & present modes ONCE per VkSurfaceKHR!
    // In Mesa Wayland WSI, vkGetPhysicalDeviceSurfacePresentModesKHR opens a brand new
    // wl_registry + binds all 60+ globals + blocks on 2 wl_display_roundtrip()s (~22 ms per resize)!
    if (!d->surfaceCapsCached) {
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, d->surface, &d->cachedSurfaceCaps) != VK_SUCCESS)
            return false;

        d->cachedPresentMode = VK_PRESENT_MODE_FIFO_KHR;
        uint32_t pmCount = 0;
        if (vkGetPhysicalDeviceSurfacePresentModesKHR(phys, d->surface, &pmCount, nullptr) == VK_SUCCESS && pmCount > 0) {
            std::vector<VkPresentModeKHR> modes(pmCount);
            vkGetPhysicalDeviceSurfacePresentModesKHR(phys, d->surface, &pmCount, modes.data());
            for (VkPresentModeKHR m : modes) {
                if (m == VK_PRESENT_MODE_MAILBOX_KHR) {
                    d->cachedPresentMode = VK_PRESENT_MODE_MAILBOX_KHR;
                    break;
                }
            }
        }
        d->surfaceCapsCached = true;
    }

    const VkSurfaceCapabilitiesKHR &caps = d->cachedSurfaceCaps;
    const VkPresentModeKHR chosenPresentMode = d->cachedPresentMode;

    VkExtent2D extent{};
    if (caps.currentExtent.width != 0xFFFFFFFFu && d->swapchain == VK_NULL_HANDLE) {
        extent = caps.currentExtent;
    } else {
        extent.width = std::clamp(uint32_t(pixelSize.width()), caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(uint32_t(pixelSize.height()), caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0)
        return false;

    uint32_t minImages = std::max(3u, caps.minImageCount);
    if (caps.maxImageCount > 0 && minImages > caps.maxImageCount)
        minImages = caps.maxImageCount;

    VkCompositeAlphaFlagBitsKHR compAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (hasAlpha && (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR))
        compAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    else if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR))
        compAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;

    uint32_t desiredImages = std::max(minImages, 3u);
    if (caps.maxImageCount > 0 && desiredImages > caps.maxImageCount)
        desiredImages = caps.maxImageCount;

    VkSwapchainKHR oldSwapchain = d->swapchain;
    VkSwapchainCreateInfoKHR scInfo{};
    scInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    scInfo.surface = d->surface;
    scInfo.minImageCount = desiredImages;
    scInfo.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    scInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    scInfo.imageExtent = extent;
    scInfo.imageArrayLayers = 1;
    scInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    scInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    scInfo.preTransform = caps.currentTransform;
    scInfo.compositeAlpha = compAlpha;
    scInfo.presentMode = chosenPresentMode;
    scInfo.clipped = VK_TRUE;
    scInfo.oldSwapchain = oldSwapchain;

    VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(dev, &scInfo, nullptr, &newSwapchain) != VK_SUCCESS)
        return false;
    const qint64 tAfterCreate = sProfile ? (scProf.nsecsElapsed() / 1000) : 0;

    for (VkImageView v : d->swapchainViews)
        vkDestroyImageView(dev, v, nullptr);
    d->swapchainViews.clear();
    for (VkSemaphore sem : d->renderFinishedSems)
        vkDestroySemaphore(dev, sem, nullptr);
    d->renderFinishedSems.clear();
    if (oldSwapchain != VK_NULL_HANDLE)
        vkDestroySwapchainKHR(dev, oldSwapchain, nullptr);
    const qint64 tAfterDestroy = sProfile ? (scProf.nsecsElapsed() / 1000) : 0;
    if (sProfile) {
        std::fprintf(stderr,
                     "[PhaseProfile SC] px=%ux%u mode=%d desiredImgs=%u waitIdle=%.2fms vkCreateSc=%.2fms vkDestroySc=%.2fms\n",
                     extent.width, extent.height, int(chosenPresentMode), desiredImages,
                     double(tAfterWait) / 1000.0,
                     double(tAfterCreate - tAfterWait) / 1000.0,
                     double(tAfterDestroy - tAfterCreate) / 1000.0);
    }

    d->swapchain = newSwapchain;
    d->swapchainExtent = extent;
    d->swapchainHasAlpha = hasAlpha;
    d->swapchainNeedsRecreate = false;

    uint32_t imgCount = 0;
    vkGetSwapchainImagesKHR(dev, d->swapchain, &imgCount, nullptr);
    d->swapchainImages.resize(imgCount);
    vkGetSwapchainImagesKHR(dev, d->swapchain, &imgCount, d->swapchainImages.data());

    d->swapchainViews.resize(imgCount, VK_NULL_HANDLE);
    d->renderFinishedSems.resize(imgCount, VK_NULL_HANDLE);
    d->swapchainDamageTrackers.clear();
    d->swapchainDamageTrackers.resize(imgCount);
    d->presentSurfaceTracker.reset();

    for (uint32_t i = 0; i < imgCount; ++i) {
        VkImageViewCreateInfo ivInfo{};
        ivInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivInfo.image = d->swapchainImages[i];
        ivInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        ivInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(dev, &ivInfo, nullptr, &d->swapchainViews[i]) != VK_SUCCESS)
            return false;

        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(dev, &semInfo, nullptr, &d->renderFinishedSems[i]);
    }

    return true;
}

static qint64 readSlotTimestampDurationUs(QPhaseVulkanBackendData *d, uint32_t slotIdx)
{
    if (!d->shared || !d->shared->supportsTimestamps || d->timestampQueryPool == VK_NULL_HANDLE)
        return 0;

    uint64_t ts[2]{ 0, 0 };
    const uint32_t queryBase = slotIdx * 2u;
    const VkResult res = vkGetQueryPoolResults(
            d->shared->device, d->timestampQueryPool, queryBase, 2,
            sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    if (res != VK_SUCCESS || ts[1] < ts[0])
        return d->lastCompletedGpuHwUs;

    const double ns = double(ts[1] - ts[0]) * d->shared->timestampPeriodNs;
    const qint64 us = std::max<qint64>(1, qint64(std::llround(ns / 1000.0)));
    d->lastCompletedGpuHwUs = us;
    return us;
}

static void recordDrawCommands(QPhaseVulkanBackendData *d,
                               uint32_t slotIdx,
                               VkImage targetImage,
                               VkImageView targetView,
                               VkExtent2D extent,
                               VkImageLayout finalLayout,
                               const QColor &clearColor,
                               float textStemDarkening,
                               bool forceOpaqueAlpha,
                               FrameSlot &f,
                               qreal devicePixelRatio,
                               uint64_t frameTimeNs,
                               bool preserveExistingColor = false,
                               const QRegion *partialDamageRegion = nullptr)
{
    VkCommandBuffer cmd = f.cmdBuf;
    QPhaseSharedVulkanDevice *sh = d->shared.get();
    const uint32_t queryBase = slotIdx * 2u;

    if (sh->supportsTimestamps && d->timestampQueryPool != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(cmd, d->timestampQueryPool, queryBase, 2);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, d->timestampQueryPool, queryBase + 0u);
    }

    // Step 0: Zero-stall batched staging image uploads directly inside f.cmdBuf!
    if (!f.pendingImageCopies.empty() && f.stagingBuffer != VK_NULL_HANDLE) {
        std::vector<VkImageMemoryBarrier2> barriers(f.pendingImageCopies.size());
        for (size_t i = 0; i < f.pendingImageCopies.size(); ++i) {
            VkImageMemoryBarrier2 &b = barriers[i];
            b = {};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.image = f.pendingImageCopies[i].dstImage;
            b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        }

        VkDependencyInfo dep1{};
        dep1.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep1.imageMemoryBarrierCount = uint32_t(barriers.size());
        dep1.pImageMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(cmd, &dep1);

        for (const PendingGpuImageCopy &cp : f.pendingImageCopies) {
            VkBufferImageCopy region{};
            region.bufferOffset = cp.stagingOffset;
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { cp.width, cp.height, 1 };
            vkCmdCopyBufferToImage(cmd, f.stagingBuffer, cp.dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }

        for (VkImageMemoryBarrier2 &b : barriers) {
            b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        vkCmdPipelineBarrier2(cmd, &dep1);
        f.pendingImageCopies.clear();
    }

    // Step 0b: Execute dirty custom GPU 3D / compute surface render callbacks into offscreen bindless textures!
    if (!f.pendingGpuSurfacePasses.empty()) {
        for (const PendingGpuSurfacePass &sp : f.pendingGpuSurfacePasses) {
            VkImageMemoryBarrier2 preBarriers[2]{};
            uint32_t preCount = 1;

            preBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            preBarriers[0].srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            preBarriers[0].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            preBarriers[0].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
            preBarriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            preBarriers[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            preBarriers[0].image = sp.colorImage;
            preBarriers[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

            if (sp.depthImage != VK_NULL_HANDLE) {
                preBarriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                preBarriers[1].srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
                preBarriers[1].dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
                preBarriers[1].dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                preBarriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                preBarriers[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                preBarriers[1].image = sp.depthImage;
                preBarriers[1].subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
                preCount = 2;
            }

            VkDependencyInfo depPre{};
            depPre.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            depPre.imageMemoryBarrierCount = preCount;
            depPre.pImageMemoryBarriers = preBarriers;
            vkCmdPipelineBarrier2(cmd, &depPre);

            if (sp.callback && *sp.callback) {
                QPhaseGpuSurfaceContext ctx{};
                ctx.vkInstance = reinterpret_cast<void *>(sh->instance);
                ctx.vkPhysicalDevice = reinterpret_cast<void *>(sh->physDevice);
                ctx.vkDevice = reinterpret_cast<void *>(sh->device);
                ctx.vkCommandBuffer = reinterpret_cast<void *>(cmd);
                ctx.colorImage = quint64(sp.colorImage);
                ctx.colorImageView = quint64(sp.colorView);
                ctx.depthImage = quint64(sp.depthImage);
                ctx.depthImageView = quint64(sp.depthView);
                ctx.pixelSize = QSize(int(sp.width), int(sp.height));
                ctx.devicePixelRatio = devicePixelRatio;
                ctx.frameTimeNs = frameTimeNs;
                (*sp.callback)(ctx);
            } else {
                VkRenderingAttachmentInfo clearAtt{};
                clearAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                clearAtt.imageView = sp.colorView;
                clearAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                clearAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                clearAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                VkRenderingInfo ri{};
                ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                ri.renderArea = { {0, 0}, { sp.width, sp.height } };
                ri.layerCount = 1;
                ri.colorAttachmentCount = 1;
                ri.pColorAttachments = &clearAtt;
                vkCmdBeginRendering(cmd, &ri);
                vkCmdEndRendering(cmd);
            }

            VkImageMemoryBarrier2 postBarrier{};
            postBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            postBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            postBarrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
            postBarrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            postBarrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            postBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            postBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            postBarrier.image = sp.colorImage;
            postBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

            VkDependencyInfo depPost{};
            depPost.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            depPost.imageMemoryBarrierCount = 1;
            depPost.pImageMemoryBarriers = &postBarrier;
            vkCmdPipelineBarrier2(cmd, &depPost);
        }
        f.pendingGpuSurfacePasses.clear();
    }

    // Build exact non-overlapping scissor rects (up to 8 disjoint rects, collapsing if > 8)
    VkRect2D scissorRects[8]{};
    uint32_t scissorCount = 0;
    QRect damageBounds(0, 0, int(extent.width), int(extent.height));

    if (!preserveExistingColor) {
        scissorRects[0] = { {0, 0}, extent };
        scissorCount = 1;
    } else if (partialDamageRegion && !partialDamageRegion->isEmpty()) {
        const QRect fullBox(0, 0, int(extent.width), int(extent.height));
        damageBounds = partialDamageRegion->boundingRect() & fullBox;
        if (partialDamageRegion->rectCount() <= 8) {
            for (const QRect &dr : *partialDamageRegion) {
                const QRect r = dr & fullBox;
                if (r.isEmpty())
                    continue;
                scissorRects[scissorCount++] = {
                    { r.x(), r.y() },
                    { uint32_t(r.width()), uint32_t(r.height()) }
                };
            }
        } else if (!damageBounds.isEmpty()) {
            scissorRects[0] = {
                { damageBounds.x(), damageBounds.y() },
                { uint32_t(damageBounds.width()), uint32_t(damageBounds.height()) }
            };
            scissorCount = 1;
        }
    }

    // Step 1: 1D 8x8-Block Curve Baking into shared 2048x2048 R8_UNORM atlas (all glyphs + icons + rotations!)
    if (f.bakeBlockCount > 0) {
        VkImageMemoryBarrier2 toStorage{};
        toStorage.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        toStorage.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        toStorage.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        toStorage.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        toStorage.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        toStorage.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toStorage.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toStorage.image = sh->glyphAtlasImage;
        toStorage.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

        VkDependencyInfo dep1{};
        dep1.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep1.imageMemoryBarrierCount = 1;
        dep1.pImageMemoryBarriers = &toStorage;
        vkCmdPipelineBarrier2(cmd, &dep1);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sh->glyphBakePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sh->glyphBakeLayout,
                                0, 1, &d->bindlessSet, 0, nullptr);

        PhaseGlyphBakePushConstants bakePc{};
        bakePc.curveArenaAddr = d->curveBaseAddr ? d->curveBaseAddr : sh->dummyAddr;
        bakePc.bakeCmdsAddr = f.bakeCmdsAddr ? f.bakeCmdsAddr : sh->dummyAddr;
        bakePc.bakeBlocksAddr = f.bakeBlocksAddr ? f.bakeBlocksAddr : sh->dummyAddr;
        bakePc.blockCount = f.bakeBlockCount;
        vkCmdPushConstants(cmd, sh->glyphBakeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(PhaseGlyphBakePushConstants), &bakePc);

        vkCmdDispatch(cmd, f.bakeBlockCount, 1u, 1u);

        VkImageMemoryBarrier2 toSampled{};
        toSampled.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        toSampled.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        toSampled.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        toSampled.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        toSampled.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        toSampled.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        toSampled.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toSampled.image = sh->glyphAtlasImage;
        toSampled.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

        VkDependencyInfo dep2{};
        dep2.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep2.imageMemoryBarrierCount = 1;
        dep2.pImageMemoryBarriers = &toSampled;
        vkCmdPipelineBarrier2(cmd, &dep2);
    }

    const uint32_t tilesX = (extent.width + kTileSize - 1u) / kTileSize;
    const uint32_t tilesY = (extent.height + kTileSize - 1u) / kTileSize;
    const bool needShaderDraw = (scissorCount > 0) && (f.drawCount > 0 || preserveExistingColor);

    // Step 2: Two-Stage Coarse-to-Fine O(drawCount) Compute Tile Binning (phase_prim_prep + phase_tile_bin)
    if (needShaderDraw) {
        uint32_t superMinX = 0u;
        uint32_t superMinY = 0u;
        uint32_t tileMinX = 0u;
        uint32_t tileMinY = 0u;
        uint32_t activeTilesX = tilesX;
        uint32_t activeTilesY = tilesY;

        if (preserveExistingColor && !damageBounds.isEmpty()) {
            const uint32_t rawTileMinX = uint32_t(damageBounds.x()) / kTileSize;
            const uint32_t rawTileMinY = uint32_t(damageBounds.y()) / kTileSize;
            const uint32_t rawTileMaxX = std::min(tilesX, (uint32_t(damageBounds.x() + damageBounds.width()) + kTileSize - 1u) / kTileSize);
            const uint32_t rawTileMaxY = std::min(tilesY, (uint32_t(damageBounds.y() + damageBounds.height()) + kTileSize - 1u) / kTileSize);
            superMinX = rawTileMinX >> 3u;
            superMinY = rawTileMinY >> 3u;
            tileMinX = superMinX << 3u;
            tileMinY = superMinY << 3u;
            activeTilesX = std::max(1u, rawTileMaxX - tileMinX);
            activeTilesY = std::max(1u, rawTileMaxY - tileMinY);
        }

        const uint32_t activeSuperTilesX = (activeTilesX + 7u) / 8u;
        const uint32_t activeSuperTilesY = (activeTilesY + 7u) / 8u;
        const uint32_t numPrimWords = (f.drawCount + 31u) / 32u;

        vkCmdFillBuffer(cmd, f.workBuffer, f.counterOffset, 4, 0u);
        if (numPrimWords > 0) {
            const VkDeviceSize maskFillBytes = VkDeviceSize(activeSuperTilesX)
                    * VkDeviceSize(activeSuperTilesY)
                    * VkDeviceSize(numPrimWords)
                    * sizeof(uint32_t);
            vkCmdFillBuffer(cmd, f.workBuffer, f.superTileMasksOffset, maskFillBytes, 0u);
        }

        VkMemoryBarrier2 fillToPrep{};
        fillToPrep.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        fillToPrep.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        fillToPrep.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        fillToPrep.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        fillToPrep.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

        VkDependencyInfo depFill{};
        depFill.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        depFill.memoryBarrierCount = 1;
        depFill.pMemoryBarriers = &fillToPrep;
        vkCmdPipelineBarrier2(cmd, &depFill);

        if (numPrimWords > 0) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sh->primPrepPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sh->primPrepLayout,
                                    0, 1, &d->bindlessSet, 0, nullptr);

            PhasePrimPrepPushConstants prepPc{};
            prepPc.transformsAddr = f.transformsAddr ? f.transformsAddr : sh->dummyAddr;
            prepPc.clipsAddr = f.clipsAddr ? f.clipsAddr : sh->dummyAddr;
            prepPc.primitivesAddr = f.primitivesAddr ? f.primitivesAddr : sh->dummyAddr;
            prepPc.drawOrderAddr = f.drawOrderAddr ? f.drawOrderAddr : sh->dummyAddr;
            prepPc.preparedPrimsAddr = f.preparedPrimsAddr;
            prepPc.superTileMasksAddr = f.superTileMasksAddr;
            prepPc.frameTimeNs = frameTimeNs;
            prepPc.viewportWidth = float(extent.width);
            prepPc.viewportHeight = float(extent.height);
            prepPc.devicePixelRatio = float(devicePixelRatio);
            prepPc.drawCount = f.drawCount;
            prepPc.numPrimWords = numPrimWords;
            prepPc.superMinX = superMinX;
            prepPc.superMinY = superMinY;
            prepPc.activeSuperTilesX = activeSuperTilesX;
            prepPc.activeSuperTilesY = activeSuperTilesY;

            vkCmdPushConstants(cmd, sh->primPrepLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(PhasePrimPrepPushConstants), &prepPc);
            vkCmdDispatch(cmd, numPrimWords, 1u, 1u);

            VkMemoryBarrier2 prepToBin{};
            prepToBin.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            prepToBin.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            prepToBin.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            prepToBin.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            prepToBin.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

            VkDependencyInfo depPrep{};
            depPrep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            depPrep.memoryBarrierCount = 1;
            depPrep.pMemoryBarriers = &prepToBin;
            vkCmdPipelineBarrier2(cmd, &depPrep);
        }

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sh->tileBinPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sh->tileBinLayout,
                                0, 1, &d->bindlessSet, 0, nullptr);

        PhaseTileBinPushConstants binPc{};
        binPc.preparedPrimsAddr = f.preparedPrimsAddr;
        binPc.superTileMasksAddr = f.superTileMasksAddr;
        binPc.tileCounterAddr = f.tileCounterAddr;
        binPc.tileHeadersAddr = f.tileHeadersAddr;
        binPc.tileItemsAddr = f.tileItemsAddr;
        binPc.viewportWidth = float(extent.width);
        binPc.viewportHeight = float(extent.height);
        binPc.numPrimWords = numPrimWords;
        binPc.tilesX = tilesX;
        binPc.tilesY = tilesY;
        binPc.tileMinX = tileMinX;
        binPc.tileMinY = tileMinY;
        binPc.activeTilesX = activeTilesX;
        binPc.activeTilesY = activeTilesY;
        binPc.activeSuperTilesX = activeSuperTilesX;
        binPc.maxTileItems = f.capTileItems;

        vkCmdPushConstants(cmd, sh->tileBinLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(PhaseTileBinPushConstants), &binPc);
        vkCmdDispatch(cmd, activeSuperTilesX, activeSuperTilesY, 1u);

        VkMemoryBarrier2 binToFrag{};
        binToFrag.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        binToFrag.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        binToFrag.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        binToFrag.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        binToFrag.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;

        VkDependencyInfo depBin{};
        depBin.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        depBin.memoryBarrierCount = 1;
        depBin.pMemoryBarriers = &binToFrag;
        vkCmdPipelineBarrier2(cmd, &depBin);
    }

    // Step 3: Lean Single-Pass Fullscreen Tile Rasterization (0 Bezier loops, 0 MB depth attachment!)
    VkImageMemoryBarrier2 inBarrier{};
    inBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    inBarrier.srcStageMask = preserveExistingColor
            ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
            : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    inBarrier.srcAccessMask = preserveExistingColor
            ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
            : 0;
    inBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    inBarrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
            | (preserveExistingColor ? VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT : 0);
    inBarrier.oldLayout = preserveExistingColor
            ? ((finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR || finalLayout == VK_IMAGE_LAYOUT_GENERAL)
               ? finalLayout
               : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
            : VK_IMAGE_LAYOUT_UNDEFINED;
    inBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    inBarrier.image = targetImage;
    inBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkDependencyInfo depIn{};
    depIn.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    depIn.imageMemoryBarrierCount = 1;
    depIn.pImageMemoryBarriers = &inBarrier;
    vkCmdPipelineBarrier2(cmd, &depIn);

    const float ca = forceOpaqueAlpha ? 1.0f : clearColor.alphaF();
    const float cr = clearColor.redF() * ca;
    const float cg = clearColor.greenF() * ca;
    const float cb = clearColor.blueF() * ca;
    const VkClearColorValue clearVal = {{ cr, cg, cb, ca }};

    VkRenderingAttachmentInfo colorAtt{};
    colorAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAtt.imageView = targetView;
    colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAtt.loadOp = preserveExistingColor
            ? VK_ATTACHMENT_LOAD_OP_LOAD
            : (needShaderDraw ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_CLEAR);
    colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAtt.clearValue.color = clearVal;

    VkRenderingInfo renderInfo{};
    renderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderInfo.renderArea = { {0, 0}, extent };
    renderInfo.layerCount = 1;
    renderInfo.colorAttachmentCount = 1;
    renderInfo.pColorAttachments = &colorAtt;
    renderInfo.pDepthAttachment = nullptr;

    vkCmdBeginRendering(cmd, &renderInfo);

    if (needShaderDraw) {
        VkViewport vp{ 0.0f, 0.0f, float(extent.width), float(extent.height), 0.0f, 1.0f };
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sh->uberPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sh->uberLayout,
                                0, 1, &d->bindlessSet, 0, nullptr);

        PhaseUberPushConstants pc{};
        pc.transformsAddr = f.transformsAddr ? f.transformsAddr : sh->dummyAddr;
        pc.clipsAddr = f.clipsAddr ? f.clipsAddr : sh->dummyAddr;
        pc.primitivesAddr = f.primitivesAddr ? f.primitivesAddr : sh->dummyAddr;
        pc.preparedPrimsAddr = f.preparedPrimsAddr;
        pc.tileHeadersAddr = f.tileHeadersAddr;
        pc.tileItemsAddr = f.tileItemsAddr;
        pc.glyphSlotsAddr = f.glyphSlotsAddr ? f.glyphSlotsAddr : sh->dummyAddr;
        pc.clearR = cr;
        pc.clearG = cg;
        pc.clearB = cb;
        pc.clearA = ca;
        pc.viewportWidth = float(extent.width);
        pc.viewportHeight = float(extent.height);
        pc.devicePixelRatio = float(devicePixelRatio);
        pc.textStemDarkening = textStemDarkening;
        pc.tilesX = tilesX;
        pc.forceOpaqueAlpha = forceOpaqueAlpha ? 1u : 0u;

        vkCmdPushConstants(cmd, sh->uberLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(PhaseUberPushConstants), &pc);

        for (uint32_t si = 0; si < scissorCount; ++si) {
            vkCmdSetScissor(cmd, 0, 1, &scissorRects[si]);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }
    }

    vkCmdEndRendering(cmd);

    VkImageMemoryBarrier2 toFinal{};
    toFinal.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    toFinal.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toFinal.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toFinal.dstStageMask = finalLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
            ? VK_PIPELINE_STAGE_2_TRANSFER_BIT
            : VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
    toFinal.dstAccessMask = finalLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
            ? VK_ACCESS_2_TRANSFER_READ_BIT
            : 0;
    toFinal.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toFinal.newLayout = finalLayout;
    toFinal.image = targetImage;
    toFinal.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkDependencyInfo depOut{};
    depOut.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    depOut.imageMemoryBarrierCount = 1;
    depOut.pImageMemoryBarriers = &toFinal;
    vkCmdPipelineBarrier2(cmd, &depOut);

    if (sh->supportsTimestamps && d->timestampQueryPool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, d->timestampQueryPool, queryBase + 1u);
        f.hasValidTimestamps = true;
    }
}

void QPhaseVulkanBackend::releaseRaceResources()
{
    if (!d->deviceReady || !d->shared)
        return;
    if (d->raceView != VK_NULL_HANDLE || d->raceImage != VK_NULL_HANDLE) {
        waitAllBackendFramesIdle(d.get());
        if (d->raceView != VK_NULL_HANDLE) {
            vkDestroyImageView(d->shared->device, d->raceView, nullptr);
            d->raceView = VK_NULL_HANDLE;
        }
        if (d->raceImage != VK_NULL_HANDLE) {
            vmaDestroyImage(d->shared->allocator, d->raceImage, d->raceAlloc);
            d->raceImage = VK_NULL_HANDLE;
            d->raceAlloc = VK_NULL_HANDLE;
        }
        d->raceExtent = {0, 0};
        d->raceDamageTracker.reset();
    }
}

bool QPhaseVulkanBackend::benchmarkFrameOffscreen(QPhaseSceneArena &arena,
                                                  const QSize &pixelSize,
                                                  qreal devicePixelRatio,
                                                  bool hasAlpha,
                                                  uint64_t frameTimeNs,
                                                  GpuFrameMetrics *outMetrics)
{
    if (!d->deviceReady || !d->shared || pixelSize.isEmpty())
        return false;

    QElapsedTimer workTimer;
    workTimer.start();

    const VkExtent2D extent{ uint32_t(pixelSize.width()), uint32_t(pixelSize.height()) };
    if (d->raceImage == VK_NULL_HANDLE
        || d->raceExtent.width != extent.width
        || d->raceExtent.height != extent.height) {
        releaseRaceResources();

        VkImageCreateInfo imgInfo{};
        imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgInfo.imageType = VK_IMAGE_TYPE_2D;
        imgInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        imgInfo.extent = { extent.width, extent.height, 1 };
        imgInfo.mipLevels = 1;
        imgInfo.arrayLayers = 1;
        imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imgInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

        VmaAllocationCreateInfo imgVma{};
        imgVma.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (vmaCreateImage(d->shared->allocator, &imgInfo, &imgVma, &d->raceImage, &d->raceAlloc, nullptr) != VK_SUCCESS)
            return false;

        VkImageViewCreateInfo ivInfo{};
        ivInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivInfo.image = d->raceImage;
        ivInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        ivInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(d->shared->device, &ivInfo, nullptr, &d->raceView) != VK_SUCCESS)
            return false;

        d->raceExtent = extent;
        d->raceDamageTracker.reset();
    }

    const uint32_t slotIdx = d->currentFrame;
    FrameSlot &f = d->frames[slotIdx];
    vkWaitForFences(d->shared->device, 1, &f.inFlightFence, VK_TRUE, UINT64_MAX);
    d->completedSubmitSeq = std::max(d->completedSubmitSeq, f.submittedSeq);
    reclaimCompletedResources(d.get());

    quint64 syncedBytes = 0;
    if (!syncArenaToGpu(arena, slotIdx, devicePixelRatio, frameTimeNs, &syncedBytes))
        return false;

    if (!ensureFrameWorkBuffer(d.get(), f, d->raceExtent, f.drawCount))
        return false;

    bool isFullDmg = true;
    const QRegion bufDmg = d->raceDamageTracker.computeAndAdvance(
            arena, pixelSize, devicePixelRatio, frameTimeNs, &isFullDmg, QPhaseRendererAffinity::GpuOnly);

    vkResetFences(d->shared->device, 1, &f.inFlightFence);
    vkResetCommandBuffer(f.cmdBuf, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmdBuf, &beginInfo);

    recordDrawCommands(d.get(),
                       slotIdx,
                       d->raceImage,
                       d->raceView,
                       d->raceExtent,
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       arena.clearColor(),
                       arena.textStemDarkening(),
                       !hasAlpha,
                       f,
                       devicePixelRatio,
                       frameTimeNs,
                       !isFullDmg,
                       isFullDmg ? nullptr : &bufDmg);

    vkEndCommandBuffer(f.cmdBuf);

    VkCommandBufferSubmitInfo cmdSubmit{};
    cmdSubmit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdSubmit.commandBuffer = f.cmdBuf;

    VkSubmitInfo2 submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &cmdSubmit;

    f.submittedSeq = ++d->nextSubmitSeq;
    {
        std::lock_guard<std::mutex> qlock(d->shared->queueMutex);
        if (vkQueueSubmit2(d->shared->graphicsQueue, 1, &submitInfo, f.inFlightFence) != VK_SUCCESS)
            return false;
    }

    vkWaitForFences(d->shared->device, 1, &f.inFlightFence, VK_TRUE, UINT64_MAX);
    d->completedSubmitSeq = std::max(d->completedSubmitSeq, f.submittedSeq);
    reclaimCompletedResources(d.get());

    const qint64 hwUs = f.hasValidTimestamps ? readSlotTimestampDurationUs(d.get(), slotIdx) : 0;
    const qint64 wallUs = workTimer.nsecsElapsed() / 1000;
    d->currentFrame = (slotIdx + 1) % kFramesInFlight;

    if (outMetrics) {
        outMetrics->cpuSubmitWorkUs = wallUs;
        outMetrics->gpuHardwareUs = (hwUs > 0) ? hwUs : wallUs;
        outMetrics->damagedPixels = QPhaseDamageTracker::regionPixelArea(bufDmg);
        outMetrics->syncedSceneBytes = syncedBytes;
        outMetrics->uploadedImageGen = d->maxUploadedImageGen;
    }
    return true;
}

bool QPhaseVulkanBackend::renderAndPresentWindow(QPlatformWindow *platformWindow,
                                                 QPhaseSceneArena &arena,
                                                 const QSize &pixelSize,
                                                 qreal devicePixelRatio,
                                                 bool hasAlpha,
                                                 uint64_t frameTimeNs,
                                                 GpuFrameMetrics *outMetrics,
                                                 bool waitForGpuCompletion,
                                                 const std::function<void(const GpuFrameMetrics &)> &onGpuPixelsReady,
                                                 const std::atomic<uint64_t> *expectedSizeKey)
{
    if (!d->deviceReady || !d->shared || !platformWindow || pixelSize.isEmpty())
        return false;

    const uint64_t mySizeKey = (uint64_t(uint32_t(pixelSize.width())) << 32)
            | uint64_t(uint32_t(pixelSize.height()));
    auto isSizeObsolete = [&]() noexcept -> bool {
        if (expectedSizeKey) {
            const uint64_t curKey = expectedSizeKey->load(std::memory_order_acquire);
            if (curKey != 0 && curKey != mySizeKey)
                return true;
        }
        return !platformWindow->isPhasedSizeCurrent(pixelSize, devicePixelRatio);
    };

    if (isSizeObsolete())
        return false;

    static const bool sProfile = qEnvironmentVariableIntValue("QT_WIDGETS_PHASE_PROFILE") > 0;
    QElapsedTimer submitTimer;
    submitTimer.start();
    qint64 recreateUs = 0;

    if (d->swapchain == VK_NULL_HANDLE
        || d->swapchainNeedsRecreate
        || d->swapchainExtent.width != uint32_t(pixelSize.width())
        || d->swapchainExtent.height != uint32_t(pixelSize.height())
        || d->swapchainHasAlpha != hasAlpha) {
        QElapsedTimer scT;
        if (sProfile)
            scT.start();
        if (!recreateSwapchain(pixelSize, hasAlpha))
            return false;
        if (sProfile)
            recreateUs = scT.nsecsElapsed() / 1000;
        // Re-check after recreateSwapchain BEFORE acquiring an image or doing GPU work
        if (isSizeObsolete())
            return false;
    }

    const qint64 tAfterSc = sProfile ? (submitTimer.nsecsElapsed() / 1000) : 0;
    const uint32_t slotIdx = d->currentFrame;
    FrameSlot &f = d->frames[slotIdx];
    vkWaitForFences(d->shared->device, 1, &f.inFlightFence, VK_TRUE, UINT64_MAX);
    d->completedSubmitSeq = std::max(d->completedSubmitSeq, f.submittedSeq);
    reclaimCompletedResources(d.get());
    const qint64 tAfterFence = sProfile ? (submitTimer.nsecsElapsed() / 1000) : 0;

    qint64 gpuHwUs = f.hasValidTimestamps ? readSlotTimestampDurationUs(d.get(), slotIdx)
                                          : d->lastCompletedGpuHwUs;

    uint32_t imageIndex = 0;
    static constexpr uint64_t kAcquireTimeoutNs = 100'000'000ULL;
    VkResult acq = vkAcquireNextImageKHR(
            d->shared->device, d->swapchain, kAcquireTimeoutNs, f.imageAvailableSem, VK_NULL_HANDLE, &imageIndex);
    const qint64 tAfterAcq = sProfile ? (submitTimer.nsecsElapsed() / 1000) : 0;
    if (acq == VK_TIMEOUT || acq == VK_NOT_READY)
        return false;
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain(pixelSize, hasAlpha);
        return false;
    }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR)
        return false;

    quint64 syncedBytes = 0;
    if (!syncArenaToGpu(arena, slotIdx, devicePixelRatio, frameTimeNs, &syncedBytes))
        return false;

    if (!ensureFrameWorkBuffer(d.get(), f, d->swapchainExtent, f.drawCount))
        return false;
    const qint64 tAfterSync = sProfile ? (submitTimer.nsecsElapsed() / 1000) : 0;

    // Single-pass scene hash snapshot shared across swapchainDamageTrackers[imageIndex] and presentSurfaceTracker!
    const QSize actualPxSize(int(d->swapchainExtent.width), int(d->swapchainExtent.height));
    QRegion bufferDamage;
    QRegion surfaceDamage;
    bool isFullBufferDmg = true;
    bool isFullSurfaceDmg = true;

    if (d->swapchainDamageTrackers[imageIndex].isUnchanged(arena, actualPxSize, devicePixelRatio)
        && d->presentSurfaceTracker.isUnchanged(arena, actualPxSize, devicePixelRatio)) {
        isFullBufferDmg = false;
        isFullSurfaceDmg = false;
    } else {
        const QPhaseDamageFrameSnapshot snap = QPhaseDamageFrameSnapshot::capture(
                arena, actualPxSize, devicePixelRatio, frameTimeNs, QPhaseRendererAffinity::GpuOnly);
        bufferDamage = d->swapchainDamageTrackers[imageIndex].diffAndAdvance(snap, &isFullBufferDmg);
        surfaceDamage = d->presentSurfaceTracker.diffAndAdvance(snap, &isFullSurfaceDmg);
    }
    const quint64 damagedPx = QPhaseDamageTracker::regionPixelArea(surfaceDamage);

    vkResetFences(d->shared->device, 1, &f.inFlightFence);
    vkResetCommandBuffer(f.cmdBuf, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmdBuf, &beginInfo);

    recordDrawCommands(d.get(),
                       slotIdx,
                       d->swapchainImages[imageIndex],
                       d->swapchainViews[imageIndex],
                       d->swapchainExtent,
                       VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                       arena.clearColor(),
                       arena.textStemDarkening(),
                       !hasAlpha,
                       f,
                       devicePixelRatio,
                       frameTimeNs,
                       !isFullBufferDmg,
                       isFullBufferDmg ? nullptr : &bufferDamage);

    vkEndCommandBuffer(f.cmdBuf);

    VkSemaphoreSubmitInfo waitSem{};
    waitSem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waitSem.semaphore = f.imageAvailableSem;
    waitSem.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSemaphore renderDoneSem = d->renderFinishedSems[imageIndex];
    VkSemaphoreSubmitInfo sigSem{};
    sigSem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    sigSem.semaphore = renderDoneSem;
    sigSem.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkCommandBufferSubmitInfo cmdSubmit{};
    cmdSubmit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdSubmit.commandBuffer = f.cmdBuf;

    VkSubmitInfo2 submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submitInfo.waitSemaphoreInfoCount = 1;
    submitInfo.pWaitSemaphoreInfos = &waitSem;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &cmdSubmit;
    submitInfo.signalSemaphoreInfoCount = 1;
    submitInfo.pSignalSemaphoreInfos = &sigSem;

    f.submittedSeq = ++d->nextSubmitSeq;
    {
        std::lock_guard<std::mutex> qlock(d->shared->queueMutex);
        if (vkQueueSubmit2(d->shared->graphicsQueue, 1, &submitInfo, f.inFlightFence) != VK_SUCCESS)
            return false;
    }

    if (waitForGpuCompletion) {
        vkWaitForFences(d->shared->device, 1, &f.inFlightFence, VK_TRUE, UINT64_MAX);
        d->completedSubmitSeq = std::max(d->completedSubmitSeq, f.submittedSeq);
        reclaimCompletedResources(d.get());
        if (f.hasValidTimestamps)
            gpuHwUs = readSlotTimestampDurationUs(d.get(), slotIdx);
    }

    const qint64 submitWorkUs = submitTimer.nsecsElapsed() / 1000;
    GpuFrameMetrics metrics{};
    metrics.cpuSubmitWorkUs = submitWorkUs;
    metrics.gpuHardwareUs = (gpuHwUs > 0) ? gpuHwUs : submitWorkUs;
    metrics.damagedPixels = damagedPx;
    metrics.syncedSceneBytes = syncedBytes;
    metrics.uploadedImageGen = d->maxUploadedImageGen;

    if (outMetrics)
        *outMetrics = metrics;
    if (onGpuPixelsReady)
        onGpuPixelsReady(metrics);

    std::vector<VkRectLayerKHR> presentRects;
    VkPresentRegionKHR presentRegion{};
    VkPresentRegionsKHR presentRegionsInfo{};

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &renderDoneSem;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &d->swapchain;
    presentInfo.pImageIndices = &imageIndex;

    if (d->shared->hasIncrementalPresent && !isFullSurfaceDmg && !surfaceDamage.isEmpty()) {
        const QRect fullBox(0, 0, int(d->swapchainExtent.width), int(d->swapchainExtent.height));
        presentRects.reserve(size_t(surfaceDamage.rectCount()));
        for (const QRect &dr : surfaceDamage) {
            const QRect r = dr & fullBox;
            if (r.isEmpty())
                continue;
            VkRectLayerKHR rl{};
            rl.offset = { r.x(), r.y() };
            rl.extent = { uint32_t(r.width()), uint32_t(r.height()) };
            rl.layer = 0;
            presentRects.push_back(rl);
        }
        if (!presentRects.empty()) {
            presentRegion.rectangleCount = uint32_t(presentRects.size());
            presentRegion.pRectangles = presentRects.data();
            presentRegionsInfo.sType = VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR;
            presentRegionsInfo.swapchainCount = 1;
            presentRegionsInfo.pRegions = &presentRegion;
            presentInfo.pNext = &presentRegionsInfo;
        }
    }

    if (expectedSizeKey) {
        const uint64_t curKey = expectedSizeKey->load(std::memory_order_acquire);
        if (curKey != 0 && curKey != mySizeKey) {
            d->swapchainDamageTrackers[imageIndex].reset();
            d->presentSurfaceTracker.reset();
            d->swapchainNeedsRecreate = true;
            d->currentFrame = (slotIdx + 1) % kFramesInFlight;
            if (sProfile)
                std::fprintf(stderr, "[PhaseProfile GPU] ABORT(expectedSizeKey) px=%dx%d sc=%.2fms\n",
                             pixelSize.width(), pixelSize.height(), double(recreateUs) / 1000.0);
            return false;
        }
    }

    // Acquire atomic Wayland commit lock + verify exact configured size match under lock
    // so vkQueuePresentKHR can NEVER commit an obsolete buffer onto a newer ack_configure + wp_viewport!
    if (!platformWindow->beginPhasedGpuPresent(pixelSize, devicePixelRatio)) {
        d->swapchainDamageTrackers[imageIndex].reset();
        d->presentSurfaceTracker.reset();
        d->swapchainNeedsRecreate = true;
        d->currentFrame = (slotIdx + 1) % kFramesInFlight;
        if (sProfile)
            std::fprintf(stderr, "[PhaseProfile GPU] ABORT(beginPresent) px=%dx%d sc=%.2fms\n",
                         pixelSize.width(), pixelSize.height(), double(recreateUs) / 1000.0);
        return false;
    }

    const qint64 tBeforePres = sProfile ? (submitTimer.nsecsElapsed() / 1000) : 0;
    VkResult presRes = VK_SUCCESS;
    {
        std::lock_guard<std::mutex> qlock(d->shared->queueMutex);
        presRes = vkQueuePresentKHR(d->shared->graphicsQueue, &presentInfo);
    }
    platformWindow->endPhasedGpuPresent();
    const qint64 tAfterPres = sProfile ? (submitTimer.nsecsElapsed() / 1000) : 0;
    if (sProfile) {
        std::fprintf(stderr,
                     "[PhaseProfile GPU] px=%dx%d mode=%d imgs=%zu sc=%.2fms fence=%.2fms acq=%.2fms sync=%.2fms sub=%.2fms pres=%.2fms total=%.2fms hw=%.2fms\n",
                     pixelSize.width(), pixelSize.height(), int(d->cachedPresentMode), d->swapchainImages.size(),
                     double(recreateUs) / 1000.0,
                     double(tAfterFence - tAfterSc) / 1000.0,
                     double(tAfterAcq - tAfterFence) / 1000.0,
                     double(tAfterSync - tAfterAcq) / 1000.0,
                     double(tBeforePres - tAfterSync) / 1000.0,
                     double(tAfterPres - tBeforePres) / 1000.0,
                     double(tAfterPres) / 1000.0,
                     double(gpuHwUs) / 1000.0);
    }
    d->currentFrame = (slotIdx + 1) % kFramesInFlight;

    if (presRes == VK_ERROR_OUT_OF_DATE_KHR || presRes == VK_SUBOPTIMAL_KHR)
        d->swapchainNeedsRecreate = true;

    return presRes == VK_SUCCESS || presRes == VK_SUBOPTIMAL_KHR;
}

static QSize bucketizeDmaBufSize(const QSize &px) noexcept
{
    auto roundUp = [](int v) -> int {
        if (v <= 64)
            return 64;
        if (v <= 128)
            return 128;
        if (v <= 256)
            return 256;
        return (v + 255) & ~255;
    };
    return QSize(roundUp(std::max(1, px.width())), roundUp(std::max(1, px.height())));
}

static bool selectDmaBufModifier(QPhaseVulkanBackendData *d,
                                 QPlatformWindow *platformWindow,
                                 bool hasAlpha,
                                 uint64_t *outModifier)
{
    if (!d || !d->shared || !platformWindow || !outModifier)
        return false;
    if (d->dmabufModifierCached && d->dmabufModifierHasAlpha == hasAlpha) {
        *outModifier = d->dmabufChosenModifier;
        return true;
    }

    VkPhysicalDevice phys = d->shared->physDevice;
    const uint32_t drmFmt = hasAlpha ? kDrmFormatArgb8888 : kDrmFormatXrgb8888;
    const QList<quint64> compMods = platformWindow->phasedDmaBufModifiers(drmFmt);

    VkDrmFormatModifierPropertiesListEXT modList{};
    modList.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
    VkFormatProperties2 fp2{};
    fp2.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    fp2.pNext = &modList;
    vkGetPhysicalDeviceFormatProperties2(phys, VK_FORMAT_B8G8R8A8_UNORM, &fp2);
    if (modList.drmFormatModifierCount == 0)
        return false;

    std::vector<VkDrmFormatModifierPropertiesEXT> gpuMods(modList.drmFormatModifierCount);
    modList.pDrmFormatModifierProperties = gpuMods.data();
    vkGetPhysicalDeviceFormatProperties2(phys, VK_FORMAT_B8G8R8A8_UNORM, &fp2);

    auto supportsExport = [&](uint64_t mod) -> bool {
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT modInfo{};
        modInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
        modInfo.drmFormatModifier = mod;
        modInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkPhysicalDeviceExternalImageFormatInfo extInfo{};
        extInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
        extInfo.pNext = &modInfo;
        extInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

        VkPhysicalDeviceImageFormatInfo2 imgFmtInfo{};
        imgFmtInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
        imgFmtInfo.pNext = &extInfo;
        imgFmtInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        imgFmtInfo.type = VK_IMAGE_TYPE_2D;
        imgFmtInfo.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        imgFmtInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

        VkExternalImageFormatProperties extProps{};
        extProps.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
        VkImageFormatProperties2 fmtProps{};
        fmtProps.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
        fmtProps.pNext = &extProps;

        if (vkGetPhysicalDeviceImageFormatProperties2(phys, &imgFmtInfo, &fmtProps) != VK_SUCCESS)
            return false;
        return (extProps.externalMemoryProperties.externalMemoryFeatures
                & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0;
    };

    bool foundTiled = false;
    uint64_t tiledMod = 0;
    bool foundLinear = false;

    for (const VkDrmFormatModifierPropertiesEXT &m : gpuMods) {
        if (m.drmFormatModifierPlaneCount != 1u)
            continue;
        if ((m.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0u)
            continue;
        const bool compAccepts = compMods.isEmpty() || compMods.contains(quint64(m.drmFormatModifier));
        if (!compAccepts)
            continue;
        if (!supportsExport(m.drmFormatModifier))
            continue;
        if (m.drmFormatModifier == 0ULL) {
            foundLinear = true;
        } else if (!foundTiled) {
            foundTiled = true;
            tiledMod = m.drmFormatModifier;
        }
    }

    if (!foundTiled && !foundLinear)
        return false;

    d->dmabufChosenModifier = foundTiled ? tiledMod : 0ULL;
    d->dmabufModifierHasAlpha = hasAlpha;
    d->dmabufModifierCached = true;
    *outModifier = d->dmabufChosenModifier;
    return true;
}

static bool allocateDmaBufSlot(QPhaseVulkanBackendData *d,
                               QPlatformWindow *platformWindow,
                               const QSize &bucketSize,
                               bool hasAlpha,
                               uint64_t modifier,
                               QPhaseVulkanBackendData::DmaBufRingSlot *outSlot)
{
#ifdef Q_OS_LINUX
    if (!d || !d->shared || !d->shared->hasDmaBufExport || !platformWindow || !outSlot)
        return false;

    VkDevice dev = d->shared->device;
    VkPhysicalDevice phys = d->shared->physDevice;
    const uint32_t drmFmt = hasAlpha ? kDrmFormatArgb8888 : kDrmFormatXrgb8888;

    VkImageDrmFormatModifierListCreateInfoEXT modCreateInfo{};
    modCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
    modCreateInfo.drmFormatModifierCount = 1;
    modCreateInfo.pDrmFormatModifiers = &modifier;

    VkExternalMemoryImageCreateInfo extImgInfo{};
    extImgInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    extImgInfo.pNext = &modCreateInfo;
    extImgInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageCreateInfo imgInfo{};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.pNext = &extImgInfo;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
    imgInfo.extent = { uint32_t(bucketSize.width()), uint32_t(bucketSize.height()), 1 };
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    imgInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    QPhaseVulkanBackendData::DmaBufRingSlot slot{};
    slot.bucketSize = bucketSize;
    slot.hasAlpha = hasAlpha;

    auto cleanup = [&]() {
        if (slot.wlBufferHandle)
            platformWindow->destroyPhasedDmaBufBuffer(slot.wlBufferHandle);
        if (slot.exportSyncSem != VK_NULL_HANDLE)
            vkDestroySemaphore(dev, slot.exportSyncSem, nullptr);
        if (slot.view != VK_NULL_HANDLE)
            vkDestroyImageView(dev, slot.view, nullptr);
        if (slot.image != VK_NULL_HANDLE)
            vkDestroyImage(dev, slot.image, nullptr);
        if (slot.memory != VK_NULL_HANDLE)
            vkFreeMemory(dev, slot.memory, nullptr);
        if (slot.dmabufFd >= 0)
            ::close(slot.dmabufFd);
    };

    if (vkCreateImage(dev, &imgInfo, nullptr, &slot.image) != VK_SUCCESS)
        return false;

    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(dev, slot.image, &memReq);

    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(phys, &memProps);
    uint32_t chosenMemType = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReq.memoryTypeBits & (1u << i)) != 0u
            && (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0u) {
            chosenMemType = i;
            break;
        }
    }
    if (chosenMemType == UINT32_MAX) {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if ((memReq.memoryTypeBits & (1u << i)) != 0u) {
                chosenMemType = i;
                break;
            }
        }
    }
    if (chosenMemType == UINT32_MAX) {
        cleanup();
        return false;
    }

    VkMemoryDedicatedAllocateInfo dedInfo{};
    dedInfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedInfo.image = slot.image;

    VkExportMemoryAllocateInfo expAllocInfo{};
    expAllocInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    expAllocInfo.pNext = &dedInfo;
    expAllocInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = &expAllocInfo;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = chosenMemType;

    if (vkAllocateMemory(dev, &allocInfo, nullptr, &slot.memory) != VK_SUCCESS) {
        cleanup();
        return false;
    }
    if (vkBindImageMemory(dev, slot.image, slot.memory, 0) != VK_SUCCESS) {
        cleanup();
        return false;
    }

    VkImageViewCreateInfo ivInfo{};
    ivInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivInfo.image = slot.image;
    ivInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
    ivInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(dev, &ivInfo, nullptr, &slot.view) != VK_SUCCESS) {
        cleanup();
        return false;
    }

    if (d->shared->hasSyncFdExport) {
        VkExportSemaphoreCreateInfo expSem{};
        expSem.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
        expSem.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        semInfo.pNext = &expSem;
        if (vkCreateSemaphore(dev, &semInfo, nullptr, &slot.exportSyncSem) != VK_SUCCESS) {
            cleanup();
            return false;
        }
    }

    VkImageDrmFormatModifierPropertiesEXT actualMod{};
    actualMod.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
    if (d->shared->pfnGetImageDrmFormatModifierPropertiesEXT(dev, slot.image, &actualMod) != VK_SUCCESS) {
        cleanup();
        return false;
    }

    VkImageSubresource sub{};
    sub.aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT;
    sub.mipLevel = 0;
    sub.arrayLayer = 0;
    VkSubresourceLayout layout{};
    vkGetImageSubresourceLayout(dev, slot.image, &sub, &layout);

    VkMemoryGetFdInfoKHR fdInfo{};
    fdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    fdInfo.memory = slot.memory;
    fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    if (d->shared->pfnGetMemoryFdKHR(dev, &fdInfo, &slot.dmabufFd) != VK_SUCCESS || slot.dmabufFd < 0) {
        cleanup();
        return false;
    }

    QPlatformWindow::PhasedDmaBufDesc desc{};
    desc.fd = slot.dmabufFd;
    desc.bufferWidth = bucketSize.width();
    desc.bufferHeight = bucketSize.height();
    desc.drmFormat = drmFmt;
    desc.offset = uint32_t(layout.offset);
    desc.stride = uint32_t(layout.rowPitch);
    desc.modifier = actualMod.drmFormatModifier;

    slot.wlBufferHandle = platformWindow->createPhasedDmaBufBuffer(desc);
    if (!slot.wlBufferHandle) {
        cleanup();
        return false;
    }

    *outSlot = std::move(slot);
    return true;
#else
    Q_UNUSED(d);
    Q_UNUSED(platformWindow);
    Q_UNUSED(bucketSize);
    Q_UNUSED(hasAlpha);
    Q_UNUSED(modifier);
    Q_UNUSED(outSlot);
    return false;
#endif
}

void QPhaseVulkanBackend::releaseDmaBufRing(QPlatformWindow *platformWindow)
{
    if (!d)
        return;
    for (auto &slot : d->dmabufRing) {
        if (platformWindow && slot.wlBufferHandle)
            platformWindow->destroyPhasedDmaBufBuffer(slot.wlBufferHandle);
        slot.wlBufferHandle = nullptr;
        d->retiredDmaBufs.push_back({
            slot.image, slot.memory, slot.view, slot.exportSyncSem, slot.dmabufFd, d->nextSubmitSeq
        });
    }
    d->dmabufRing.clear();
    d->dmabufBucketSize = QSize();
    d->presentSurfaceTracker.reset();
}

bool QPhaseVulkanBackend::renderAndPresentDmaBuf(QPlatformWindow *platformWindow,
                                                 QPhaseSceneArena &arena,
                                                 const QSize &pixelSize,
                                                 const QSize &logicalSize,
                                                 qreal devicePixelRatio,
                                                 bool hasAlpha,
                                                 uint64_t frameTimeNs,
                                                 GpuFrameMetrics *outMetrics,
                                                 bool waitForGpuCompletion)
{
#ifdef Q_OS_LINUX
    if (!d->deviceReady || !d->shared || !d->shared->hasDmaBufExport
        || !platformWindow || pixelSize.isEmpty() || logicalSize.isEmpty()) {
        return false;
    }

    static const bool sProfile = qEnvironmentVariableIntValue("QT_WIDGETS_PHASE_PROFILE") > 0;
    QElapsedTimer submitTimer;
    submitTimer.start();
    qint64 allocUs = 0;

    // Bucketed 256px sizing with 4x shrink hysteresis -> 0 allocations across drag-resizes!
    const QSize targetBucket = bucketizeDmaBufSize(pixelSize);
    const bool bucketFits = !d->dmabufBucketSize.isEmpty()
            && d->dmabufHasAlpha == hasAlpha
            && pixelSize.width() <= d->dmabufBucketSize.width()
            && pixelSize.height() <= d->dmabufBucketSize.height();
    const qint64 curArea = qint64(d->dmabufBucketSize.width()) * qint64(d->dmabufBucketSize.height());
    const qint64 targetArea = qint64(targetBucket.width()) * qint64(targetBucket.height());
    const bool shouldShrink = bucketFits && (curArea > targetArea * 4) && (curArea > 512LL * 512LL);

    if (!bucketFits || shouldShrink) {
        releaseDmaBufRing(platformWindow);
        d->dmabufBucketSize = targetBucket;
        d->dmabufHasAlpha = hasAlpha;
    }

    uint64_t chosenMod = 0;
    if (!selectDmaBufModifier(d.get(), platformWindow, hasAlpha, &chosenMod))
        return false;

    const uint32_t frameSlotIdx = d->currentFrame;
    FrameSlot &f = d->frames[frameSlotIdx];
    vkWaitForFences(d->shared->device, 1, &f.inFlightFence, VK_TRUE, UINT64_MAX);
    d->completedSubmitSeq = std::max(d->completedSubmitSeq, f.submittedSeq);
    reclaimCompletedResources(d.get());
    const qint64 tAfterFence = sProfile ? (submitTimer.nsecsElapsed() / 1000) : 0;

    qint64 gpuHwUs = f.hasValidTimestamps ? readSlotTimestampDurationUs(d.get(), frameSlotIdx)
                                          : d->lastCompletedGpuHwUs;

    // Pick an idle DMA-BUF ring slot (prefer one whose wl_buffer is released & GPU finished)
    int chosenRingIdx = -1;
    uint64_t bestSeq = UINT64_MAX;
    for (size_t i = 0; i < d->dmabufRing.size(); ++i) {
        const auto &s = d->dmabufRing[i];
        if (!platformWindow->isPhasedDmaBufBufferBusy(s.wlBufferHandle)
            && s.lastSubmitSeq <= d->completedSubmitSeq
            && s.lastSubmitSeq < bestSeq) {
            bestSeq = s.lastSubmitSeq;
            chosenRingIdx = int(i);
        }
    }
    if (chosenRingIdx < 0 && d->dmabufRing.size() < kMaxDmaBufRingSlots) {
        QElapsedTimer allocT;
        if (sProfile)
            allocT.start();
        QPhaseVulkanBackendData::DmaBufRingSlot newSlot{};
        if (!allocateDmaBufSlot(d.get(), platformWindow, d->dmabufBucketSize, hasAlpha, chosenMod, &newSlot))
            return false;
        d->dmabufRing.push_back(std::move(newSlot));
        chosenRingIdx = int(d->dmabufRing.size()) - 1;
        if (sProfile)
            allocUs = allocT.nsecsElapsed() / 1000;
    }
    if (chosenRingIdx < 0) {
        for (size_t i = 0; i < d->dmabufRing.size(); ++i) {
            const auto &s = d->dmabufRing[i];
            if (!platformWindow->isPhasedDmaBufBufferBusy(s.wlBufferHandle)
                && s.lastSubmitSeq < bestSeq) {
                bestSeq = s.lastSubmitSeq;
                chosenRingIdx = int(i);
            }
        }
    }
    if (chosenRingIdx < 0) {
        for (size_t i = 0; i < d->dmabufRing.size(); ++i) {
            if (d->dmabufRing[i].lastSubmitSeq < bestSeq) {
                bestSeq = d->dmabufRing[i].lastSubmitSeq;
                chosenRingIdx = int(i);
            }
        }
    }
    if (chosenRingIdx < 0)
        return false;

    auto &ringSlot = d->dmabufRing[size_t(chosenRingIdx)];
    // Never reuse ringSlot.image or ringSlot.exportSyncSem while a prior GPU submission is still executing on it!
    if (ringSlot.lastSubmitSeq > d->completedSubmitSeq) {
        for (uint32_t j = 0; j < kFramesInFlight; ++j) {
            if (j == frameSlotIdx)
                continue;
            if (d->frames[j].submittedSeq >= ringSlot.lastSubmitSeq && d->frames[j].inFlightFence != VK_NULL_HANDLE) {
                vkWaitForFences(d->shared->device, 1, &d->frames[j].inFlightFence, VK_TRUE, UINT64_MAX);
                d->completedSubmitSeq = std::max(d->completedSubmitSeq, d->frames[j].submittedSeq);
            }
        }
        reclaimCompletedResources(d.get());
    }
    const qint64 tAfterSlot = sProfile ? (submitTimer.nsecsElapsed() / 1000) : 0;

    quint64 syncedBytes = 0;
    if (!syncArenaToGpu(arena, frameSlotIdx, devicePixelRatio, frameTimeNs, &syncedBytes))
        return false;

    const VkExtent2D extent{ uint32_t(pixelSize.width()), uint32_t(pixelSize.height()) };
    if (!ensureFrameWorkBuffer(d.get(), f, extent, f.drawCount))
        return false;
    const qint64 tAfterSync = sProfile ? (submitTimer.nsecsElapsed() / 1000) : 0;

    QRegion bufferDamage;
    QRegion surfaceDamage;
    bool isFullBufferDmg = true;
    bool isFullSurfaceDmg = true;

    if (ringSlot.initializedLayout
        && ringSlot.damageTracker.isUnchanged(arena, pixelSize, devicePixelRatio)
        && d->presentSurfaceTracker.isUnchanged(arena, pixelSize, devicePixelRatio)) {
        isFullBufferDmg = false;
        isFullSurfaceDmg = false;
    } else {
        const QPhaseDamageFrameSnapshot snap = QPhaseDamageFrameSnapshot::capture(
                arena, pixelSize, devicePixelRatio, frameTimeNs, QPhaseRendererAffinity::GpuOnly);
        bufferDamage = ringSlot.damageTracker.diffAndAdvance(snap, &isFullBufferDmg);
        surfaceDamage = d->presentSurfaceTracker.diffAndAdvance(snap, &isFullSurfaceDmg);
        if (!ringSlot.initializedLayout)
            isFullBufferDmg = true;
    }
    const quint64 damagedPx = QPhaseDamageTracker::regionPixelArea(surfaceDamage);

    vkResetFences(d->shared->device, 1, &f.inFlightFence);
    vkResetCommandBuffer(f.cmdBuf, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmdBuf, &beginInfo);

    recordDrawCommands(d.get(),
                       frameSlotIdx,
                       ringSlot.image,
                       ringSlot.view,
                       extent,
                       VK_IMAGE_LAYOUT_GENERAL,
                       arena.clearColor(),
                       arena.textStemDarkening(),
                       !hasAlpha,
                       f,
                       devicePixelRatio,
                       frameTimeNs,
                       !isFullBufferDmg && ringSlot.initializedLayout,
                       isFullBufferDmg ? nullptr : &bufferDamage);

    vkEndCommandBuffer(f.cmdBuf);

    VkCommandBufferSubmitInfo cmdSubmit{};
    cmdSubmit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdSubmit.commandBuffer = f.cmdBuf;

    VkSemaphoreSubmitInfo sigSem{};
    sigSem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    sigSem.semaphore = ringSlot.exportSyncSem;
    sigSem.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    const bool useExportSync = d->shared->hasSyncFdExport && (ringSlot.exportSyncSem != VK_NULL_HANDLE);

    VkSubmitInfo2 submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &cmdSubmit;
    if (useExportSync) {
        submitInfo.signalSemaphoreInfoCount = 1;
        submitInfo.pSignalSemaphoreInfos = &sigSem;
    }

    f.submittedSeq = ++d->nextSubmitSeq;
    ringSlot.lastSubmitSeq = f.submittedSeq;
    ringSlot.initializedLayout = true;
    {
        std::lock_guard<std::mutex> qlock(d->shared->queueMutex);
        if (vkQueueSubmit2(d->shared->graphicsQueue, 1, &submitInfo, f.inFlightFence) != VK_SUCCESS)
            return false;
    }

    bool importedSyncFd = false;
    if (useExportSync) {
        VkSemaphoreGetFdInfoKHR getFd{};
        getFd.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
        getFd.semaphore = ringSlot.exportSyncSem;
        getFd.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        int syncFd = -1;
        if (d->shared->pfnGetSemaphoreFdKHR(d->shared->device, &getFd, &syncFd) == VK_SUCCESS) {
            if (syncFd >= 0) {
                struct dma_buf_import_sync_file imp{};
                imp.flags = DMA_BUF_SYNC_WRITE;
                imp.fd = syncFd;
                if (::ioctl(ringSlot.dmabufFd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &imp) == 0)
                    importedSyncFd = true;
                ::close(syncFd);
            } else {
                // Already signaled on the GPU!
                importedSyncFd = true;
            }
        }
    }

    if (waitForGpuCompletion || !importedSyncFd) {
        vkWaitForFences(d->shared->device, 1, &f.inFlightFence, VK_TRUE, UINT64_MAX);
        d->completedSubmitSeq = std::max(d->completedSubmitSeq, f.submittedSeq);
        reclaimCompletedResources(d.get());
        if (f.hasValidTimestamps)
            gpuHwUs = readSlotTimestampDurationUs(d.get(), frameSlotIdx);
    }

    const qint64 tBeforePres = sProfile ? (submitTimer.nsecsElapsed() / 1000) : 0;
    const QRegion presentDmg = isFullSurfaceDmg
            ? QRegion(QRect(0, 0, pixelSize.width(), pixelSize.height()))
            : surfaceDamage;
    platformWindow->presentPhasedDmaBufBuffer(ringSlot.wlBufferHandle, pixelSize, logicalSize, presentDmg);
    const qint64 tAfterPres = sProfile ? (submitTimer.nsecsElapsed() / 1000) : 0;

    d->currentFrame = (frameSlotIdx + 1) % kFramesInFlight;

    if (outMetrics) {
        outMetrics->cpuSubmitWorkUs = submitTimer.nsecsElapsed() / 1000;
        outMetrics->gpuHardwareUs = (gpuHwUs > 0) ? gpuHwUs : outMetrics->cpuSubmitWorkUs;
        outMetrics->damagedPixels = damagedPx;
        outMetrics->syncedSceneBytes = syncedBytes;
        outMetrics->uploadedImageGen = d->maxUploadedImageGen;
    }

    if (sProfile) {
        std::fprintf(stderr,
                     "[PhaseProfile DMABUF] px=%dx%d bucket=%dx%d slot=%d/%zu mod=0x%llx alloc=%.2fms fence=%.2fms sync=%.2fms sub=%.2fms pres=%.2fms total=%.2fms hw=%.2fms\n",
                     pixelSize.width(), pixelSize.height(),
                     d->dmabufBucketSize.width(), d->dmabufBucketSize.height(),
                     chosenRingIdx, d->dmabufRing.size(),
                     static_cast<unsigned long long>(chosenMod),
                     double(allocUs) / 1000.0,
                     double(tAfterFence) / 1000.0,
                     double(tAfterSync - tAfterSlot) / 1000.0,
                     double(tBeforePres - tAfterSync) / 1000.0,
                     double(tAfterPres - tBeforePres) / 1000.0,
                     double(tAfterPres) / 1000.0,
                     double(gpuHwUs) / 1000.0);
    }

    return true;
#else
    Q_UNUSED(platformWindow);
    Q_UNUSED(arena);
    Q_UNUSED(pixelSize);
    Q_UNUSED(logicalSize);
    Q_UNUSED(devicePixelRatio);
    Q_UNUSED(hasAlpha);
    Q_UNUSED(frameTimeNs);
    Q_UNUSED(outMetrics);
    Q_UNUSED(waitForGpuCompletion);
    return false;
#endif
}

QImage QPhaseVulkanBackend::renderOffscreenToImage(QPhaseSceneArena &arena,
                                                   const QSize &pixelSize,
                                                   qreal devicePixelRatio,
                                                   uint64_t frameTimeNs)
{
    if (!d->deviceReady && !initializeDevice(false))
        return {};
    if (pixelSize.isEmpty() || !d->shared)
        return {};

    VkDevice dev = d->shared->device;
    VmaAllocator alloc = d->shared->allocator;
    const VkExtent2D extent{ uint32_t(pixelSize.width()), uint32_t(pixelSize.height()) };

    VkImageCreateInfo imgInfo{};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
    imgInfo.extent = { extent.width, extent.height, 1 };
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo imgVma{};
    imgVma.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    VkImage offImg = VK_NULL_HANDLE;
    VmaAllocation offImgAlloc = VK_NULL_HANDLE;
    if (vmaCreateImage(alloc, &imgInfo, &imgVma, &offImg, &offImgAlloc, nullptr) != VK_SUCCESS)
        return {};

    VkImageViewCreateInfo ivInfo{};
    ivInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivInfo.image = offImg;
    ivInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
    ivInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageView offView = VK_NULL_HANDLE;
    vkCreateImageView(dev, &ivInfo, nullptr, &offView);

    const VkDeviceSize readbackBytes = VkDeviceSize(extent.width) * VkDeviceSize(extent.height) * 4;
    VkBufferCreateInfo rbInfo{};
    rbInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    rbInfo.size = readbackBytes;
    rbInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VmaAllocationCreateInfo rbVma{};
    rbVma.usage = VMA_MEMORY_USAGE_AUTO;
    rbVma.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer rbBuf = VK_NULL_HANDLE;
    VmaAllocation rbAlloc = VK_NULL_HANDLE;
    VmaAllocationInfo rbAllocInfo{};
    vmaCreateBuffer(alloc, &rbInfo, &rbVma, &rbBuf, &rbAlloc, &rbAllocInfo);

    FrameSlot &f = d->frames[0];
    vkWaitForFences(dev, 1, &f.inFlightFence, VK_TRUE, UINT64_MAX);
    d->completedSubmitSeq = std::max(d->completedSubmitSeq, f.submittedSeq);
    reclaimCompletedResources(d.get());

    f.syncedRevision = 0;
    if (!syncArenaToGpu(arena, 0, devicePixelRatio, frameTimeNs, nullptr)
        || !ensureFrameWorkBuffer(d.get(), f, extent, f.drawCount)) {
        vmaDestroyBuffer(alloc, rbBuf, rbAlloc);
        vkDestroyImageView(dev, offView, nullptr);
        vmaDestroyImage(alloc, offImg, offImgAlloc);
        return {};
    }

    vkResetFences(dev, 1, &f.inFlightFence);
    vkResetCommandBuffer(f.cmdBuf, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmdBuf, &beginInfo);

    recordDrawCommands(d.get(),
                       0,
                       offImg,
                       offView,
                       extent,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       arena.clearColor(),
                       arena.textStemDarkening(),
                       false,
                       f,
                       devicePixelRatio,
                       frameTimeNs);

    VkBufferImageCopy copyRegion{};
    copyRegion.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copyRegion.imageExtent = { extent.width, extent.height, 1 };
    vkCmdCopyImageToBuffer(f.cmdBuf, offImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbBuf, 1, &copyRegion);

    vkEndCommandBuffer(f.cmdBuf);

    VkCommandBufferSubmitInfo cmdSubmit{};
    cmdSubmit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdSubmit.commandBuffer = f.cmdBuf;

    VkSubmitInfo2 submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &cmdSubmit;

    f.submittedSeq = ++d->nextSubmitSeq;
    {
        std::lock_guard<std::mutex> qlock(d->shared->queueMutex);
        vkQueueSubmit2(d->shared->graphicsQueue, 1, &submitInfo, f.inFlightFence);
    }
    vkWaitForFences(dev, 1, &f.inFlightFence, VK_TRUE, UINT64_MAX);
    d->completedSubmitSeq = std::max(d->completedSubmitSeq, f.submittedSeq);
    reclaimCompletedResources(d.get());
    vmaInvalidateAllocation(alloc, rbAlloc, 0, readbackBytes);

    QImage result(pixelSize, QImage::Format_ARGB32_Premultiplied);
    result.setDevicePixelRatio(devicePixelRatio);
    const auto *srcBytes = static_cast<const uchar *>(rbAllocInfo.pMappedData);
    for (uint32_t y = 0; y < extent.height; ++y)
        std::memcpy(result.scanLine(int(y)), srcBytes + size_t(y) * extent.width * 4, size_t(extent.width) * 4);

    vmaDestroyBuffer(alloc, rbBuf, rbAlloc);
    vkDestroyImageView(dev, offView, nullptr);
    vmaDestroyImage(alloc, offImg, offImgAlloc);

    return result;
}

QT_END_NAMESPACE
