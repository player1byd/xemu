/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "ui/xemu-gpu-info.h"
#include "ui/xemu-gpu-launch.h"
#include "ui/xemu-settings.h"
#include "renderer.h"
#include "device-inventory.h"
#include "xemu-version.h"

#define VkExtensionPropertiesArray GArray
#define StringArray GArray

static bool enable_validation = false;

static char const *const validation_layers[] = {
    "VK_LAYER_KHRONOS_validation",
};

static char const *const required_device_extensions[] = {
#ifdef WIN32
    VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
#else
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
#endif
};

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData, void *pUserData)
{
    fprintf(stderr, "[vk] %s\n", pCallbackData->pMessage);

    if ((messageType & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) &&
        (messageSeverity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT))) {
        assert(!g_config.display.vulkan.assert_on_validation_msg);
    }
    return VK_FALSE;
}

static bool check_validation_layer_support(void)
{
    uint32_t num_available_layers;
    vkEnumerateInstanceLayerProperties(&num_available_layers, NULL);

    g_autofree VkLayerProperties *available_layers =
        g_malloc_n(num_available_layers, sizeof(VkLayerProperties));
    vkEnumerateInstanceLayerProperties(&num_available_layers, available_layers);

    for (int i = 0; i < ARRAY_SIZE(validation_layers); i++) {
        bool found = false;
        for (int j = 0; j < num_available_layers; j++) {
            if (!strcmp(validation_layers[i], available_layers[j].layerName)) {
                found = true;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "desired validation layer not found: %s\n",
                    validation_layers[i]);
            return false;
        }
    }

    return true;
}

static VkExtensionPropertiesArray *
get_available_instance_extensions(PGRAPHState *pg)
{
    uint32_t num_extensions = 0;

    VK_CHECK(
        vkEnumerateInstanceExtensionProperties(NULL, &num_extensions, NULL));

    VkExtensionPropertiesArray *extensions = g_array_sized_new(
        FALSE, FALSE, sizeof(VkExtensionProperties), num_extensions);

    g_array_set_size(extensions, num_extensions);
    VK_CHECK(vkEnumerateInstanceExtensionProperties(
        NULL, &num_extensions, (VkExtensionProperties *)extensions->data));

    return extensions;
}

static bool
is_extension_available(VkExtensionPropertiesArray *available_extensions,
                       const char *extension_name)
{
    for (int i = 0; i < available_extensions->len; i++) {
        VkExtensionProperties *e =
            &g_array_index(available_extensions, VkExtensionProperties, i);
        if (!strcmp(e->extensionName, extension_name)) {
            return true;
        }
    }

    return false;
}

static bool
add_extension_if_available(VkExtensionPropertiesArray *available_extensions,
                           StringArray *enabled_extension_names,
                           const char *desired_extension_name)
{
    if (is_extension_available(available_extensions, desired_extension_name)) {
        g_array_append_val(enabled_extension_names, desired_extension_name);
        return true;
    }

    fprintf(stderr, "Warning: extension not available: %s\n",
            desired_extension_name);
    return false;
}

static void
add_optional_instance_extension_names(PGRAPHState *pg,
                                      VkExtensionPropertiesArray *available_extensions,
                                      StringArray *enabled_extension_names)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    r->debug_utils_extension_enabled =
        g_config.display.vulkan.validation_layers &&
        add_extension_if_available(available_extensions, enabled_extension_names,
                                   VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
}

static bool create_instance(PGRAPHState *pg, Error **errp)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    VkResult result;

    result = volkInitialize();
    if (result != VK_SUCCESS) {
        error_setg(errp, "volkInitialize failed");
        return false;
    }

    uint32_t instance_version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) {
        vkEnumerateInstanceVersion(&instance_version);
        instance_version = MIN(instance_version, VK_API_VERSION_1_3);
    }
    if (instance_version < VK_API_VERSION_1_1) {
        error_setg(errp, "Vulkan 1.1 or higher is required");
        return false;
    }
    r->vk_api_version = instance_version;

    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "xemu",
        .applicationVersion = VK_MAKE_VERSION(
            xemu_version_major, xemu_version_minor, xemu_version_patch),
        .pEngineName = "No Engine",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = r->vk_api_version,
    };

    g_autoptr(VkExtensionPropertiesArray) available_extensions =
        get_available_instance_extensions(pg);

    g_autoptr(StringArray) enabled_extension_names =
        g_array_new(FALSE, FALSE, sizeof(char *));

    add_optional_instance_extension_names(pg, available_extensions,
                                          enabled_extension_names);

    if (enabled_extension_names->len > 0) {
        fprintf(stderr, "Enabled instance extensions:\n");
        for (int i = 0; i < enabled_extension_names->len; i++) {
            fprintf(stderr, "- %s\n",
                    g_array_index(enabled_extension_names, char *, i));
        }
    }

    VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
        .enabledExtensionCount = enabled_extension_names->len,
        .ppEnabledExtensionNames =
            &g_array_index(enabled_extension_names, const char *, 0),
    };

    enable_validation = g_config.display.vulkan.validation_layers;

    VkValidationFeatureEnableEXT enables[] = {
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,
        // VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT,
    };

    VkValidationFeaturesEXT validationFeatures = {
        .sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
        .enabledValidationFeatureCount = ARRAY_SIZE(enables),
        .pEnabledValidationFeatures = enables,
    };

    if (enable_validation) {
        if (check_validation_layer_support()) {
            fprintf(stderr, "Warning: Validation layers enabled. Expect "
                            "performance impact.\n");
            create_info.enabledLayerCount = ARRAY_SIZE(validation_layers);
            create_info.ppEnabledLayerNames = validation_layers;
            create_info.pNext = &validationFeatures;
        } else {
            fprintf(stderr, "Warning: validation layers not available\n");
            enable_validation = false;
        }
    }

    result = vkCreateInstance(&create_info, NULL, &r->instance);
    if (result != VK_SUCCESS) {
        error_setg(errp, "Failed to create instance (%d)", result);
        return false;
    }

    volkLoadInstance(r->instance);

    if (r->debug_utils_extension_enabled) {
        VkDebugUtilsMessengerCreateInfoEXT messenger_info = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debugCallback,
        };
        VK_CHECK(vkCreateDebugUtilsMessengerEXT(r->instance, &messenger_info,
                                                NULL, &r->debug_messenger));
    }

    return true;
}

static bool is_queue_family_indicies_complete(QueueFamilyIndices indices)
{
    return indices.queue_family >= 0;
}

QueueFamilyIndices pgraph_vk_find_queue_families(VkPhysicalDevice device)
{
    QueueFamilyIndices indices = {
        .queue_family = -1,
    };

    uint32_t num_queue_families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &num_queue_families, NULL);

    g_autofree VkQueueFamilyProperties *queue_families =
        g_malloc_n(num_queue_families, sizeof(VkQueueFamilyProperties));
    vkGetPhysicalDeviceQueueFamilyProperties(device, &num_queue_families,
                                             queue_families);

    for (int i = 0; i < num_queue_families; i++) {
        VkQueueFamilyProperties queueFamily = queue_families[i];
        // FIXME: Support independent graphics, compute queues
        int required_flags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        if ((queueFamily.queueFlags & required_flags) == required_flags) {
            indices.queue_family = i;
        }
        if (is_queue_family_indicies_complete(indices)) {
            break;
        }
    }

    return indices;
}

static VkExtensionPropertiesArray *
get_available_device_extensions(VkPhysicalDevice device)
{
    uint32_t num_extensions = 0;

    VK_CHECK(vkEnumerateDeviceExtensionProperties(device, NULL, &num_extensions,
                                                  NULL));

    VkExtensionPropertiesArray *extensions = g_array_sized_new(
        FALSE, FALSE, sizeof(VkExtensionProperties), num_extensions);

    g_array_set_size(extensions, num_extensions);
    VK_CHECK(vkEnumerateDeviceExtensionProperties(
        device, NULL, &num_extensions,
        (VkExtensionProperties *)extensions->data));

    return extensions;
}

static StringArray *get_required_device_extension_names(void)
{
    StringArray *extensions =
        g_array_sized_new(FALSE, FALSE, sizeof(char *),
                          ARRAY_SIZE(required_device_extensions));

    g_array_append_vals(extensions, required_device_extensions,
                        ARRAY_SIZE(required_device_extensions));

    return extensions;
}

static void add_optional_device_extension_names(
    PGRAPHState *pg, VkExtensionPropertiesArray *available_extensions,
    StringArray *enabled_extension_names)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    r->custom_border_color_extension_enabled =
        add_extension_if_available(available_extensions, enabled_extension_names,
                                   VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);

    r->memory_budget_extension_enabled = add_extension_if_available(
        available_extensions, enabled_extension_names,
        VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);

    r->demote_to_helper_extension_enabled = add_extension_if_available(
        available_extensions, enabled_extension_names,
        VK_EXT_SHADER_DEMOTE_TO_HELPER_INVOCATION_EXTENSION_NAME);
}

static void get_required_device_extension_support(VkPhysicalDevice device,
                                                  bool *external_memory,
                                                  bool *external_semaphore)
{
    g_autoptr(VkExtensionPropertiesArray) available_extensions =
        get_available_device_extensions(device);

#ifdef WIN32
    *external_memory = is_extension_available(
        available_extensions, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
    *external_semaphore = is_extension_available(
        available_extensions, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
#else
    *external_memory = is_extension_available(
        available_extensions, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    *external_semaphore = is_extension_available(
        available_extensions, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
#endif
}

static uint32_t get_available_required_features(VkPhysicalDevice device)
{
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(device, &features);
    uint32_t result = 0;

    if (features.depthClamp) {
        result |= PGRAPH_VK_FEATURE_DEPTH_CLAMP;
    }
    if (features.fillModeNonSolid) {
        result |= PGRAPH_VK_FEATURE_FILL_MODE_NON_SOLID;
    }
    if (features.geometryShader) {
        result |= PGRAPH_VK_FEATURE_GEOMETRY_SHADER;
    }
    if (features.occlusionQueryPrecise) {
        result |= PGRAPH_VK_FEATURE_OCCLUSION_QUERY_PRECISE;
    }
    if (features.shaderClipDistance) {
        result |= PGRAPH_VK_FEATURE_SHADER_CLIP_DISTANCE;
    }
    if (features.shaderTessellationAndGeometryPointSize) {
        result |= PGRAPH_VK_FEATURE_GEOMETRY_POINT_SIZE;
    }
    return result;
}

static PGRAPHVkDeviceType convert_device_type(VkPhysicalDeviceType type)
{
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return PGRAPH_VK_DEVICE_TYPE_INTEGRATED;
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return PGRAPH_VK_DEVICE_TYPE_DISCRETE;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return PGRAPH_VK_DEVICE_TYPE_VIRTUAL;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return PGRAPH_VK_DEVICE_TYPE_CPU;
    default:
        return PGRAPH_VK_DEVICE_TYPE_OTHER;
    }
}

typedef struct VkEnumerationContext {
    VkInstance instance;
    VkResult last_result;
} VkEnumerationContext;

static PGRAPHVkEnumerateStatus enumerate_physical_device_tokens(
    void *opaque, uint32_t *count, uintptr_t *tokens)
{
    VkEnumerationContext *context = opaque;
    context->last_result = vkEnumeratePhysicalDevices(
        context->instance, count, (VkPhysicalDevice *)tokens);
    switch (context->last_result) {
    case VK_SUCCESS:
        return PGRAPH_VK_ENUMERATE_SUCCESS;
    case VK_INCOMPLETE:
        return PGRAPH_VK_ENUMERATE_INCOMPLETE;
    default:
        return PGRAPH_VK_ENUMERATE_FAILURE;
    }
}

static bool collect_device_inventory(PGRAPHVkState *r,
                                     PGRAPHVkDeviceRecord **records_out,
                                     VkPhysicalDevice **devices_out,
                                     size_t *count_out, Error **errp)
{
    VkEnumerationContext context = {
        .instance = r->instance,
    };
    uintptr_t *tokens = NULL;
    size_t count = 0;
    PGRAPHVkEnumerationResult enumeration =
        pgraph_vk_enumerate_device_tokens(enumerate_physical_device_tokens,
                                          &context, &tokens, &count);
    if (enumeration != PGRAPH_VK_ENUMERATION_OK) {
        if (enumeration == PGRAPH_VK_ENUMERATION_EMPTY) {
            error_setg(errp, "No Vulkan physical devices found");
        } else if (enumeration == PGRAPH_VK_ENUMERATION_UNSTABLE) {
            error_setg(errp,
                       "Vulkan physical-device inventory did not stabilize");
        } else {
            error_setg(errp, "Failed to enumerate Vulkan physical devices (%d)",
                       context.last_result);
        }
        free(tokens);
        return false;
    }

    PGRAPHVkDeviceRecord *records = g_new0(PGRAPHVkDeviceRecord, count);
    VkPhysicalDevice *devices = g_new(VkPhysicalDevice, count);
    for (size_t i = 0; i < count; i++) {
        devices[i] = (VkPhysicalDevice)tokens[i];

        VkPhysicalDeviceIDProperties id_props = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
        };
        VkPhysicalDeviceProperties2 props = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &id_props,
        };
        vkGetPhysicalDeviceProperties2(devices[i], &props);

        pstrcpy(records[i].name, sizeof(records[i].name),
                props.properties.deviceName);
        memcpy(records[i].device_uuid, id_props.deviceUUID,
               sizeof(records[i].device_uuid));
        memcpy(records[i].driver_uuid, id_props.driverUUID,
               sizeof(records[i].driver_uuid));
        records[i].vendor_id = props.properties.vendorID;
        records[i].device_id = props.properties.deviceID;
        records[i].api_version = props.properties.apiVersion;
        records[i].driver_version = props.properties.driverVersion;
        records[i].type = convert_device_type(props.properties.deviceType);

        QueueFamilyIndices indices =
            pgraph_vk_find_queue_families(devices[i]);
        bool external_memory;
        bool external_semaphore;
        get_required_device_extension_support(devices[i], &external_memory,
                                              &external_semaphore);
        PGRAPHVkDeviceCapabilities capabilities = {
            .api_version = props.properties.apiVersion,
            .has_graphics_compute_queue =
                is_queue_family_indicies_complete(indices),
            .has_external_memory = external_memory,
            .has_external_semaphore = external_semaphore,
            .available_required_features =
                get_available_required_features(devices[i]),
        };
        pgraph_vk_device_record_check_renderer_support(&records[i],
                                                        &capabilities);
    }
    free(tokens);

    *records_out = records;
    *devices_out = devices;
    *count_out = count;
    return true;
}

bool pgraph_vk_probe_device_inventory(PGRAPHVkDeviceRecord **records,
                                      size_t *count, Error **errp)
{
    g_autofree PGRAPHState *pg = g_new0(PGRAPHState, 1);
    g_autofree PGRAPHVkState *renderer = g_new0(PGRAPHVkState, 1);
    g_autofree VkPhysicalDevice *devices = NULL;

    pg->vk_renderer_state = renderer;
    *records = NULL;
    *count = 0;

    bool success = create_instance(pg, errp) &&
        collect_device_inventory(renderer, records, &devices, count, errp);
    pgraph_vk_finalize_instance(pg);
    return success;
}

static bool select_physical_device(PGRAPHState *pg, Error **errp)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    g_autofree PGRAPHVkDeviceRecord *records = NULL;
    g_autofree VkPhysicalDevice *devices = NULL;
    size_t count = 0;
    if (!collect_device_inventory(r, &records, &devices, &count, errp)) {
        return false;
    }

    xemu_gpu_info_record_inventory(records, count);

    const XemuGpuLaunchRequest *launch_request =
        xemu_gpu_launch_request_get();
    PGRAPHVkSelectionRequest request = launch_request->selection;

    fprintf(stderr, "Available physical devices:\n");
    for (size_t i = 0; i < count; i++) {
        char uuid[PGRAPH_VK_DEVICE_UUID_STRING_SIZE];
        pgraph_vk_device_uuid_format(records[i].device_uuid, uuid);
        fprintf(stderr, "- %s [%s]: %s\n", records[i].name, uuid,
                records[i].renderer_supported ? "compatible" :
                                                 records[i].rejection_reason);
    }

    PGRAPHVkSelectionResult selected =
        pgraph_vk_resolve_device(records, count, &request);
    if (selected.status != PGRAPH_VK_SELECTION_OK &&
        request.kind == PGRAPH_VK_SELECTION_LEGACY_NAME &&
        !launch_request->strict) {
        warn_report("Configured Vulkan device '%s' cannot be selected: %s; "
                    "using automatic selection",
                    request.legacy_name,
                    pgraph_vk_selection_status_string(selected.status));
        request = (PGRAPHVkSelectionRequest) {
            .kind = PGRAPH_VK_SELECTION_AUTOMATIC,
            .allow_software = true,
        };
        selected = pgraph_vk_resolve_device(records, count, &request);
    }
    if (selected.status != PGRAPH_VK_SELECTION_OK) {
        error_setg(errp, "Failed to select Vulkan physical device: %s",
                   pgraph_vk_selection_status_string(selected.status));
        return false;
    }

    r->physical_device = devices[selected.index];
    r->selected_device = records[selected.index];
    vkGetPhysicalDeviceProperties(r->physical_device, &r->device_props);
    r->vk_api_version = MIN(r->vk_api_version, r->device_props.apiVersion);

    char selected_uuid[PGRAPH_VK_DEVICE_UUID_STRING_SIZE];
    pgraph_vk_device_uuid_format(r->selected_device.device_uuid,
                                 selected_uuid);

    fprintf(stderr,
            "Selected physical device: %s [%s]\n"
            "- Vendor: %x, Device: %x\n"
            "- Driver Version: %d.%d.%d\n",
            r->device_props.deviceName, selected_uuid,
            r->device_props.vendorID,
            r->device_props.deviceID,
            VK_VERSION_MAJOR(r->device_props.driverVersion),
            VK_VERSION_MINOR(r->device_props.driverVersion),
            VK_VERSION_PATCH(r->device_props.driverVersion));

    return true;
}

static bool create_logical_device(PGRAPHState *pg, Error **errp)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    VkResult result;

    QueueFamilyIndices indices =
        pgraph_vk_find_queue_families(r->physical_device);

    g_autoptr(VkExtensionPropertiesArray) available_extensions =
        get_available_device_extensions(r->physical_device);

    g_autoptr(StringArray) enabled_extension_names =
        get_required_device_extension_names();

    add_optional_device_extension_names(pg, available_extensions,
                                        enabled_extension_names);

    fprintf(stderr, "Enabled device extensions:\n");
    for (int i = 0; i < enabled_extension_names->len; i++) {
        fprintf(stderr, "- %s\n",
                g_array_index(enabled_extension_names, char *, i));
    }

    float queuePriority = 1.0f;

    VkDeviceQueueCreateInfo queue_create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = indices.queue_family,
        .queueCount = 1,
        .pQueuePriorities = &queuePriority,
    };

    // Check device features
    VkPhysicalDeviceFeatures physical_device_features;
    vkGetPhysicalDeviceFeatures(r->physical_device, &physical_device_features);
    memset(&r->enabled_physical_device_features, 0,
           sizeof(r->enabled_physical_device_features));

    struct {
        const char *name;
        VkBool32 available, *enabled;
        bool required;
    } desired_features[] = {
        // clang-format off
        #define F(n, req) { \
            .name = #n, \
            .available = physical_device_features.n, \
            .enabled = &r->enabled_physical_device_features.n, \
            .required = req, \
        }
        F(depthClamp, true),
        F(fillModeNonSolid, true),
        F(geometryShader, true),
        F(occlusionQueryPrecise, true),
        F(samplerAnisotropy, false),
        F(shaderClipDistance, true),
        F(shaderTessellationAndGeometryPointSize, true),
        F(textureCompressionBC, false),
        F(wideLines, false),
        #undef F
        // clang-format on
    };

    bool all_required_features_available = true;
    for (int i = 0; i < ARRAY_SIZE(desired_features); i++) {
        if (desired_features[i].required &&
            desired_features[i].available != VK_TRUE) {
            fprintf(stderr,
                    "Error: Device does not support required feature %s\n",
                    desired_features[i].name);
            all_required_features_available = false;
        }
        *desired_features[i].enabled = desired_features[i].available;
    }

    if (!all_required_features_available) {
        error_setg(errp, "Device does not support required features");
        return false;
    }

    void *next_struct = NULL;

    VkPhysicalDeviceCustomBorderColorFeaturesEXT custom_border_features;
    if (r->custom_border_color_extension_enabled) {
        custom_border_features = (VkPhysicalDeviceCustomBorderColorFeaturesEXT){
            .sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT,
            .customBorderColors = VK_TRUE,
            .pNext = next_struct,
        };
        next_struct = &custom_border_features;
    }

    VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures demote_features;
    if (r->device_props.apiVersion >= VK_API_VERSION_1_3 ||
        r->demote_to_helper_extension_enabled) {
        VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures supported = {
            .sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES,
        };
        VkPhysicalDeviceFeatures2 features2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &supported,
        };

        vkGetPhysicalDeviceFeatures2(r->physical_device, &features2);
        if (supported.shaderDemoteToHelperInvocation) {
            demote_features =
                (VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures){
                    .sType =
                        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES,
                    .shaderDemoteToHelperInvocation = VK_TRUE,
                    .pNext = next_struct,
                };
            next_struct = &demote_features;
        }
    }

    VkDeviceCreateInfo device_create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_create_info,
        .pEnabledFeatures = &r->enabled_physical_device_features,
        .enabledExtensionCount = enabled_extension_names->len,
        .ppEnabledExtensionNames =
            &g_array_index(enabled_extension_names, const char *, 0),
        .pNext = next_struct,
    };

    result = vkCreateDevice(r->physical_device, &device_create_info, NULL,
                            &r->device);
    if (result != VK_SUCCESS) {
        error_setg(errp, "Failed to create logical device (%d)", result);
        return false;
    }

    vkGetDeviceQueue(r->device, indices.queue_family, 0, &r->queue);
    return true;
}

uint32_t pgraph_vk_get_memory_type(PGRAPHState *pg, uint32_t type_bits,
                                   VkMemoryPropertyFlags properties)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkPhysicalDeviceMemoryProperties prop;
    vkGetPhysicalDeviceMemoryProperties(r->physical_device, &prop);
    for (uint32_t i = 0; i < prop.memoryTypeCount; i++) {
        if ((prop.memoryTypes[i].propertyFlags & properties) == properties &&
            type_bits & (1 << i)) {
            return i;
        }
    }
    return 0xFFFFFFFF; // Unable to find memoryType
}

static bool init_allocator(PGRAPHState *pg, Error **errp)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    VkResult result;

    VmaAllocatorCreateInfo create_info = {
        .flags = (r->memory_budget_extension_enabled ?
                      VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT :
                      0),
        .vulkanApiVersion = r->vk_api_version,
        .instance = r->instance,
        .physicalDevice = r->physical_device,
        .device = r->device,
    };

    VmaVulkanFunctions vulkan_functions;
    VkResult res = vmaImportVulkanFunctionsFromVolk(&create_info, &vulkan_functions);
    if (res != VK_SUCCESS) {
        error_setg(errp, "vmaImportVulkanFunctionsFromVolk failed");
        return false;
    }
    create_info.pVulkanFunctions = &vulkan_functions;

    result = vmaCreateAllocator(&create_info, &r->allocator);
    if (result != VK_SUCCESS) {
        error_setg(errp, "vmaCreateAllocator failed");
        return false;
    }

    return true;
}

void pgraph_vk_init_instance(PGRAPHState *pg, Error **errp)
{
    if (create_instance(pg, errp) &&
        select_physical_device(pg, errp) &&
        create_logical_device(pg, errp) &&
        init_allocator(pg, errp)) {
        PGRAPHVkState *r = pg->vk_renderer_state;
        xemu_gpu_info_set_actual_device(&r->selected_device);
        return;
    }

    pgraph_vk_finalize_instance(pg);

    const char *msg = "Failed to initialize Vulkan renderer";
    if (*errp) {
        error_prepend(errp, "%s: ", msg);
    } else {
        error_setg(errp, "%s", msg);
    }
}

void pgraph_vk_finalize_instance(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->allocator != VK_NULL_HANDLE) {
        vmaDestroyAllocator(r->allocator);
        r->allocator = VK_NULL_HANDLE;
    }

    if (r->device != VK_NULL_HANDLE) {
        vkDestroyDevice(r->device, NULL);
        r->device = VK_NULL_HANDLE;
    }

    if (r->debug_messenger != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(r->instance, r->debug_messenger, NULL);
        r->debug_messenger = VK_NULL_HANDLE;
    }

    if (r->instance != VK_NULL_HANDLE) {
        vkDestroyInstance(r->instance, NULL);
        r->instance = VK_NULL_HANDLE;
    }

    volkFinalize();
}
