/*
 * wlrsrc - Wayland wlroots screencopy GStreamer source
 * Copyright (C) 2025 Kaliban <Callyth@users.noreply.github.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <gst/base/gstpushsrc.h>
#include <gst/gst.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/gstmemory.h>
#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <gbm.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/memfd.h>
#include <iostream>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <sys/syscall.h>
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"

#ifndef PACKAGE
#define PACKAGE "wlrsrc"
#endif

GST_DEBUG_CATEGORY_STATIC(wlr_src_debug);
#define GST_CAT_DEFAULT wlr_src_debug

enum
{
    PROP_0,
    PROP_DMABUF,
    PROP_SHOW_CURSOR,
};

typedef struct _GstWlrSrc
{
    GstPushSrc parent;
    struct wl_shm *shm = NULL;
    struct wl_shm_pool *pool = NULL;
    struct zwlr_screencopy_manager_v1 *manager = NULL;
    struct wl_output *output = NULL;
    struct wl_buffer *wlbuf = NULL;
    struct wl_display *display = NULL;
    struct wl_registry *registry = NULL;
    struct zwlr_screencopy_frame_v1 *frame = NULL;
    struct zwp_linux_dmabuf_v1 *dmabuf = NULL;
    struct zwp_linux_buffer_params_v1 *params = NULL;
    GstAllocator *dmabuf_alloc = NULL;
    gbm_device *gbm = NULL;
    gbm_bo *bo = NULL;
    void *shm_map = NULL;
    GstMapInfo info;
    GstSegment segment;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t buffer_size;
    uint32_t format;
    bool got_ready = false;
    int fd = -1;
    int drm_fd = -1;
    bool is_dmabuf;
    int show_cursor = 1;
} GstWlrSrc;

typedef struct _GstWlrSrcClass
{
    GstPushSrcClass parent_class;
} GstWlrSrcClass;

#define GST_TYPE_WLR_SRC (gst_wlr_src_get_type())
G_DEFINE_TYPE(GstWlrSrc, gst_wlr_src, GST_TYPE_PUSH_SRC)

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS("video/x-raw(memory:DMABuf),format=BGRx,width=[1,2147483646],height=[1,2147483646],framerate=[0/1,2147483647/1];video/x-raw,format=BGRx,width=[1,2147483646],height=[1,2147483646],framerate=[0/1,2147483647/1]"));

static void gst_wlr_src_set_property(GObject *object, guint prop_id,
                                     const GValue *value, GParamSpec *pspec)
{
    GstWlrSrc *self = (GstWlrSrc *)object;

    switch (prop_id)
    {
    case PROP_DMABUF:
        self->is_dmabuf = g_value_get_boolean(value);
        break;
    case PROP_SHOW_CURSOR:
        self->show_cursor = g_value_get_boolean(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void gst_wlr_src_get_property(GObject *object, guint prop_id,
                                     GValue *value, GParamSpec *pspec)
{
    GstWlrSrc *self = (GstWlrSrc *)object;

    switch (prop_id)
    {
    case PROP_DMABUF:
        g_value_set_boolean(value, self->is_dmabuf);
        break;
    case PROP_SHOW_CURSOR:
        g_value_set_boolean(value, (bool)self->show_cursor);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void param_created(void *data, zwp_linux_buffer_params_v1 *param, wl_buffer *buffer)
{
    GstWlrSrc *self = (GstWlrSrc *)data;
    if (self->wlbuf)
    {
        wl_buffer_destroy(self->wlbuf);
    }
    self->wlbuf = buffer;
    zwlr_screencopy_frame_v1_copy(self->frame, self->wlbuf);
}

static void param_failed(void *data, zwp_linux_buffer_params_v1 *param)
{
    GstWlrSrc *self = (GstWlrSrc *)data;
    GST_ERROR_OBJECT(self, "Failed to get buffer");
}

static const struct zwp_linux_buffer_params_v1_listener param_listener = {
    .created = param_created,
    .failed = param_failed};

static int make_shm_fd(size_t size, void **out_map)
{
    int fd = syscall(SYS_memfd_create, "wlrsrc", MFD_CLOEXEC);
    if (fd < 0)
        return -1;
    if (ftruncate(fd, size) < 0)
    {
        close(fd);
        return -1;
    }
    void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED)
    {
        close(fd);
        return -1;
    }
    *out_map = map;
    return fd;
}

static bool find_render_node(char *render_node, size_t max_length)
{
    bool result = false;
    drmDevicePtr devices[64];

    int n = drmGetDevices2(0, devices, (int)(sizeof(devices) / sizeof(devices[0])));
    for (int i = 0; i < n; i++)
    {
        drmDevice *dev = devices[i];
        if (!(dev->available_nodes & (1 << DRM_NODE_RENDER)))
        {
            continue;
        }
        strncpy(render_node, dev->nodes[DRM_NODE_RENDER], max_length - 1);
        render_node[max_length - 1] = '\0';
        result = true;
        break;
    }
    drmFreeDevices(devices, n);
    return result;
}

static void set_caps(GstWlrSrc *self)
{
    self->buffer_size = self->stride * self->height;
    GstCaps *caps = gst_caps_new_simple(
        "video/x-raw",
        "format", G_TYPE_STRING, "BGRx",
        "width", G_TYPE_INT, self->width,
        "height", G_TYPE_INT, self->height,
        "framerate", GST_TYPE_FRACTION, 0, 1,
        NULL);
    gst_base_src_set_caps(GST_BASE_SRC(self), caps);
    gst_caps_unref(caps);
}

static void frame_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t format, uint32_t width, uint32_t height, uint32_t stride)
{
    GstWlrSrc *self = (GstWlrSrc *)data;
    if (!self->is_dmabuf && !self->format)
    {
        self->format = format;
        self->width = width;
        self->height = height;
        self->stride = stride;
        set_caps(self);
        GST_DEBUG_OBJECT(self, "Frame width=%u height=%u", self->width, self->height);
        self->fd = make_shm_fd(self->buffer_size, &self->shm_map);
        if (!self->fd)
        {
            GST_ERROR_OBJECT(self, "Failed to create fd");
            return;
        }
        self->pool = wl_shm_create_pool(self->shm, self->fd, self->buffer_size);
        if (!self->pool)
        {
            GST_ERROR_OBJECT(self, "Failed to create pool");
            return;
        }
        self->wlbuf = wl_shm_pool_create_buffer(self->pool, 0, self->width, self->height, self->stride, self->format);
        if (!self->wlbuf)
        {
            GST_ERROR_OBJECT(self, "Failed to create wlbuf");
            return;
        }
    }
}

static void frame_flags(void *data, struct zwlr_screencopy_frame_v1 *frame,
                        uint32_t flags)
{
    //
}

static void frame_failed(void *data, struct zwlr_screencopy_frame_v1 *frame)
{
    //
}

static void frame_damage(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    //
}

static void frame_linux_dmabuf(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t format, uint32_t width, uint32_t height)

{
    GstWlrSrc *self = (GstWlrSrc *)data;
    if (self->is_dmabuf && !self->format)
    {
        self->format = format;
        self->width = width;
        self->height = height;
        GST_DEBUG_OBJECT(self, "Frame width=%u height=%u", self->width, self->height);
        if (!self->bo)
        {
            self->bo = gbm_bo_create(self->gbm, self->width, self->height, self->format, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
            if (!self->bo)
            {
                GST_ERROR_OBJECT(self, "Failed to create GBM BO");
                return;
            }
            self->stride = gbm_bo_get_stride(self->bo);
            set_caps(self);
            self->fd = gbm_bo_get_fd(self->bo);
            if (self->fd < 0)
            {
                GST_ERROR_OBJECT(self, "Failed to get GBM FD");
                return;
            }
            self->dmabuf_alloc = gst_dmabuf_allocator_new();
            if (!self->dmabuf_alloc)
            {
                GST_ERROR_OBJECT(self, "ailed to set dmabuf alloc");
                return;
            }
        }
    }
}

static void frame_buffer_done(void *data, struct zwlr_screencopy_frame_v1 *frame)
{
    GstWlrSrc *self = (GstWlrSrc *)data;
    if (self->is_dmabuf)
    {

        if (self->params)
        {
            zwp_linux_buffer_params_v1_destroy(self->params);
        }
        self->params = zwp_linux_dmabuf_v1_create_params(self->dmabuf);

        uint32_t offset = gbm_bo_get_offset(self->bo, 0);
        self->stride = gbm_bo_get_stride(self->bo);
        uint64_t modifier = gbm_bo_get_modifier(self->bo);
        zwp_linux_buffer_params_v1_add(self->params, self->fd, 0, offset, self->stride, modifier >> 32, modifier & 0xffffffff);
        zwp_linux_buffer_params_v1_add_listener(self->params, &param_listener, self);
        zwp_linux_buffer_params_v1_create(self->params, self->width, self->height, self->format, 0);
    }
    else
    {
        zwlr_screencopy_frame_v1_copy(self->frame, self->wlbuf);
    }
}

static void frame_ready(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec)
{
    GstWlrSrc *self = (GstWlrSrc *)data;
    self->got_ready = true;
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = frame_buffer,
    .flags = frame_flags,
    .ready = frame_ready,
    .failed = frame_failed,
    .damage = frame_damage,
    .linux_dmabuf = frame_linux_dmabuf,
    .buffer_done = frame_buffer_done};
static void dmabuf_format(void *data, zwp_linux_dmabuf_v1 *dmabuf, uint32_t format)
{
    // Is this used?
}

static void dmabuf_modifier(void *data, zwp_linux_dmabuf_v1 *dmabuf, uint32_t format, uint32_t modifier_hi, uint32_t modifier_low) {
    // Is this used?
};

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
    .format = dmabuf_format,
    .modifier = dmabuf_modifier};

void registory_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    GstWlrSrc *self = (GstWlrSrc *)data;
    if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0 && self->is_dmabuf)
    {
        self->dmabuf = (struct zwp_linux_dmabuf_v1 *)wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, version);
        zwp_linux_dmabuf_v1_add_listener(self->dmabuf, &dmabuf_listener, self);
        wl_display_roundtrip(self->display);
    }
    else if (strcmp(interface, "wl_shm") == 0 && !self->is_dmabuf)
    {
        self->shm = (struct wl_shm *)wl_registry_bind(registry, name, &wl_shm_interface, version);
    }
    else if (strcmp(interface, "zwlr_screencopy_manager_v1") == 0)
    {
        self->manager = (struct zwlr_screencopy_manager_v1 *)wl_registry_bind(registry, name, &zwlr_screencopy_manager_v1_interface, version);
    }
    else if (strcmp(interface, "wl_output") == 0)
    {
        self->output = (struct wl_output *)wl_registry_bind(registry, name, &wl_output_interface, version);
    }
};

static void gst_wlr_src_init(GstWlrSrc *self)
{
    self->show_cursor = 1;
    gst_segment_init(&self->segment, GST_FORMAT_TIME);
    gst_base_src_set_format(GST_BASE_SRC(self), GST_FORMAT_TIME);
    gst_base_src_set_live(GST_BASE_SRC(self), TRUE);
    gst_base_src_set_do_timestamp(GST_BASE_SRC(self), TRUE);
}

static gboolean gst_wlr_src_start(GstBaseSrc *src)
{
    GstWlrSrc *self = (GstWlrSrc *)src;
    if (self->is_dmabuf)
    {
        GST_DEBUG_OBJECT(self, "dmabuf mode");

        char render_node[256];
        if (!find_render_node(render_node, sizeof(render_node)))
        {
            GST_ERROR_OBJECT(self, "failed to find a DRM render node");
            return FALSE;
        }
        GST_DEBUG_OBJECT(self, "Render node=%s", render_node);
        self->drm_fd = open(render_node, O_RDWR);
        self->gbm = gbm_create_device(self->drm_fd);
        if (!self->gbm)
        {
            GST_ERROR_OBJECT(self, "Failed to create GBM Device");
            return FALSE;
        }
    }
    else
    {
        GST_DEBUG_OBJECT(self, "shm mode");
    }

    self->display = wl_display_connect(NULL);
    if (!self->display)
    {
        GST_ERROR_OBJECT(self, "Failed to connect to Wayland display");
        return FALSE;
    }
    self->registry = wl_display_get_registry(self->display);
    if (!self->registry)
    {
        GST_ERROR_OBJECT(self, "Failed to get registry");
        return FALSE;
    }
    static const struct wl_registry_listener registry_listener = {
        .global = registory_global,
        .global_remove = NULL,
    };
    wl_registry_add_listener(self->registry, &registry_listener, self);
    wl_display_roundtrip(self->display);
    return TRUE;
}

static GstFlowReturn gst_wlr_src_create(GstPushSrc *src, GstBuffer **buf)
{
    GstWlrSrc *self = (GstWlrSrc *)src;
    self->got_ready = FALSE;

    self->frame = zwlr_screencopy_manager_v1_capture_output(self->manager, self->show_cursor, self->output);
    zwlr_screencopy_frame_v1_add_listener(self->frame, &frame_listener, self);

    while (!self->got_ready)
    {
        if (wl_display_dispatch(self->display) == -1)
            break;
    }

    GstBuffer *buffer = NULL;
    if (self->is_dmabuf)
    {
        buffer = gst_buffer_new();
        int gst_fd = dup(self->fd);
        GstMemory *mem = gst_dmabuf_allocator_alloc_with_flags(self->dmabuf_alloc, gst_fd, self->buffer_size, GST_FD_MEMORY_FLAG_NONE);
        gst_buffer_append_memory(buffer, mem);
    }
    else
    {
        buffer = gst_buffer_new_and_alloc(self->buffer_size);
        if (gst_buffer_map(buffer, &self->info, GST_MAP_WRITE))
        {
            memcpy(self->info.data, self->shm_map, self->buffer_size);
            gst_buffer_unmap(buffer, &self->info);
        }
    }

    *buf = buffer;
    zwlr_screencopy_frame_v1_destroy(self->frame);
    self->frame = NULL;
    return GST_FLOW_OK;
}
static gboolean gst_wlr_src_close(GstBaseSrc *src)
{
    GstWlrSrc *self = (GstWlrSrc *)src;
    if (self->frame)
    {
        zwlr_screencopy_frame_v1_destroy(self->frame);
    }
    if (self->wlbuf)
    {
        wl_buffer_destroy(self->wlbuf);
    }
    if (self->manager)
    {
        zwlr_screencopy_manager_v1_destroy(self->manager);
    }
    if (self->is_dmabuf)
    {
        if (self->bo)
        {
            gbm_bo_destroy(self->bo);
        }
        if (self->dmabuf_alloc)
        {
            gst_object_unref(self->dmabuf_alloc);
        }
        if (self->dmabuf)
        {
            zwp_linux_dmabuf_v1_destroy(self->dmabuf);
        }
        if (self->gbm)
        {
            gbm_device_destroy(self->gbm);
        }
    }
    else
    {
        if (self->pool)
        {
            wl_shm_pool_destroy(self->pool);
        }
        if (self->shm_map)
        {
            munmap(self->shm_map, self->buffer_size);
        }
        if (self->shm)
        {
            wl_shm_destroy(self->shm);
        }
    };
    if (self->registry)
    {
        wl_registry_destroy(self->registry);
    }
    if (self->display)
    {
        wl_display_disconnect(self->display);
    }
    if (self->fd >= 0)
    {
        close(self->fd);
    }
    if (self->drm_fd >= 0)
    {
        close(self->drm_fd);
    }
    return TRUE;
}

static void gst_wlr_src_class_init(GstWlrSrcClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    GstPushSrcClass *pushsrc_class = GST_PUSH_SRC_CLASS(klass);
    GstBaseSrcClass *basesrc_class = GST_BASE_SRC_CLASS(klass);
    basesrc_class->start = gst_wlr_src_start;
    basesrc_class->stop = gst_wlr_src_close;
    pushsrc_class->create = gst_wlr_src_create;
    gobject_class->set_property = gst_wlr_src_set_property;
    gobject_class->get_property = gst_wlr_src_get_property;
    g_object_class_install_property(
        gobject_class,
        PROP_DMABUF,
        g_param_spec_boolean(
            "dmabuf", "Use DMABUF",
            "Use Linux DMABUF or SHM",
            FALSE, G_PARAM_READWRITE));
    g_object_class_install_property(
        gobject_class,
        PROP_SHOW_CURSOR,
        g_param_spec_boolean(
            "show-cursor", "Show cursor",
            "Capture mouse cursor",
            TRUE, G_PARAM_READWRITE));

    gst_element_class_set_static_metadata(GST_ELEMENT_CLASS(klass), "wlroots video source", "Source/Video", "Creates a video stream", "Kaliban <Callyth@users.noreply.github.com>");
    gst_element_class_add_static_pad_template(GST_ELEMENT_CLASS(klass), &src_template);
}

extern "C" gboolean wlrsrc_init(GstPlugin *plugin)
{
    GST_DEBUG_CATEGORY_INIT(wlr_src_debug, "wlrsrc", 0, "Wayland wlroots video source");
    return gst_element_register(plugin, "wlrsrc", GST_RANK_NONE, GST_TYPE_WLR_SRC);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    wlrsrc,
    "Wayland video input using zwlr_screencopy_manager_v1 and zwp_linux_dmabuf_v1",
    wlrsrc_init,
    "0.1",
    "LGPL",
    "Wlrsrc",
    "https://github.com/gst-wlrsrc")
