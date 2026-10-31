#include <stdlib.h>
#include <string.h>

#include <android/log.h>
#include <vulkan/vulkan.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_vulkan.h>

#define LOG_TAG "totk-probe"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static const char *yes_no(int value)
{
    return value ? "yes" : "no";
}

static int has_extension(const VkExtensionProperties *props, uint32_t count, const char *name)
{
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(props[i].extensionName, name) == 0) {
            return 1;
        }
    }
    return 0;
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    int result = 1;
    SDL_Window *window = NULL;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkExtensionProperties *device_exts = NULL;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        LOGE("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    window = SDL_CreateWindow("TotK Recomp", 1280, 720, SDL_WINDOW_VULKAN);
    if (!window) {
        LOGE("SDL_CreateWindow failed: %s", SDL_GetError());
        goto out;
    }

    Uint32 ext_count = 0;
    const char *const *ext_names = SDL_Vulkan_GetInstanceExtensions(&ext_count);
    if (!ext_names) {
        LOGE("SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
        goto out;
    }

    uint32_t api_version = VK_API_VERSION_1_0;
    vkEnumerateInstanceVersion(&api_version);

    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "TotK Recomp",
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
        .enabledExtensionCount = ext_count,
        .ppEnabledExtensionNames = ext_names,
    };
    if (vkCreateInstance(&create_info, NULL, &instance) != VK_SUCCESS) {
        LOGE("vkCreateInstance failed");
        goto out;
    }

    uint32_t gpu_count = 0;
    vkEnumeratePhysicalDevices(instance, &gpu_count, NULL);
    if (gpu_count == 0) {
        LOGE("no Vulkan physical device");
        goto out;
    }
    gpu_count = 1;
    vkEnumeratePhysicalDevices(instance, &gpu_count, &gpu);

    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT,
    };
    VkPhysicalDeviceFeatures2 features2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &gpl,
    };
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceProperties(gpu, &props);
    vkGetPhysicalDeviceMemoryProperties(gpu, &mem);
    if (props.apiVersion >= VK_API_VERSION_1_1) {
        vkGetPhysicalDeviceFeatures2(gpu, &features2);   // gpl stays zero where the extension is absent
    }
    const VkPhysicalDeviceFeatures *features = &features2.features;

    uint32_t device_ext_count = 0;
    vkEnumerateDeviceExtensionProperties(gpu, NULL, &device_ext_count, NULL);
    device_exts = malloc(device_ext_count * sizeof *device_exts);
    if (!device_exts) {
        LOGE("out of memory");
        goto out;
    }
    vkEnumerateDeviceExtensionProperties(gpu, NULL, &device_ext_count, device_exts);

    LOGI("instance Vulkan %u.%u.%u", VK_API_VERSION_MAJOR(api_version),
         VK_API_VERSION_MINOR(api_version), VK_API_VERSION_PATCH(api_version));
    LOGI("device: %s (API %u.%u.%u)", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
         VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));
    int has_gpl = has_extension(device_exts, device_ext_count, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
    LOGI("VK_EXT_external_memory_host: %s (absent: shadow storage)",
         yes_no(has_extension(device_exts, device_ext_count, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)));
    LOGI("VK_EXT_graphics_pipeline_library: ext %s, feature %s", yes_no(has_gpl),
         yes_no(has_gpl && gpl.graphicsPipelineLibrary));
    LOGI("textureCompressionBC: %s (absent: unsupported until a BC transcoder exists)",
         yes_no(features->textureCompressionBC));
    LOGI("textureCompressionASTC_LDR: %s", yes_no(features->textureCompressionASTC_LDR));
    LOGI("shaderInt64: %s", yes_no(features->shaderInt64));
    LOGI("maxStorageBufferRange: %u", props.limits.maxStorageBufferRange);
    for (uint32_t i = 0; i < mem.memoryHeapCount; i++) {
        LOGI("memory heap %u: %llu MiB%s", i, (unsigned long long)(mem.memoryHeaps[i].size >> 20),
             (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " device-local" : "");
    }

    result = 0;

out:
    free(device_exts);
    if (instance != VK_NULL_HANDLE) {
        vkDestroyInstance(instance, NULL);
    }
    if (window) {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
    return result;
}
