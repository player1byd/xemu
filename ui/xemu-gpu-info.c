/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "xemu-gpu-info.h"

#include <stdarg.h>

#ifndef G_GNUC_PRINTF
#define G_GNUC_PRINTF(format_index, first_arg) \
    __attribute__((format(printf, format_index, first_arg)))
#endif

typedef struct JsonBuffer {
    char *data;
    size_t length;
    size_t capacity;
    bool failed;
} JsonBuffer;

static PGRAPHVkDeviceRecord *last_inventory;
static size_t last_inventory_count;
static PGRAPHVkDeviceRecord last_actual_device;
static bool has_actual_device;

static bool json_reserve(JsonBuffer *buffer, size_t extra)
{
    if (buffer->failed || extra > SIZE_MAX - buffer->length - 1) {
        buffer->failed = true;
        return false;
    }
    size_t needed = buffer->length + extra + 1;
    if (needed <= buffer->capacity) {
        return true;
    }
    size_t capacity = buffer->capacity ? buffer->capacity : 512;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    char *data = realloc(buffer->data, capacity);
    if (data == NULL) {
        buffer->failed = true;
        return false;
    }
    buffer->data = data;
    buffer->capacity = capacity;
    return true;
}

static void json_append_n(JsonBuffer *buffer, const char *text, size_t length)
{
    if (!json_reserve(buffer, length)) {
        return;
    }
    memcpy(buffer->data + buffer->length, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
}

static void json_append(JsonBuffer *buffer, const char *text)
{
    json_append_n(buffer, text, strlen(text));
}

static void json_append_format(JsonBuffer *buffer, const char *format, ...)
    G_GNUC_PRINTF(2, 3);

static void json_append_format(JsonBuffer *buffer, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    int length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (length < 0 || !json_reserve(buffer, (size_t)length)) {
        buffer->failed = true;
        va_end(args);
        return;
    }
    vsnprintf(buffer->data + buffer->length,
              buffer->capacity - buffer->length, format, args);
    buffer->length += length;
    va_end(args);
}

static void json_append_string(JsonBuffer *buffer, const char *text)
{
    if (text == NULL) {
        json_append(buffer, "null");
        return;
    }

    json_append(buffer, "\"");
    for (const unsigned char *cursor = (const unsigned char *)text;
         *cursor != '\0'; cursor++) {
        switch (*cursor) {
        case '\"':
            json_append(buffer, "\\\"");
            break;
        case '\\':
            json_append(buffer, "\\\\");
            break;
        case '\b':
            json_append(buffer, "\\b");
            break;
        case '\f':
            json_append(buffer, "\\f");
            break;
        case '\n':
            json_append(buffer, "\\n");
            break;
        case '\r':
            json_append(buffer, "\\r");
            break;
        case '\t':
            json_append(buffer, "\\t");
            break;
        default:
            if (*cursor < 0x20) {
                json_append_format(buffer, "\\u%04x", *cursor);
            } else {
                json_append_n(buffer, (const char *)cursor, 1);
            }
            break;
        }
    }
    json_append(buffer, "\"");
}

static const char *device_type_name(PGRAPHVkDeviceType type)
{
    switch (type) {
    case PGRAPH_VK_DEVICE_TYPE_INTEGRATED:
        return "integrated";
    case PGRAPH_VK_DEVICE_TYPE_DISCRETE:
        return "discrete";
    case PGRAPH_VK_DEVICE_TYPE_VIRTUAL:
        return "virtual";
    case PGRAPH_VK_DEVICE_TYPE_CPU:
        return "cpu";
    default:
        return "other";
    }
}

static const char *selection_source_name(XemuGpuSelectionSource source)
{
    switch (source) {
    case XEMU_GPU_SELECTION_SOURCE_CONFIG_UUID:
        return "saved_uuid";
    case XEMU_GPU_SELECTION_SOURCE_LEGACY:
        return "legacy_name";
    case XEMU_GPU_SELECTION_SOURCE_CLI:
        return "command_line";
    default:
        return "automatic";
    }
}

static const char *document_state_name(XemuGpuInfoState state)
{
    switch (state) {
    case XEMU_GPU_INFO_INITIALIZED:
        return "initialized";
    case XEMU_GPU_INFO_FAILED:
        return "failed";
    default:
        return "inventory";
    }
}

static const char *presentation_mode_name(XemuGpuPresentationMode mode)
{
    switch (mode) {
    case XEMU_GPU_PRESENTATION_SHARED:
        return "shared";
    case XEMU_GPU_PRESENTATION_HOST_COPY:
        return "host_copy";
    default:
        return "unknown";
    }
}

static void append_device(JsonBuffer *buffer,
                          const PGRAPHVkDeviceRecord *device)
{
    char device_uuid[PGRAPH_VK_DEVICE_UUID_STRING_SIZE];
    char driver_uuid[PGRAPH_VK_DEVICE_UUID_STRING_SIZE];
    pgraph_vk_device_uuid_format(device->device_uuid, device_uuid);
    pgraph_vk_device_uuid_format(device->driver_uuid, driver_uuid);

    json_append(buffer, "{\"name\":");
    json_append_string(buffer, device->name);
    json_append(buffer, ",\"device_uuid\":");
    json_append_string(buffer, device_uuid);
    json_append(buffer, ",\"driver_uuid\":");
    json_append_string(buffer, driver_uuid);
    json_append_format(buffer,
                       ",\"vendor_id\":%" PRIu32
                       ",\"device_id\":%" PRIu32
                       ",\"api_version\":%" PRIu32
                       ",\"driver_version\":%" PRIu32,
                       device->vendor_id, device->device_id,
                       device->api_version, device->driver_version);
    json_append(buffer, ",\"type\":");
    json_append_string(buffer, device_type_name(device->type));
    json_append(buffer, ",\"compatible\":");
    json_append(buffer, device->renderer_supported ? "true" : "false");
    json_append(buffer, ",\"rejection_reason\":");
    json_append_string(buffer, device->rejection_reason);
    json_append(buffer, "}");
}

static void append_request(JsonBuffer *buffer,
                           const XemuGpuLaunchRequest *request)
{
    char selector[sizeof("uuid:") + PGRAPH_VK_DEVICE_UUID_STRING_SIZE];
    const char *selector_text = "auto";
    if (request != NULL) {
        if (request->selection.kind == PGRAPH_VK_SELECTION_UUID) {
            memcpy(selector, "uuid:", sizeof("uuid:") - 1);
            pgraph_vk_device_uuid_format(
                request->selection.device_uuid,
                selector + sizeof("uuid:") - 1);
            selector_text = selector;
        } else if (request->selection.kind ==
                   PGRAPH_VK_SELECTION_LEGACY_NAME) {
            selector_text = request->selection.legacy_name;
        }
    }

    json_append(buffer, "{\"source\":");
    json_append_string(buffer, selection_source_name(
        request ? request->selection_source :
                  XEMU_GPU_SELECTION_SOURCE_AUTOMATIC));
    json_append(buffer, ",\"selector\":");
    json_append_string(buffer, selector_text);
    json_append(buffer, ",\"strict\":");
    json_append(buffer, request && request->strict ? "true" : "false");
    json_append(buffer, "}");
}

char *xemu_gpu_info_render_json(const XemuGpuInfoDocument *document)
{
    if (document == NULL ||
        (document->device_count != 0 && document->devices == NULL)) {
        return NULL;
    }

    JsonBuffer buffer = { 0 };
    json_append(&buffer, "{\"schema_version\":1,\"state\":");
    json_append_string(&buffer, document_state_name(document->state));
    json_append(&buffer, ",\"request\":");
    append_request(&buffer, document->request);
    json_append(&buffer, ",\"requested_backend\":");
    json_append_string(&buffer, document->requested_backend);
    json_append(&buffer, ",\"actual_backend\":");
    json_append_string(&buffer, document->actual_backend);
    json_append(&buffer, ",\"devices\":[");
    for (size_t i = 0; i < document->device_count; i++) {
        if (i != 0) {
            json_append(&buffer, ",");
        }
        append_device(&buffer, &document->devices[i]);
    }
    json_append(&buffer, "],\"actual_device\":");
    if (document->actual_device != NULL) {
        append_device(&buffer, document->actual_device);
    } else {
        json_append(&buffer, "null");
    }
    json_append(&buffer, ",\"fallback\":{\"used\":");
    json_append(&buffer, document->fallback_used ? "true" : "false");
    json_append(&buffer, ",\"reason\":");
    json_append_string(&buffer, document->fallback_reason);
    json_append(&buffer, "},\"presentation\":{\"mode\":");
    json_append_string(&buffer,
                       presentation_mode_name(document->presentation_mode));
    json_append(&buffer, ",\"gl_vendor\":");
    json_append_string(&buffer, document->presentation_vendor);
    json_append(&buffer, ",\"gl_renderer\":");
    json_append_string(&buffer, document->presentation_renderer);
    json_append(&buffer, "},\"error\":");
    json_append_string(&buffer, document->error_message);
    json_append(&buffer, "}\n");

    if (buffer.failed) {
        free(buffer.data);
        return NULL;
    }
    return buffer.data;
}

static void set_file_error(char *error, size_t error_size,
                           const char *operation, const char *path)
{
    if (error != NULL && error_size != 0) {
        snprintf(error, error_size, "%s '%s': %s", operation, path,
                 strerror(errno));
    }
}

bool xemu_gpu_info_write_atomic(const char *path, const char *contents,
                                char *error, size_t error_size)
{
    if (path == NULL || path[0] == '\0' || contents == NULL) {
        errno = EINVAL;
        set_file_error(error, error_size, "invalid output path", path ?: "");
        return false;
    }

    size_t temporary_length = strlen(path) + sizeof(".tmp");
    char *temporary = malloc(temporary_length);
    if (temporary == NULL) {
        errno = ENOMEM;
        set_file_error(error, error_size, "allocate temporary path", path);
        return false;
    }
    snprintf(temporary, temporary_length, "%s.tmp", path);

#ifdef XEMU_GPU_INFO_STANDALONE
    FILE *file = fopen(temporary, "wb");
#else
    FILE *file = qemu_fopen(temporary, "wb");
#endif
    if (file == NULL) {
        set_file_error(error, error_size, "open temporary file", temporary);
        free(temporary);
        return false;
    }

    bool success = true;
    size_t length = strlen(contents);
    if (fwrite(contents, 1, length, file) != length) {
        set_file_error(error, error_size, "write temporary file", temporary);
        success = false;
    }
    if (success && fflush(file) != 0) {
        set_file_error(error, error_size, "flush temporary file", temporary);
        success = false;
    }
    if (success && fsync(fileno(file)) != 0) {
        set_file_error(error, error_size, "sync temporary file", temporary);
        success = false;
    }
    if (fclose(file) != 0 && success) {
        set_file_error(error, error_size, "close temporary file", temporary);
        success = false;
    }

    if (success) {
#ifdef _WIN32
        if (!MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING |
                                          MOVEFILE_WRITE_THROUGH)) {
            errno = EIO;
            set_file_error(error, error_size, "replace output file", path);
            success = false;
        }
#else
        if (rename(temporary, path) != 0) {
            set_file_error(error, error_size, "replace output file", path);
            success = false;
        }
#endif
    }
    if (!success) {
#ifdef XEMU_GPU_INFO_STANDALONE
        remove(temporary);
#else
        qemu_unlink(temporary);
#endif
    }
    free(temporary);
    return success;
}

void xemu_gpu_info_record_inventory(const PGRAPHVkDeviceRecord *devices,
                                    size_t count)
{
    PGRAPHVkDeviceRecord *copy = NULL;
    if (count != 0) {
        if (count > SIZE_MAX / sizeof(*copy)) {
            return;
        }
        copy = malloc(sizeof(*copy) * count);
        if (copy == NULL) {
            return;
        }
        memcpy(copy, devices, sizeof(*copy) * count);
    }
    free(last_inventory);
    last_inventory = copy;
    last_inventory_count = count;
}

const PGRAPHVkDeviceRecord *xemu_gpu_info_get_inventory(size_t *count)
{
    if (count != NULL) {
        *count = last_inventory_count;
    }
    return last_inventory;
}

const PGRAPHVkDeviceRecord *xemu_gpu_info_get_actual_device(void)
{
    return has_actual_device ? &last_actual_device : NULL;
}

void xemu_gpu_info_set_actual_device(const PGRAPHVkDeviceRecord *device)
{
    has_actual_device = device != NULL;
    if (device != NULL) {
        last_actual_device = *device;
    }
}

static bool write_runtime_document(XemuGpuInfoState state,
                                   const PGRAPHVkDeviceRecord *actual_device,
                                   const char *actual_backend,
                                   XemuGpuPresentationMode mode,
                                   const char *message, bool fallback_used,
                                   const char *fallback_reason)
{
    const XemuGpuLaunchRequest *request = xemu_gpu_launch_request_get();
    if (request->info_path == NULL) {
        return true;
    }
    XemuGpuInfoDocument document = {
        .state = state,
        .request = request,
        .devices = last_inventory,
        .device_count = last_inventory_count,
        .actual_device = actual_device,
        .requested_backend = "Vulkan",
        .actual_backend = actual_backend,
        .fallback_used = fallback_used,
        .fallback_reason = fallback_reason,
        .presentation_mode = mode,
        .error_message = message,
    };
    char *json = xemu_gpu_info_render_json(&document);
    if (json == NULL) {
        fprintf(stderr, "GPU status output failed: could not allocate JSON\n");
        return false;
    }
    char file_error[512] = { 0 };
    bool success = xemu_gpu_info_write_atomic(request->info_path, json,
                                              file_error,
                                              sizeof(file_error));
    free(json);
    if (!success) {
        fprintf(stderr, "GPU status output failed: %s\n", file_error);
    }
    return success;
}

bool xemu_gpu_info_record_initialized(const PGRAPHVkDeviceRecord *device,
                                      const char *actual_backend,
                                      XemuGpuPresentationMode mode,
                                      bool fallback_used,
                                      const char *fallback_reason)
{
    xemu_gpu_info_set_actual_device(device);
    return write_runtime_document(XEMU_GPU_INFO_INITIALIZED, device,
                                  actual_backend, mode, NULL, fallback_used,
                                  fallback_reason);
}

bool xemu_gpu_info_record_presentation(XemuGpuPresentationMode mode,
                                       const char *vendor,
                                       const char *renderer)
{
    const XemuGpuLaunchRequest *request = xemu_gpu_launch_request_get();
    if (request->info_path == NULL) {
        return true;
    }
    XemuGpuInfoDocument document = {
        .state = XEMU_GPU_INFO_INITIALIZED,
        .request = request,
        .devices = last_inventory,
        .device_count = last_inventory_count,
        .actual_device = xemu_gpu_info_get_actual_device(),
        .requested_backend = "Vulkan",
        .actual_backend = "Vulkan",
        .presentation_mode = mode,
        .presentation_vendor = vendor,
        .presentation_renderer = renderer,
    };
    char *json = xemu_gpu_info_render_json(&document);
    if (json == NULL) {
        return false;
    }
    char file_error[512] = { 0 };
    bool success = xemu_gpu_info_write_atomic(request->info_path, json,
                                              file_error,
                                              sizeof(file_error));
    free(json);
    if (!success) {
        fprintf(stderr, "GPU presentation output failed: %s\n", file_error);
    }
    return success;
}

bool xemu_gpu_info_record_failure(const char *actual_backend,
                                  const char *message, bool fallback_used,
                                  const char *fallback_reason)
{
    return write_runtime_document(XEMU_GPU_INFO_FAILED, NULL, actual_backend,
                                  XEMU_GPU_PRESENTATION_UNKNOWN, message,
                                  fallback_used, fallback_reason);
}
