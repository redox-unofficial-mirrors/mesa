/*
 * Mesa 3-D graphics library
 *
 * Copyright (c) 2014 The Chromium OS Authors.
 * Copyright © 2011 Intel Corporation
 * Copyright © 2023 Redox OS Authors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER

 * DEALINGS IN THE SOFTWARE.
 */

#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <orbital.h>

#include "egl_dri2.h"
#include "eglglobals.h"
#include "kopper_interface.h"
#include "loader.h"

static __DRIimage *
redox_alloc_image(struct dri2_egl_display *dri2_dpy,
                  struct dri2_egl_surface *dri2_surf)
{
    return dri2_dpy->image->createImage(
       dri2_dpy->dri_screen_render_gpu, dri2_surf->base.Width,
       dri2_surf->base.Height, dri2_surf->visual, 0, NULL);
}

static void
redox_free_images(struct dri2_egl_surface *dri2_surf)
{
    struct dri2_egl_display *dri2_dpy =
       dri2_egl_display(dri2_surf->base.Resource.Display);

    if (dri2_surf->front) {
       dri2_dpy->image->destroyImage(dri2_surf->front);
       dri2_surf->front = NULL;
    }

    free(dri2_surf->swrast_device_buffer);
    dri2_surf->swrast_device_buffer = NULL;
}

static EGLBoolean
redox_swap_interval(_EGLDisplay *disp, _EGLSurface *surf, EGLint interval)
{
    // liborbital doesn't have a swap interval API, so we just
    // accept the value as required by the EGL spec.
    surf->SwapInterval = interval;
    return EGL_TRUE;
}

static EGLBoolean
redox_query_surface(_EGLDisplay *disp, _EGLSurface *surf, EGLint attribute,
                    EGLint *value)
{
    struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);

    switch (attribute) {
    case EGL_WIDTH:
        if (dri2_surf->orb_window) {
            *value = orb_window_width(dri2_surf->orb_window);
            return EGL_TRUE;
        }
        break;
    case EGL_HEIGHT:
        if (dri2_surf->orb_window) {
            *value = orb_window_height(dri2_surf->orb_window);
            return EGL_TRUE;
        }
        break;
    default:
        break;
    }

    // Fallback
    return _eglQuerySurface(disp, surf, attribute, value);
}

static bool
redox_set_shared_buffer_mode(_EGLDisplay *disp, _EGLSurface *surf, bool mode)
{
    // Redox does not support EGL_KHR_mutable_render_buffer
    return EGL_FALSE;
}

static _EGLSurface *
dri2_redox_create_surface(_EGLDisplay *disp, EGLint type,
                          _EGLConfig *conf, const EGLint *attrib_list)
{
    struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
    struct dri2_egl_config *dri2_conf = dri2_egl_config(conf);
    struct dri2_egl_surface *dri2_surf;
    const __DRIconfig *config;

    dri2_surf = calloc(1, sizeof *dri2_surf);
    if (!dri2_surf) {
       _eglError(EGL_BAD_ALLOC, "eglCreatePbufferSurface");
       return NULL;
    }

    if (!dri2_init_surface(&dri2_surf->base, disp, type, conf, attrib_list,
                           false, NULL))
       goto cleanup_surface;

    dri2_surf->orb_window = orb_window_new_flags(-1, -1, dri2_surf->base.Width, dri2_surf->base.Height, "EGL", ORB_WINDOW_ASYNC);
    if (!dri2_surf->orb_window) {
        _eglError(EGL_BAD_ALLOC, "Failed to create orbital window");
        goto cleanup_surface;
    }

    config = dri2_get_dri_config(dri2_conf, type, dri2_surf->base.GLColorspace);
    if (!config) {
       _eglError(EGL_BAD_MATCH,
                 "Unsupported surfacetype/colorspace configuration");
       goto cleanup_orbital;
    }

    dri2_surf->visual = dri2_image_format_for_pbuffer_config(dri2_dpy, config);
    if (dri2_surf->visual == __DRI_IMAGE_FORMAT_NONE)
       goto cleanup_orbital;

    if (!dri2_create_drawable(dri2_dpy, config, dri2_surf, dri2_surf))
       goto cleanup_orbital;

    return &dri2_surf->base;

cleanup_orbital:
    orb_window_destroy(dri2_surf->orb_window);
cleanup_surface:
    free(dri2_surf);
    return NULL;
}

static EGLBoolean
redox_destroy_surface(_EGLDisplay *disp, _EGLSurface *surf)
{
    struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
    struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);

    redox_free_images(dri2_surf);
    dri2_dpy->core->destroyDrawable(dri2_surf->dri_drawable);

    if (dri2_surf->orb_window) {
        orb_window_destroy(dri2_surf->orb_window);
        dri2_surf->orb_window = NULL;
    }

    dri2_fini_surface(surf);
    free(dri2_surf);
    return EGL_TRUE;
}

static _EGLSurface *
dri2_redox_create_pbuffer_surface(_EGLDisplay *disp, _EGLConfig *conf,
                                  const EGLint *attrib_list)
{
    return dri2_redox_create_surface(disp, EGL_PBUFFER_BIT, conf,
                                     attrib_list);
}

static _EGLSurface *
dri2_redox_create_window_surface(_EGLDisplay *disp, _EGLConfig *conf,
                                 void *native_window, const EGLint *attrib_list)
{
    struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
    struct dri2_egl_config *dri2_conf = dri2_egl_config(conf);
    struct dri2_egl_surface *dri2_surf;
    const __DRIconfig *config;

    if (!native_window) {
        _eglError(EGL_BAD_NATIVE_WINDOW, "dri2_redox_create_window_surface");
        return NULL;
    }

    dri2_surf = calloc(1, sizeof *dri2_surf);
    if (!dri2_surf) {
       _eglError(EGL_BAD_ALLOC, "dri2_redox_create_window_surface");
       return NULL;
    }

    if (!dri2_init_surface(&dri2_surf->base, disp, EGL_WINDOW_BIT, conf, attrib_list,
                           true, native_window)) {
        goto cleanup_surface;
    }

    dri2_surf->orb_window = orb_window_from_fd((int)native_window);
    dri2_surf->base.Width = orb_window_width(dri2_surf->orb_window);
    dri2_surf->base.Height = orb_window_height(dri2_surf->orb_window);

    config = dri2_get_dri_config(dri2_conf, EGL_WINDOW_BIT, dri2_surf->base.GLColorspace);
    if (!config) {
       _eglError(EGL_BAD_MATCH,
                 "Unsupported surfacetype/colorspace configuration");
       goto cleanup_surface;
    }

    if (!dri2_create_drawable(dri2_dpy, config, dri2_surf, dri2_surf)) {
        goto cleanup_surface;
    }

    return &dri2_surf->base;

cleanup_surface:
    free(dri2_surf);
    return NULL;
}


static void
redox_swrast_put_image(__DRIdrawable *draw, int op, int x, int y, int w,
                        int h, char *data, void *loaderPrivate)
{
    struct dri2_egl_surface *dri2_surf = loaderPrivate;

    if (op != __DRI_SWRAST_IMAGE_OP_SWAP || !dri2_surf || !dri2_surf->orb_window || !data) {
        return;
    }

    const int bpp = 4;
    char *dst_base = (char *)orb_window_data(dri2_surf->orb_window);

    if (!dst_base) {
        _eglLog(_EGL_WARNING, "redox_swrast_put_image: orb_window_data() returned NULL!");
        return;
    }

    int dst_window_width = orb_window_width(dri2_surf->orb_window);
    int dst_window_height = orb_window_height(dri2_surf->orb_window);
    int stride =  dst_window_width * 4;
    int copy_width_bytes = w * bpp;
    int dst_stride_bytes = dst_window_width * bpp;
    int x_offset_bytes = x * bpp;

    char *src_line = data;
    char *dst_line = dst_base;
    dst_line += x_offset_bytes;
    dst_line += y * dst_stride_bytes;

    if (copy_width_bytes > dst_stride_bytes - x_offset_bytes)
        copy_width_bytes = dst_stride_bytes - x_offset_bytes;

    if (h > dst_window_height - y)
        h = dst_window_height - y;

    if (copy_width_bytes <= 0 || h <= 0) {
        return;
    }

    for (; h > 0; h--) {
        memcpy(dst_line, src_line, copy_width_bytes);
        src_line += stride;
        dst_line += dst_stride_bytes;
    }
}

static void
redox_swrast_get_drawable_info(__DRIdrawable *draw, int *x, int *y, int *w, int *h,
                               void *loaderPrivate)
{
    struct dri2_egl_surface *dri2_surf = loaderPrivate;

    if (!dri2_surf->orb_window) {
        *x = *y = *w = *h = 0;
        return;
    }

    *x = orb_window_x(dri2_surf->orb_window);
    *y = orb_window_y(dri2_surf->orb_window);
    *w = orb_window_width(dri2_surf->orb_window);
    *h = orb_window_height(dri2_surf->orb_window);
}

static EGLBoolean
redox_swap_buffers(_EGLDisplay *disp, _EGLSurface *draw)
{
    struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
    struct dri2_egl_surface *dri2_surf = dri2_egl_surface(draw);

    if (dri2_dpy->kopper) {
        /* From the EGL 1.4 spec (page 52):
        *
        *     "The contents of ancillary buffers are always undefined
        *      after calling eglSwapBuffers."
        */
        dri2_dpy->kopper->swapBuffers(dri2_surf->dri_drawable,
                            __DRI2_FLUSH_INVALIDATE_ANCILLARY);
    } else if (!dri2_dpy->flush) {
        /* aka the swrast path, which does the swap in the gallium driver. */
        dri2_dpy->core->swapBuffers(dri2_surf->dri_drawable);
    } else {
        dri2_flush_drawable_for_swapbuffers(disp, draw);
    }

    if (dri2_surf->orb_window) {
        orb_window_sync(dri2_surf->orb_window);
    }

    return EGL_TRUE;
}

static EGLint
redox_query_buffer_age(_EGLDisplay *disp, _EGLSurface *surface)
{
    // liborbital does not track buffer age.
    return 0;
}

static const struct dri2_egl_display_vtbl dri2_redox_display_vtbl = {
    .create_window_surface = dri2_redox_create_window_surface,
    .create_pbuffer_surface = dri2_redox_create_pbuffer_surface,
    .destroy_surface = redox_destroy_surface,
    .create_image = dri2_create_image_khr,
    .swap_buffers = redox_swap_buffers,
    .swap_interval = redox_swap_interval,
    .query_buffer_age = redox_query_buffer_age,
    .query_surface = redox_query_surface,
    .get_dri_drawable = dri2_surface_get_dri_drawable,
    .set_shared_buffer_mode = redox_set_shared_buffer_mode,
};

static void
redox_flush_front_buffer(__DRIdrawable *driDrawable, void *loaderPrivate)
{
   //  syncing pbuffer, no op
}

static unsigned
redox_get_capability(void *loaderPrivate, enum dri_loader_cap cap)
{
    switch (cap) {
    case DRI_LOADER_CAP_FP16:
       return 1;
    default:
       return 0;
    }
}

static const __DRIkopperLoaderExtension kopper_loader_extension = {
    .base = {__DRI_KOPPER_LOADER, 1},
    .SetSurfaceCreateInfo = NULL,
};

static const __DRIswrastLoaderExtension swrast_loader_extension = {
    .base = {__DRI_SWRAST_LOADER, 1},
    .getDrawableInfo = redox_swrast_get_drawable_info,
    .putImage = redox_swrast_put_image,
    .getImage = NULL, // TODO
};

static const __DRIextension *redox_swrast_loader_extensions[] = {
    &swrast_loader_extension.base,
    &image_lookup_extension.base,
    &use_invalidate.base,
    &kopper_loader_extension.base,
    NULL,
};

static bool
redox_probe_device_sw(_EGLDisplay *disp)
{
    struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
    struct _egl_device *device = _eglFindDevice(dri2_dpy->fd_render_gpu, true);

    dri2_dpy->fd_render_gpu = -1;

    if (_eglHasAttrib(disp, EGL_DEVICE_EXT) && disp->Device != device) {
       return false;
    }

    disp->Device = device;
    assert(disp->Device);

    dri2_dpy->driver_name = strdup(disp->Options.Zink ? "zink" : "swrast");
    if (!dri2_dpy->driver_name)
       return false;

    if (!dri2_load_driver_swrast(disp)) {
       free(dri2_dpy->driver_name);
       dri2_dpy->driver_name = NULL;
       return false;
    }

    dri2_dpy->loader_extensions = redox_swrast_loader_extensions;

    return true;
}

static const struct {
   int format;
   int rgba_shifts[4];
   unsigned int rgba_sizes[4];
} redox_visuals[] = {
   {
      /* DRM_FORMAT_ARGB8888 (0x34325241) */
      0x34325241,
      /* Shifts for R, G, B, A */
      { 16, 8, 0, 24 },
      /* Sizes for R, G, B, A */
      { 8, 8, 8, 8 }
   },
   {
      /* DRM_FORMAT_XRGB8888 (0x34325258) */
      0x34325258,
      /* Shifts for R, G, B, A */
      { 16, 8, 0, 24 },
      /* Sizes for R, G, B, X */
      { 8, 8, 8, 0 }
   }
};

static EGLBoolean
dri2_add_window_configs_for_visuals(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   unsigned int config_count = 0;

   for (int i = 0; i < ARRAY_SIZE(redox_visuals); i++) {

      for (int j = 0; dri2_dpy->driver_configs[j]; j++) {

         const __DRIconfig *dri_conf = dri2_dpy->driver_configs[j];

         // Only create configs for 32-bit visuals
        //  if (dri_conf->modes->redMask != 0xFF0000 ||
        //      dri_conf->modes->greenMask != 0x00FF00 ||
        //      dri_conf->modes->blueMask != 0x0000FF) {
        //     continue;
        //  }

         const EGLint surface_type = EGL_WINDOW_BIT | EGL_PBUFFER_BIT;

         const EGLint config_attrs[] = {
            EGL_NATIVE_VISUAL_ID,   redox_visuals[i].format,
            EGL_NATIVE_VISUAL_TYPE, redox_visuals[i].format,
            EGL_NONE
         };

         struct dri2_egl_config *dri2_conf = dri2_add_config(
            disp,
            dri_conf,                 // The base driver config
            config_count + 1,         // The new EGLConfig ID
            surface_type,             // The supported surface types
            config_attrs,             // Our native visual attributes
            redox_visuals[i].rgba_shifts, // Our color channel layout
            redox_visuals[i].rgba_sizes   // Our color channel sizes
         );

         if (dri2_conf) {
            if (dri2_conf->base.ConfigID == config_count + 1)
               config_count++;
         }
      }
   }

   return (config_count != 0);
}

EGLBoolean
dri2_initialize_redox(_EGLDisplay *disp)
{
    const char *err;
    bool driver_loaded = false;
    struct dri2_egl_display *dri2_dpy = dri2_display_create();

    if (!dri2_dpy) {
        err = "dri2_display_create failed";
        goto cleanup;
    }

    disp->DriverData = (void *)dri2_dpy;

      driver_loaded = redox_probe_device_sw(disp);

    if (!driver_loaded) {
       err = "DRI2: failed to load driver";
       goto cleanup;
    }

    dri2_dpy->fd_display_gpu = dri2_dpy->fd_render_gpu;

    if (!dri2_create_screen(disp)) {
       err = "DRI2: failed to create screen";
       goto cleanup;
    }

    if (!dri2_setup_extensions(disp)) {
       err = "DRI2: failed to find required DRI extensions";
       goto cleanup;
    }

    dri2_setup_screen(disp);

    if (!dri2_add_pbuffer_configs_for_visuals(disp)) {
       err = "DRI2: failed to add pbuffer configs";
       goto cleanup;
    }

    if (!dri2_add_window_configs_for_visuals(disp)) {
        err = "DRI2: failed to add window configs";
        goto cleanup;
    }

    disp->Extensions.KHR_image = EGL_TRUE;
    disp->Extensions.KHR_partial_update = EGL_FALSE;
    disp->Extensions.REDOX_image_native_buffer = EGL_TRUE;

    dri2_dpy->vtbl = &dri2_redox_display_vtbl;

    return EGL_TRUE;

cleanup:
    dri2_display_destroy(disp);
    return _eglError(EGL_NOT_INITIALIZED, err);
}
