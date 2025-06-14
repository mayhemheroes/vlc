/**
 * @file x11.c
 * @brief X C Bindings video output module for VLC media player
 */
/*****************************************************************************
 * Copyright © 2009 Rémi Denis-Courmont
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h>
#include <assert.h>

#include <xcb/xcb.h>
#include <xcb/shm.h>

#include <vlc_common.h>
#include <vlc_fs.h>
#include <vlc_plugin.h>
#include <vlc_vout_display.h>

#include "pictures.h"
#include "events.h"

typedef struct vout_display_sys_t
{
    xcb_connection_t *conn;

    xcb_window_t window; /* drawable X window */
    xcb_gcontext_t gc; /* context to put images */
    xcb_shm_seg_t segment; /**< shared memory segment XID */
    bool attached;
    uint8_t depth; /* useful bits per pixel */
    video_format_t fmt;
} vout_display_sys_t;

static void Prepare(vout_display_t *vd, picture_t *pic,
                    const struct vlc_render_subpicture *subpic,
                    vlc_tick_t date)
{
    vout_display_sys_t *sys = vd->sys;
    const picture_buffer_t *buf = pic->p_sys;
    xcb_connection_t *conn = sys->conn;

    sys->attached = false;

    if (sys->segment == 0)
        return; /* SHM extension not supported */
    if (buf->fd == -1)
        return; /* not a shareable picture buffer */

    int fd = vlc_dup(buf->fd);
    if (fd == -1)
        return;

    xcb_void_cookie_t c = xcb_shm_attach_fd_checked(conn, sys->segment, fd, 1);
    xcb_generic_error_t *e = xcb_request_check(conn, c);
    if (e != NULL) /* attach failure (likely remote access) */
    {
        free(e);
        return;
    }

    sys->attached = true;
    (void) subpic; (void) date;
}

/**
 * Sends an image to the X server.
 */
static void Display (vout_display_t *vd, picture_t *pic)
{
    vout_display_sys_t *sys = vd->sys;
    xcb_connection_t *conn = sys->conn;
    const picture_buffer_t *buf = pic->p_sys;
    xcb_shm_seg_t segment = sys->segment;
    xcb_void_cookie_t ck;

    vlc_xcb_Manage(vd->obj.logger, sys->conn);

    /* Black out the borders */
    xcb_rectangle_t rectv[4], *rect;
    unsigned int rectc = 0;

    if (vd->place->x > 0) {
        rect = &rectv[rectc++];
        rect->x = 0;
        rect->y = 0;
        rect->width = vd->place->x;
        rect->height = vd->cfg->display.height;
    }
    if (vd->place->x + vd->place->width < vd->cfg->display.width) {
        rect = &rectv[rectc++];
        rect->x = vd->place->x + vd->place->width;
        rect->y = 0;
        rect->width = vd->cfg->display.width - rect->x;
        rect->height = vd->cfg->display.height;
    }
    if (vd->place->y > 0) {
        rect = &rectv[rectc++];
        rect->x = vd->place->x;
        rect->y = 0;
        rect->width = vd->place->width;
        rect->height = vd->place->y;
    }
    if (vd->place->y + vd->place->height < vd->cfg->display.height) {
        rect = &rectv[rectc++];
        rect->x = vd->place->x;
        rect->y = vd->place->y + vd->place->height;
        rect->width = vd->place->width;
        rect->height = vd->cfg->display.height - rect->y;
    }

    xcb_poly_fill_rectangle(conn, sys->window, sys->gc, rectc, rectv);

    /* Draw the picture */
    if (sys->attached)
        ck = xcb_shm_put_image_checked(conn, sys->window, sys->gc,
              /* real width */ pic->p->i_pitch / pic->p->i_pixel_pitch,
             /* real height */ pic->p->i_lines,
                       /* x */ sys->fmt.i_x_offset,
                       /* y */ sys->fmt.i_y_offset,
                   /* width */ sys->fmt.i_visible_width,
                  /* height */ sys->fmt.i_visible_height,
                               vd->place->x, vd->place->y, sys->depth,
                               XCB_IMAGE_FORMAT_Z_PIXMAP, 0,
                               segment, buf->offset);
    else {
        const size_t offset = sys->fmt.i_x_offset * pic->p->i_pixel_pitch
                            + sys->fmt.i_y_offset * pic->p->i_pitch;
        unsigned int lines = pic->p->i_lines - sys->fmt.i_y_offset;

        if (sys->fmt.i_x_offset > 0) {
            /*
             * Draw the last line separately as the scan line padding would
             * potentially reach beyond the end of the picture buffer.
             */
            lines--;
            xcb_put_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, sys->window,
                          sys->gc, pic->p->i_visible_pitch, 1,
                          vd->place->x, vd->place->y + vd->place->height - 1,
                          0, sys->depth, pic->p->i_visible_pitch,
                          pic->p->p_pixels + offset + lines * pic->p->i_pitch);
        }

        ck = xcb_put_image_checked(conn, XCB_IMAGE_FORMAT_Z_PIXMAP,
                                   sys->window, sys->gc,
                                   pic->p->i_pitch / pic->p->i_pixel_pitch,
                                   lines, vd->place->x, vd->place->y,
                                   0, sys->depth, pic->p->i_pitch * lines,
                                   pic->p->p_pixels + offset);

    }

    /* Wait for reply. This makes sure that the X server gets CPU time to
     * display the picture. xcb_flush() is *not* sufficient: especially
     * with shared memory the PUT requests are so short that many of them
     * can fit in X11 socket output buffer before the kernel preempts VLC.
     */
   xcb_generic_error_t *e = xcb_request_check(conn, ck);
   if (e != NULL) {
       msg_Err(vd, "%s: X11 error %d", "cannot put image", e->error_code);
       free(e);
   }

    /* FIXME might be WAY better to wait in some case (be carefull with
     * reset_pictures if done) + does not work with
     * vout_display wrapper. */

    if (sys->attached)
        xcb_shm_detach(conn, segment);
}

static int ResetPictures(vout_display_t *vd, video_format_t *fmt)
{
    vout_display_sys_t *sys = vd->sys;
    video_format_t src;

    video_format_ApplyRotation(&src, vd->source);
    sys->fmt.i_width  = src.i_width  * vd->place->width / src.i_visible_width;
    sys->fmt.i_height = src.i_height * vd->place->height / src.i_visible_height;

    sys->fmt.i_visible_width  = vd->place->width;
    sys->fmt.i_visible_height = vd->place->height;
    sys->fmt.i_x_offset = src.i_x_offset * vd->place->width / src.i_visible_width;
    sys->fmt.i_y_offset = src.i_y_offset * vd->place->height / src.i_visible_height;

    *fmt = sys->fmt;
    return VLC_SUCCESS;
}

static int Control(vout_display_t *vd, int query)
{
    vout_display_sys_t *sys = vd->sys;

    switch (query) {
    case VOUT_DISPLAY_CHANGE_SOURCE_ASPECT:
    case VOUT_DISPLAY_CHANGE_SOURCE_CROP:
    case VOUT_DISPLAY_CHANGE_SOURCE_PLACE:
    {
        if (vd->place->width  != sys->fmt.i_visible_width
         || vd->place->height != sys->fmt.i_visible_height)
            return VLC_EGENERIC;

        return VLC_SUCCESS;
    }

    default:
        msg_Err (vd, "Unknown request in XCB vout display");
        return VLC_EGENERIC;
    }
}

static int SetDisplaySize(vout_display_t *vd, unsigned width, unsigned height)
{
    vout_display_sys_t *sys = vd->sys;

    uint32_t mask = XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
    const uint32_t values[] = {
        width, height,
    };

    xcb_configure_window(sys->conn, sys->window, mask, values);

    if (vd->place->width  != sys->fmt.i_visible_width ||
        vd->place->height != sys->fmt.i_visible_height)
        return VLC_EGENERIC;

    return VLC_SUCCESS;
}

/**
 * Disconnect from the X server.
 */
static void Close(vout_display_t *vd)
{
    vout_display_sys_t *sys = vd->sys;

    /* colormap, window and context are garbage-collected by X */
    xcb_disconnect(sys->conn);
    free(sys);
}

static xcb_visualid_t DepthToFormat(const xcb_setup_t *setup,
                                    const xcb_depth_t *depth,
                                    video_format_t *restrict f)
{
    /* Check visual types for the selected depth */
    const xcb_visualtype_t *vt = xcb_depth_visuals(depth);

    for (int i = xcb_depth_visuals_length(depth); i > 0; i--, vt++)
        if (vlc_xcb_VisualToFormat(setup, depth->depth, vt, f))
            return vt->visual_id;

    return 0;
}

static xcb_visualid_t ScreenToFormat(const xcb_setup_t *setup,
                                     const xcb_screen_t *screen,
                                     uint8_t *restrict bits,
                                     const video_format_t *source,
                                     video_format_t *restrict fmtp)
{
    xcb_visualid_t visual = 0;

    *bits = 0;

    for (xcb_depth_iterator_t it = xcb_screen_allowed_depths_iterator(screen);
         it.rem > 0;
         xcb_depth_next(&it))
    {
        const xcb_depth_t *depth = it.data;
        video_format_t fmt;
        xcb_visualid_t vid;

        if (depth->depth <= *bits)
            continue; /* no better than earlier depth */

        video_format_ApplyRotation(&fmt, source);
        vid = DepthToFormat(setup, depth, &fmt);
        if (vid != 0)
        {
            *bits = depth->depth;
            *fmtp = fmt;
            visual = vid;
        }
    }
    return visual;
}

static const struct vlc_display_operations ops = {
    .close = Close,
    .prepare = Prepare,
    .display = Display,
    .set_display_size = SetDisplaySize,
    .control = Control,
    .reset_pictures = ResetPictures,
};

/**
 * Probe the X server.
 */
static int Open (vout_display_t *vd,
                 video_format_t *fmtp, vlc_video_context *context)
{
    vout_display_sys_t *sys = malloc (sizeof (*sys));
    if (unlikely(sys == NULL))
        return VLC_ENOMEM;

    vd->sys = sys;

    /* Get window, connect to X server */
    struct vlc_logger *log = vd->obj.logger;
    xcb_connection_t *conn;
    const xcb_screen_t *scr;
    int ret = vlc_xcb_parent_Create(log, vd->cfg->window, &conn, &scr);
    if (ret != VLC_SUCCESS)
    {
        free (sys);
        return ret;
    }
    sys->conn = conn;

    const xcb_setup_t *setup = xcb_get_setup (conn);

    /* Determine our pixel format */
    xcb_visualid_t vid = ScreenToFormat(setup, scr, &sys->depth, vd->source, fmtp);
    if (vid == 0) {
        msg_Err(vd, "no supported visual & pixel format");
        goto error;
    }

    msg_Dbg(vd, "using X11 visual ID 0x%"PRIx32, vid);
    msg_Dbg(vd, " %"PRIu8" bits depth", sys->depth);

    /* Create colormap (needed to select non-default visual) */
    xcb_colormap_t cmap;
    if (vid != scr->root_visual)
    {
        cmap = xcb_generate_id (conn);
        xcb_create_colormap (conn, XCB_COLORMAP_ALLOC_NONE,
                             cmap, scr->root, vid);
    }
    else
        cmap = scr->default_colormap;

    /* Create window */
    const uint32_t mask =
        XCB_CW_BACK_PIXEL |
        XCB_CW_BORDER_PIXEL |
        XCB_CW_BIT_GRAVITY |
        XCB_CW_EVENT_MASK |
        XCB_CW_COLORMAP;
    const uint32_t values[] = {
        /* XCB_CW_BACK_PIXEL */
        scr->black_pixel,
        /* XCB_CW_BORDER_PIXEL */
        scr->black_pixel,
        /* XCB_CW_BIT_GRAVITY */
        XCB_GRAVITY_NORTH_WEST,
        /* XCB_CW_EVENT_MASK */
        0,
        /* XCB_CW_COLORMAP */
        cmap,
    };

    sys->window = xcb_generate_id (conn);
    sys->gc = xcb_generate_id (conn);
    xcb_create_window(conn, sys->depth, sys->window,
                      vd->cfg->window->handle.xid,
                      0, 0, vd->cfg->display.width, vd->cfg->display.height, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, vid, mask, values);
    xcb_map_window(conn, sys->window);
    /* Create graphic context (I wonder why the heck do we need this) */
    xcb_create_gc(conn, sys->gc, sys->window, 0, NULL);

    msg_Dbg (vd, "using X11 window %08"PRIx32, sys->window);
    msg_Dbg (vd, "using X11 graphic context %08"PRIx32, sys->gc);

    if (XCB_shm_Check (VLC_OBJECT(vd), conn))
        sys->segment = xcb_generate_id(conn);
    else
        sys->segment = 0;

    sys->fmt = *fmtp;
    /* Setup vout_display_t once everything is fine */
    vd->ops = &ops;

    (void) context;
    return VLC_SUCCESS;

error:
    Close (vd);
    return VLC_EGENERIC;
}

/*
 * Module descriptor
 */
vlc_module_begin()
    set_shortname(N_("X11"))
    set_description(N_("X11 video output (XCB)"))
    set_subcategory(SUBCAT_VIDEO_VOUT)
    set_callback_display(Open, 100)
    add_shortcut("xcb-x11", "x11")
vlc_module_end()
