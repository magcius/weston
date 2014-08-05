/*
 * Copyright © 2008-2011 Kristian Høgsberg
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/uio.h>

#include "compositor.h"
#include "screenshooter-server-protocol.h"

#include "../wcap/wcap-decode.h"

struct screenshooter {
	struct weston_compositor *ec;
	struct wl_global *global;
	struct wl_client *client;
	struct weston_process process;
	struct wl_listener destroy_listener;
};

struct screenshooter_frame_listener {
	struct wl_listener listener;
	struct weston_buffer *buffer;
	weston_screenshooter_done_func_t done;
	void *data;
};

static void
copy_bgra_yflip(uint8_t *dst, uint8_t *src, int height, int stride)
{
	uint8_t *end;

	end = dst + height * stride;
	while (dst < end) {
		memcpy(dst, src, stride);
		dst += stride;
		src -= stride;
	}
}

static void
copy_bgra(uint8_t *dst, uint8_t *src, int height, int stride)
{
	/* TODO: optimize this out */
	memcpy(dst, src, height * stride);
}

static void
copy_row_swap_RB(void *vdst, void *vsrc, int bytes)
{
	uint32_t *dst = vdst;
	uint32_t *src = vsrc;
	uint32_t *end = dst + bytes / 4;

	while (dst < end) {
		uint32_t v = *src++;
		/*                    A R G B */
		uint32_t tmp = v & 0xff00ff00;
		tmp |= (v >> 16) & 0x000000ff;
		tmp |= (v << 16) & 0x00ff0000;
		*dst++ = tmp;
	}
}

static void
copy_rgba_yflip(uint8_t *dst, uint8_t *src, int height, int stride)
{
	uint8_t *end;

	end = dst + height * stride;
	while (dst < end) {
		copy_row_swap_RB(dst, src, stride);
		dst += stride;
		src -= stride;
	}
}

static void
copy_rgba(uint8_t *dst, uint8_t *src, int height, int stride)
{
	uint8_t *end;

	end = dst + height * stride;
	while (dst < end) {
		copy_row_swap_RB(dst, src, stride);
		dst += stride;
		src += stride;
	}
}

static void
screenshooter_frame_notify(struct wl_listener *listener, void *data)
{
	struct screenshooter_frame_listener *l =
		container_of(listener,
			     struct screenshooter_frame_listener, listener);
	struct weston_output *output = data;
	struct weston_compositor *compositor = output->compositor;
	int32_t stride;
	uint8_t *pixels, *d, *s;

	output->disable_planes--;
	wl_list_remove(&listener->link);
	stride = l->buffer->width * (PIXMAN_FORMAT_BPP(compositor->read_format) / 8);
	pixels = malloc(stride * l->buffer->height);

	if (pixels == NULL) {
		l->done(l->data, WESTON_SCREENSHOOTER_NO_MEMORY);
		free(l);
		return;
	}

	compositor->renderer->read_pixels(output,
			     compositor->read_format, pixels,
			     0, 0, output->current_mode->width,
			     output->current_mode->height);

	stride = wl_shm_buffer_get_stride(l->buffer->shm_buffer);

	d = wl_shm_buffer_get_data(l->buffer->shm_buffer);
	s = pixels + stride * (l->buffer->height - 1);

	wl_shm_buffer_begin_access(l->buffer->shm_buffer);

	switch (compositor->read_format) {
	case PIXMAN_a8r8g8b8:
	case PIXMAN_x8r8g8b8:
		if (compositor->capabilities & WESTON_CAP_CAPTURE_YFLIP)
			copy_bgra_yflip(d, s, output->current_mode->height, stride);
		else
			copy_bgra(d, pixels, output->current_mode->height, stride);
		break;
	case PIXMAN_x8b8g8r8:
	case PIXMAN_a8b8g8r8:
		if (compositor->capabilities & WESTON_CAP_CAPTURE_YFLIP)
			copy_rgba_yflip(d, s, output->current_mode->height, stride);
		else
			copy_rgba(d, pixels, output->current_mode->height, stride);
		break;
	default:
		break;
	}

	wl_shm_buffer_end_access(l->buffer->shm_buffer);

	l->done(l->data, WESTON_SCREENSHOOTER_SUCCESS);
	free(pixels);
	free(l);
}

WL_EXPORT int
weston_screenshooter_shoot(struct weston_output *output,
			   struct weston_buffer *buffer,
			   weston_screenshooter_done_func_t done, void *data)
{
	struct screenshooter_frame_listener *l;

	if (!wl_shm_buffer_get(buffer->resource)) {
		done(data, WESTON_SCREENSHOOTER_BAD_BUFFER);
		return -1;
	}

	buffer->shm_buffer = wl_shm_buffer_get(buffer->resource);
	buffer->width = wl_shm_buffer_get_width(buffer->shm_buffer);
	buffer->height = wl_shm_buffer_get_height(buffer->shm_buffer);

	if (buffer->width < output->current_mode->width ||
	    buffer->height < output->current_mode->height) {
		done(data, WESTON_SCREENSHOOTER_BAD_BUFFER);
		return -1;
	}

	l = malloc(sizeof *l);
	if (l == NULL) {
		done(data, WESTON_SCREENSHOOTER_NO_MEMORY);
		return -1;
	}

	l->buffer = buffer;
	l->done = done;
	l->data = data;
	l->listener.notify = screenshooter_frame_notify;
	wl_signal_add(&output->frame_signal, &l->listener);
	output->disable_planes++;
	weston_output_schedule_repaint(output);

	return 0;
}

static void
screenshooter_done(void *data, enum weston_screenshooter_outcome outcome)
{
	struct wl_resource *resource = data;

	switch (outcome) {
	case WESTON_SCREENSHOOTER_SUCCESS:
		screenshooter_send_done(resource);
		break;
	case WESTON_SCREENSHOOTER_NO_MEMORY:
		wl_resource_post_no_memory(resource);
		break;
	default:
		break;
	}
}

static void
screenshooter_shoot(struct wl_client *client,
		    struct wl_resource *resource,
		    struct wl_resource *output_resource,
		    struct wl_resource *buffer_resource)
{
	struct weston_output *output =
		wl_resource_get_user_data(output_resource);
	struct weston_buffer *buffer =
		weston_buffer_from_resource(buffer_resource);

	if (buffer == NULL) {
		wl_resource_post_no_memory(resource);
		return;
	}

	weston_screenshooter_shoot(output, buffer, screenshooter_done, resource);
}

struct screenshooter_interface screenshooter_implementation = {
	screenshooter_shoot
};

static void
bind_shooter(void *data, struct wl_resource *resource)
{
	struct screenshooter *shooter = data;

	if (wl_resource_get_client(resource) != shooter->client) {
		wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "screenshooter failed: permission denied");
		return;
	}

	wl_resource_set_implementation(resource, &screenshooter_implementation,
				       data, NULL);
}

static void
screenshooter_sigchld(struct weston_process *process, int status)
{
	struct screenshooter *shooter =
		container_of(process, struct screenshooter, process);

	shooter->client = NULL;
}

static void
screenshooter_binding(struct weston_seat *seat, uint32_t time, uint32_t key,
		      void *data)
{
	struct screenshooter *shooter = data;
	const char *screenshooter_exe = LIBEXECDIR "/weston-screenshooter";

	if (!shooter->client)
		shooter->client = weston_client_launch(shooter->ec,
					&shooter->process,
					screenshooter_exe, screenshooter_sigchld);
}

struct weston_recorder {
	struct weston_output *output;
	uint32_t *frame, *rect;
	uint32_t *tmpbuf;
	uint32_t total;
	int fd;
	struct wl_listener frame_listener;
	int count, destroying;
};

static uint32_t *
output_run(uint32_t *p, uint32_t delta, int run)
{
	int i;

	while (run > 0) {
		if (run <= 0xe0) {
			*p++ = delta | ((run - 1) << 24);
			break;
		}

		i = 24 - __builtin_clz(run);
		*p++ = delta | ((i + 0xe0) << 24);
		run -= 1 << (7 + i);
	}

	return p;
}

static uint32_t
component_delta(uint32_t next, uint32_t prev)
{
	unsigned char dr, dg, db;

	dr = (next >> 16) - (prev >> 16);
	dg = (next >>  8) - (prev >>  8);
	db = (next >>  0) - (prev >>  0);

	return (dr << 16) | (dg << 8) | (db << 0);
}

static void
weston_recorder_destroy(struct weston_recorder *recorder);

static void
weston_recorder_frame_notify(struct wl_listener *listener, void *data)
{
	struct weston_recorder *recorder =
		container_of(listener, struct weston_recorder, frame_listener);
	struct weston_output *output = data;
	struct weston_compositor *compositor = output->compositor;
	uint32_t msecs = output->frame_time;
	pixman_box32_t *r;
	pixman_region32_t damage, transformed_damage;
	int i, j, k, n, width, height, run, stride;
	uint32_t delta, prev, *d, *s, *p, next;
	struct {
		uint32_t msecs;
		uint32_t nrects;
	} header;
	struct iovec v[2];
	int do_yflip;
	int y_orig;
	uint32_t *outbuf;

	do_yflip = !!(compositor->capabilities & WESTON_CAP_CAPTURE_YFLIP);
	if (do_yflip)
		outbuf = recorder->rect;
	else
		outbuf = recorder->tmpbuf;

	pixman_region32_init(&damage);
	pixman_region32_init(&transformed_damage);
	pixman_region32_intersect(&damage, &output->region,
				  &output->previous_damage);
	pixman_region32_translate(&damage, -output->x, -output->y);
	weston_transformed_region(output->width, output->height,
				 output->transform, output->current_scale,
				 &damage, &transformed_damage);
	pixman_region32_fini(&damage);

	r = pixman_region32_rectangles(&transformed_damage, &n);
	if (n == 0) {
		pixman_region32_fini(&transformed_damage);
		return;
	}

	header.msecs = msecs;
	header.nrects = n;
	v[0].iov_base = &header;
	v[0].iov_len = sizeof header;
	v[1].iov_base = r;
	v[1].iov_len = n * sizeof *r;
	recorder->total += writev(recorder->fd, v, 2);
	stride = output->current_mode->width;

	for (i = 0; i < n; i++) {
		width = r[i].x2 - r[i].x1;
		height = r[i].y2 - r[i].y1;

		if (do_yflip)
			y_orig = output->current_mode->height - r[i].y2;
		else
			y_orig = r[i].y1;

		compositor->renderer->read_pixels(output,
				compositor->read_format, recorder->rect,
				r[i].x1, y_orig, width, height);

		s = recorder->rect;
		p = outbuf;
		run = prev = 0; /* quiet gcc */
		for (j = 0; j < height; j++) {
			if (do_yflip)
				y_orig = r[i].y2 - j - 1;
			else
				y_orig = r[i].y1 + j;
			d = recorder->frame + stride * y_orig + r[i].x1;

			for (k = 0; k < width; k++) {
				next = *s++;
				delta = component_delta(next, *d);
				*d++ = next;
				if (run == 0 || delta == prev) {
					run++;
				} else {
					p = output_run(p, prev, run);
					run = 1;
				}
				prev = delta;
			}
		}

		p = output_run(p, prev, run);

		recorder->total += write(recorder->fd,
					 outbuf, (p - outbuf) * 4);

#if 0
		fprintf(stderr,
			"%dx%d at %d,%d rle from %d to %d bytes (%f) total %dM\n",
			width, height, r[i].x1, r[i].y1,
			width * height * 4, (int) (p - outbuf) * 4,
			(float) (p - outbuf) / (width * height),
			recorder->total / 1024 / 1024);
#endif
	}

	pixman_region32_fini(&transformed_damage);
	recorder->count++;

	if (recorder->destroying)
		weston_recorder_destroy(recorder);
}

static void
weston_recorder_free(struct weston_recorder *recorder)
{
	if (recorder == NULL)
		return;
	free(recorder->rect);
	free(recorder->tmpbuf);
	free(recorder->frame);
	free(recorder);
}

static void
weston_recorder_create(struct weston_output *output, const char *filename)
{
	struct weston_compositor *compositor = output->compositor;
	struct weston_recorder *recorder;
	int stride, size;
	struct { uint32_t magic, format, width, height; } header;
	int do_yflip;

	do_yflip = !!(compositor->capabilities & WESTON_CAP_CAPTURE_YFLIP);

	recorder = malloc(sizeof *recorder);
	if (recorder == NULL) {
		weston_log("%s: out of memory\n", __func__);
		return;
	}

	stride = output->current_mode->width;
	size = stride * 4 * output->current_mode->height;
	recorder->frame = zalloc(size);
	recorder->rect = malloc(size);
	recorder->total = 0;
	recorder->count = 0;
	recorder->destroying = 0;
	recorder->output = output;

	if ((recorder->frame == NULL) || (recorder->rect == NULL)) {
		weston_log("%s: out of memory\n", __func__);
		weston_recorder_free(recorder);
		return;
	}

	if (do_yflip)
		recorder->tmpbuf = NULL;
	else
		recorder->tmpbuf = malloc(size);

	header.magic = WCAP_HEADER_MAGIC;

	switch (compositor->read_format) {
	case PIXMAN_x8r8g8b8:
	case PIXMAN_a8r8g8b8:
		header.format = WCAP_FORMAT_XRGB8888;
		break;
	case PIXMAN_a8b8g8r8:
		header.format = WCAP_FORMAT_XBGR8888;
		break;
	default:
		weston_log("unknown recorder format\n");
		weston_recorder_free(recorder);
		return;
	}

	recorder->fd = open(filename,
			    O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

	if (recorder->fd < 0) {
		weston_log("problem opening output file %s: %m\n", filename);
		weston_recorder_free(recorder);
		return;
	}

	header.width = output->current_mode->width;
	header.height = output->current_mode->height;
	recorder->total += write(recorder->fd, &header, sizeof header);

	recorder->frame_listener.notify = weston_recorder_frame_notify;
	wl_signal_add(&output->frame_signal, &recorder->frame_listener);
	output->disable_planes++;
	weston_output_damage(output);
}

static void
weston_recorder_destroy(struct weston_recorder *recorder)
{
	wl_list_remove(&recorder->frame_listener.link);
	close(recorder->fd);
	recorder->output->disable_planes--;
	weston_recorder_free(recorder);
}

static void
recorder_binding(struct weston_seat *seat, uint32_t time, uint32_t key, void *data)
{
	struct weston_seat *ws = (struct weston_seat *) seat;
	struct weston_compositor *ec = ws->compositor;
	struct weston_output *output;
	struct wl_listener *listener = NULL;
	struct weston_recorder *recorder;
	static const char filename[] = "capture.wcap";

	wl_list_for_each(output, &seat->compositor->output_list, link) {
		listener = wl_signal_get(&output->frame_signal,
					 weston_recorder_frame_notify);
		if (listener)
			break;
	}

	if (listener) {
		recorder = container_of(listener, struct weston_recorder,
					frame_listener);

		weston_log(
			"stopping recorder, total file size %dM, %d frames\n",
			recorder->total / (1024 * 1024), recorder->count);

		recorder->destroying = 1;
		weston_output_schedule_repaint(recorder->output);
	} else {
		if (seat->keyboard && seat->keyboard->focus &&
		    seat->keyboard->focus->output)
			output = seat->keyboard->focus->output;
		else
			output = container_of(ec->output_list.next,
					      struct weston_output, link);

		weston_log("starting recorder for output %s, file %s\n",
			   output->name, filename);
		weston_recorder_create(output, filename);
	}
}

static void
screenshooter_destroy(struct wl_listener *listener, void *data)
{
	struct screenshooter *shooter =
		container_of(listener, struct screenshooter, destroy_listener);

	wl_global_destroy(shooter->global);
	free(shooter);
}

WL_EXPORT void
screenshooter_create(struct weston_compositor *ec)
{
	struct screenshooter *shooter;

	shooter = malloc(sizeof *shooter);
	if (shooter == NULL)
		return;

	shooter->ec = ec;
	shooter->client = NULL;

	shooter->global = wl_global_create_auto(ec->wl_display,
						&screenshooter_interface, 1,
						shooter, bind_shooter);
	weston_compositor_add_key_binding(ec, KEY_S, MODIFIER_SUPER,
					  screenshooter_binding, shooter);
	weston_compositor_add_key_binding(ec, KEY_R, MODIFIER_SUPER,
					  recorder_binding, shooter);

	shooter->destroy_listener.notify = screenshooter_destroy;
	wl_signal_add(&ec->destroy_signal, &shooter->destroy_listener);
}
