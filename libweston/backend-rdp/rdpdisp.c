/*
 * Copyright © 2020 Microsoft
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>

#include <stdio.h>
#include <wchar.h>
#include <strings.h>

#include "rdp.h"

#include "shared/xalloc.h"

static BOOL
is_line_intersected(int l1, int l2, int r1, int r2)
{
	int l = l1 > r1 ? l1 : r1;
	int r = l2 < r2 ? l2 : r2;
	return (l < r);
}

static int
compare_monitors_x(const void *p1, const void *p2)
{
	const struct rdp_monitor_mode *l = p1;
	const struct rdp_monitor_mode *r = p2;
	return l->monitorDef.x > r->monitorDef.x;
}

static int
compare_monitors_y(const void *p1, const void *p2)
{
	const struct rdp_monitor_mode *l = p1;
	const struct rdp_monitor_mode *r = p2;
	return l->monitorDef.y > r->monitorDef.y;
}

static float
disp_get_client_scale_from_monitor(struct rdp_backend *b, const rdpMonitor *config)
{
	if (config->attributes.desktopScaleFactor == 0.0)
		return 1.0f;

	if (b->enable_hi_dpi_support) {
		if (b->debug_desktop_scaling_factor)
			return (float)b->debug_desktop_scaling_factor / 100.f;
		else if (b->enable_fractional_hi_dpi_support)
			return (float)config->attributes.desktopScaleFactor / 100.0f;
		else if (b->enable_fractional_hi_dpi_roundup)
			return (float)(int)((config->attributes.desktopScaleFactor + 50) / 100);
		else
			return (float)(int)(config->attributes.desktopScaleFactor / 100);
	} else {
		return 1.0f;
	}
}

static int
disp_get_output_scale_from_monitor(struct rdp_backend *b, rdpMonitor *config)
{
	return (int) disp_get_client_scale_from_monitor(b, config);
}

static void
disp_start_monitor_layout_change(freerdp_peer *client, struct rdp_monitor_mode *monitorMode, UINT32 monitorCount, int *doneIndex)
{
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	pixman_region32_t desktop;

	assert_compositor_thread(b);

	pixman_region32_init(&desktop);
	/* move all heads to pending list */
	wl_list_init(&b->head_pending_list);
	wl_list_insert_list(&b->head_pending_list, &b->compositor->head_list);
	wl_list_init(&b->compositor->head_list);

	for (UINT32 i = 0; i < monitorCount; i++, monitorMode++) {
		struct weston_head *iter, *tmp;
		/* accumulate monitor layout */
		pixman_region32_union_rect(&desktop, &desktop,
					   monitorMode->monitorDef.x,
					   monitorMode->monitorDef.y,
					   monitorMode->monitorDef.width,
					   monitorMode->monitorDef.height);

		wl_list_for_each_safe(iter, tmp, &b->head_pending_list, compositor_link) {
			struct rdp_head *current = to_rdp_head(iter);

			if (memcmp(&current->monitorMode, monitorMode, sizeof(*monitorMode)) == 0) {
				rdp_debug_verbose(b, "Head mode exact match:%s, x:%d, y:%d, width:%d, height:%d, is_primary: %d\n",
					current->base.name,
					current->monitorMode.monitorDef.x, current->monitorMode.monitorDef.y,
					current->monitorMode.monitorDef.width, current->monitorMode.monitorDef.height,
					current->monitorMode.monitorDef.is_primary);
				wl_list_remove(&iter->compositor_link);
				wl_list_insert(&b->compositor->head_list, &iter->compositor_link);
				*doneIndex |= (1 << i);
				break;
			}
		}
	}
	peerCtx->desktop_left = desktop.extents.x1;
	peerCtx->desktop_top = desktop.extents.y1;
	peerCtx->desktop_width = desktop.extents.x2 - desktop.extents.x1;
	peerCtx->desktop_height = desktop.extents.y2 - desktop.extents.y1;
	pixman_region32_fini(&desktop);
}

static void
disp_end_monitor_layout_change(freerdp_peer *client)
{
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	struct weston_head *iter, *next;

	assert_compositor_thread(b);

	/* move output to final location */
	wl_list_for_each_safe(iter, next, &b->compositor->head_list, compositor_link) {
		struct rdp_head *current = to_rdp_head(iter);

		if (current->base.output) {
			rdp_debug(b, "move head/output %s (%d,%d) -> (%d,%d)\n",
				current->base.name,
				current->base.output->x,
				current->base.output->y,
				current->monitorMode.rectWeston.x,
				current->monitorMode.rectWeston.y);
			/* Notify clients for updated output position. */
			weston_output_move(current->base.output,
				current->monitorMode.rectWeston.x,
				current->monitorMode.rectWeston.y);
		} else {
			/* newly created head doesn't have output yet */
			/* position will be set at rdp_output_enable */
		}
	}
	/* remove all unsed head from pending list */
	if (!wl_list_empty(&b->head_pending_list)) {
		wl_list_for_each_safe(iter, next, &b->head_pending_list, compositor_link)
			rdp_head_destroy(b->compositor, to_rdp_head(iter));
		/* make sure nothing left in pending list */
		assert(wl_list_empty(&b->head_pending_list));
		wl_list_init(&b->head_pending_list);
	}
	/* make sure head list is not empty */
	assert(!wl_list_empty(&b->compositor->head_list));

	BOOL is_primary_found = FALSE;
	wl_list_for_each(iter, &b->compositor->head_list, compositor_link) {
		struct rdp_head *current = to_rdp_head(iter);

		if (current->monitorMode.monitorDef.is_primary) {
			rdp_debug(b, "client origin (0,0) is (%d,%d) in Weston space\n", 
				current->monitorMode.rectWeston.x,
				current->monitorMode.rectWeston.y);
			/* primary must be at (0,0) in client space */
			assert(current->monitorMode.monitorDef.x == 0);
			assert(current->monitorMode.monitorDef.y == 0);
			/* there must be only one primary */
			assert(is_primary_found == FALSE);
			is_primary_found = TRUE;
		}
	}
}

static UINT
disp_set_monitor_layout_change(freerdp_peer *client, struct rdp_monitor_mode *monitorMode)
{
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	rdpSettings *settings = client->context->settings;
	struct weston_output *output = NULL;
	struct weston_head *head = NULL;
	struct weston_head *iter;
	BOOL updateMode = FALSE;

	assert_compositor_thread(b);

	/* search head match configuration from pending list */
	wl_list_for_each(iter, &b->head_pending_list, compositor_link) {
		struct rdp_head *current = to_rdp_head(iter);

		if (current->monitorMode.monitorDef.is_primary != monitorMode->monitorDef.is_primary)
			continue;

		if (current->monitorMode.monitorDef.width == monitorMode->monitorDef.width &&
			current->monitorMode.monitorDef.height == monitorMode->monitorDef.height &&
			current->monitorMode.scale == monitorMode->scale) {
			/* size mode (width/height/scale) */
			head = &current->base;
			output = head->output;
			break;
		} else if (current->monitorMode.monitorDef.x == monitorMode->monitorDef.x &&
				current->monitorMode.monitorDef.y == monitorMode->monitorDef.y) {
			/* position match in client space */
			head = &current->base;
			output = head->output;
			updateMode = TRUE;
			break;
		}
	}
	if (!head) {
		/* just pick first one to change mode */
		wl_list_for_each(iter, &b->head_pending_list, compositor_link) {
			struct rdp_head *current = to_rdp_head(iter);

			/* primary is only re-used for primary */
			if (!current->monitorMode.monitorDef.is_primary) {
				head = &current->base;
				output = head->output;
				updateMode = TRUE;
				break;
			}
		}
	}

	if (head) {
		struct rdp_head *current = to_rdp_head(head);

		assert(output);
		rdp_debug(b, "Head mode change:%s OLD width:%d, height:%d, scale:%d, clientScale:%f\n",
			output->name, current->monitorMode.monitorDef.width,
				      current->monitorMode.monitorDef.height,
				      current->monitorMode.scale,
				      current->monitorMode.clientScale);
		/* reusing exising head */
		current->monitorMode = *monitorMode;
		/* update monitor region in client */
		pixman_region32_clear(&current->regionClient);
		pixman_region32_init_rect(&current->regionClient,
			monitorMode->monitorDef.x, monitorMode->monitorDef.y,
			monitorMode->monitorDef.width, monitorMode->monitorDef.height);
		wl_list_remove(&head->compositor_link);
		wl_list_insert(&b->compositor->head_list, &head->compositor_link);
	} else {
		/* no head found, create one */
		if (rdp_head_create(b->compositor, monitorMode->monitorDef.is_primary, monitorMode) == NULL)
			return ERROR_INTERNAL_ERROR;  
	}

	if (updateMode) {
		if (output) {
			assert(head);
			/* ask weston to adjust size */
			struct weston_mode new_mode = {};
			new_mode.width = monitorMode->monitorDef.width;
			new_mode.height = monitorMode->monitorDef.height;
			if (monitorMode->monitorDef.is_primary) {
				/* it looks settings's desktopWidth/Height only represents primary */
				settings->DesktopWidth = new_mode.width;
				settings->DesktopHeight = new_mode.height;
			}
			rdp_debug(b, "Head mode change:%s NEW width:%d, height:%d, scale:%d, clientScale:%f\n",
				output->name, monitorMode->monitorDef.width,
					      monitorMode->monitorDef.height,
					      monitorMode->scale,
					      monitorMode->clientScale);
			if (output->scale != monitorMode->scale) {
				weston_output_disable(output);
				output->scale = 0; /* reset scale first, otherwise assert */
				weston_output_set_scale(output, monitorMode->scale);
				weston_output_enable(output);
			}
			weston_output_mode_set_native(output, &new_mode, monitorMode->scale);
			weston_head_set_physical_size(head,
				monitorMode->monitorDef.attributes.physicalWidth,
				monitorMode->monitorDef.attributes.physicalHeight);
			/* Notify clients for updated resolution/scale. */
			weston_output_set_transform(output, WL_OUTPUT_TRANSFORM_NORMAL);
			/* output size must match with monitor's rect in weston space */
			assert(output->width == (int32_t) monitorMode->rectWeston.width);
			assert(output->height == (int32_t) monitorMode->rectWeston.height);
		} else {
			/* if head doesn't have output yet, mode is set at rdp_output_set_size */
			rdp_debug(b, "output doesn't exist for head %s\n", head->name);
		}
	}

	return 0;
}

static bool
disp_monitor_sanity_check_layout(RdpPeerContext *peerCtx, struct rdp_monitor_mode *monitorMode, uint32_t count)
{
	struct rdp_backend *b = peerCtx->rdpBackend;
	uint32_t primaryCount = 0;
	uint32_t i;

	/* dump client monitor topology */
	rdp_debug(b, "%s:---INPUT---\n", __func__);
	for (i = 0; i < count; i++) {
		rdp_debug(b, "	rdpMonitor[%d]: x:%d, y:%d, width:%d, height:%d, is_primary:%d\n",
			i, monitorMode[i].monitorDef.x, monitorMode[i].monitorDef.y,
			   monitorMode[i].monitorDef.width, monitorMode[i].monitorDef.height,
			   monitorMode[i].monitorDef.is_primary);
		rdp_debug(b, "	rdpMonitor[%d]: physicalWidth:%d, physicalHeight:%d, orientation:%d\n",
			i, monitorMode[i].monitorDef.attributes.physicalWidth,
			   monitorMode[i].monitorDef.attributes.physicalHeight,
			   monitorMode[i].monitorDef.attributes.orientation);
		rdp_debug(b, "	rdpMonitor[%d]: desktopScaleFactor:%d, deviceScaleFactor:%d\n",
			i, monitorMode[i].monitorDef.attributes.desktopScaleFactor,
			   monitorMode[i].monitorDef.attributes.deviceScaleFactor);
		rdp_debug(b, "	rdpMonitor[%d]: scale:%d, client scale :%3.2f\n",
			i, monitorMode[i].scale, monitorMode[i].clientScale);
	}

	for (i = 0; i < count; i++) {
		/* make sure there is only one primary and its position at client */
		if (monitorMode[i].monitorDef.is_primary) {
			/* count number of primary */
			if (++primaryCount > 1) {
				rdp_debug_error(b, "%s: RDP client reported unexpected primary count (%d)\n",__func__, primaryCount);
				return false;
			}
			/* primary must be at (0,0) in client space */
			if (monitorMode[i].monitorDef.x != 0 || monitorMode[i].monitorDef.y != 0) {
				rdp_debug_error(b, "%s: RDP client reported primary is not at (0,0) but (%d,%d).\n",
					__func__, monitorMode[i].monitorDef.x, monitorMode[i].monitorDef.y);
				return false;
			}
		}
	}
	return true;
}

static void
disp_monitor_validate_and_compute_layout(RdpPeerContext *peerCtx, struct rdp_monitor_mode *monitorMode, uint32_t monitorCount)
{
	struct rdp_backend *b = peerCtx->rdpBackend;
	bool isConnected_H = false;
	bool isConnected_V = false;
	bool isScalingUsed = false;
	bool isScalingSupported = true;
	int upperLeftX = 0;
	int upperLeftY = 0;
	uint32_t i;

	for (i = 0; i < monitorCount; i++) {
		/* check if any monitor has scaling enabled */
		if (monitorMode[i].clientScale != 1.0f)
			isScalingUsed = true;

		/* find upper-left corner of combined monitors in client space */
		if (upperLeftX > monitorMode[i].monitorDef.x)
			upperLeftX = monitorMode[i].monitorDef.x;
		if (upperLeftY > monitorMode[i].monitorDef.y)
			upperLeftY = monitorMode[i].monitorDef.y;
	}
	assert(upperLeftX <= 0);
	assert(upperLeftY <= 0);
	rdp_debug(b, "Client desktop upper left coordinate (%d,%d)\n", upperLeftX, upperLeftY);

	if (monitorCount > 1) {
		int32_t offsetFromOriginClient;

		/* first, sort monitors horizontally */
		qsort(monitorMode, monitorCount, sizeof(*monitorMode), compare_monitors_x);
		assert(upperLeftX == monitorMode[0].monitorDef.x);

		/* check if monitors are horizontally connected each other */
		offsetFromOriginClient = monitorMode[0].monitorDef.x + monitorMode[0].monitorDef.width;
		for (i = 1; i < monitorCount; i++) {
			if (offsetFromOriginClient != monitorMode[i].monitorDef.x) {
				rdp_debug(b, "\tRDP client reported monitors not horizontally connected each other at %d (x check)\n", i);
				break;
			}
			offsetFromOriginClient += monitorMode[i].monitorDef.width;

			if (!is_line_intersected(monitorMode[i-1].monitorDef.y,
						 monitorMode[i-1].monitorDef.y + monitorMode[i-1].monitorDef.height,
						 monitorMode[i].monitorDef.y,
						 monitorMode[i].monitorDef.y + monitorMode[i].monitorDef.height)) {
				rdp_debug(b, "\tRDP client reported monitors not horizontally connected each other at %d (y check)\n\n", i);
				break;
			}
		}
		if (i == monitorCount) {
			rdp_debug(b, "\tAll monitors are horizontally placed\n");
			isConnected_H = true;
		} else {
			/* next, trying sort monitors vertically */
			qsort(monitorMode, monitorCount, sizeof(*monitorMode), compare_monitors_y);
			assert(upperLeftY == monitorMode[0].monitorDef.y);

			/* make sure monitors are horizontally connected each other */
			offsetFromOriginClient = monitorMode[0].monitorDef.y + monitorMode[0].monitorDef.height;
			for (i = 1; i < monitorCount; i++) {
				if (offsetFromOriginClient != monitorMode[i].monitorDef.y) {
					rdp_debug(b, "\tRDP client reported monitors not vertically connected each other at %d (y check)\n", i);
					break;
				}
				offsetFromOriginClient += monitorMode[i].monitorDef.height;

				if (!is_line_intersected(monitorMode[i-1].monitorDef.x,
							 monitorMode[i-1].monitorDef.x + monitorMode[i-1].monitorDef.width,
							 monitorMode[i].monitorDef.x,
							 monitorMode[i].monitorDef.x + monitorMode[i].monitorDef.width)) {
					rdp_debug(b, "\tRDP client reported monitors not horizontally connected each other at %d (x check)\n\n", i);
					break;
				}
			}

			if (i == monitorCount) {
				rdp_debug(b, "\tAll monitors are vertically placed\n");
				isConnected_V = true;
			}
		}
	} else {
		isConnected_H = true;
	}

	if (isScalingUsed && (!isConnected_H && !isConnected_V)) {
		/* scaling can't be supported in complex monitor placement */
		rdp_debug_error(b, "\nWARNING\nWARNING\nWARNING: Scaling is used, but can't be supported in complex monitor placement\nWARNING\nWARNING\n");
		isScalingSupported = false;
	}

	if (isScalingUsed && isScalingSupported) {
		uint32_t offsetFromOriginWeston = 0;
		for (i = 0; i < monitorCount; i++) {
			monitorMode[i].rectWeston.width = monitorMode[i].monitorDef.width / monitorMode[i].scale;
			monitorMode[i].rectWeston.height = monitorMode[i].monitorDef.height / monitorMode[i].scale;
			if (isConnected_H) {
				assert(isConnected_V == false);
				monitorMode[i].rectWeston.x = offsetFromOriginWeston;
				monitorMode[i].rectWeston.y = abs((upperLeftY - monitorMode[i].monitorDef.y) / monitorMode[i].scale);
				offsetFromOriginWeston += monitorMode[i].rectWeston.width;
			} else {
				assert(isConnected_V == true);
				monitorMode[i].rectWeston.x = abs((upperLeftX - monitorMode[i].monitorDef.x) / monitorMode[i].scale);
				monitorMode[i].rectWeston.y = offsetFromOriginWeston;
				offsetFromOriginWeston += monitorMode[i].rectWeston.height;
			}
			assert(monitorMode[i].rectWeston.x >= 0);
			assert(monitorMode[i].rectWeston.y >= 0);
		}
	} else {
		/* no scaling is used or monitor placement is too complex to scale in weston space, fallback to 1.0f */
		for (i = 0; i < monitorCount; i++) {
			monitorMode[i].rectWeston.width = monitorMode[i].monitorDef.width;
			monitorMode[i].rectWeston.height = monitorMode[i].monitorDef.height;
			monitorMode[i].rectWeston.x = monitorMode[i].monitorDef.x + abs(upperLeftX);
			monitorMode[i].rectWeston.y = monitorMode[i].monitorDef.y + abs(upperLeftY);
			assert(monitorMode[i].rectWeston.x >= 0);
			assert(monitorMode[i].rectWeston.y >= 0);
			monitorMode[i].scale = 1;
			monitorMode[i].clientScale = 1.0f;
		}
	}

	rdp_debug(b, "%s:---OUTPUT---\n", __func__);
	for (UINT32 i = 0; i < monitorCount; i++) {
		rdp_debug(b, "	rdpMonitor[%d]: x:%d, y:%d, width:%d, height:%d, is_primary:%d\n",
			i, monitorMode[i].monitorDef.x, monitorMode[i].monitorDef.y,
			   monitorMode[i].monitorDef.width, monitorMode[i].monitorDef.height,
			   monitorMode[i].monitorDef.is_primary);
		rdp_debug(b, "	rdpMonitor[%d]: weston x:%d, y:%d, width:%d, height:%d\n",
			i, monitorMode[i].rectWeston.x, monitorMode[i].rectWeston.y,
			   monitorMode[i].rectWeston.width, monitorMode[i].rectWeston.height);
		rdp_debug(b, "	rdpMonitor[%d]: physicalWidth:%d, physicalHeight:%d, orientation:%d\n",
			i, monitorMode[i].monitorDef.attributes.physicalWidth,
			   monitorMode[i].monitorDef.attributes.physicalHeight,
			   monitorMode[i].monitorDef.attributes.orientation);
		rdp_debug(b, "	rdpMonitor[%d]: desktopScaleFactor:%d, deviceScaleFactor:%d\n",
			i, monitorMode[i].monitorDef.attributes.desktopScaleFactor,
			   monitorMode[i].monitorDef.attributes.deviceScaleFactor);
		rdp_debug(b, "	rdpMonitor[%d]: scale:%d, clientScale:%3.2f\n",
			i, monitorMode[i].scale, monitorMode[i].clientScale);
	}
}

bool
handle_adjust_monitor_layout(freerdp_peer *client, int monitor_count, rdpMonitor *monitors)
{
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	bool success = true;
	struct rdp_monitor_mode *monitorMode = NULL;
	int i;

	monitorMode = xmalloc(sizeof(struct rdp_monitor_mode) * monitor_count);

	for (i = 0; i < monitor_count; i++) {
		monitorMode[i].monitorDef = monitors[i];
		monitorMode[i].monitorDef.orig_screen = 0;
		monitorMode[i].scale = disp_get_output_scale_from_monitor(b, &monitorMode[i].monitorDef);
		monitorMode[i].clientScale = disp_get_client_scale_from_monitor(b, &monitorMode[i].monitorDef);
	}

	if (!disp_monitor_sanity_check_layout(peerCtx, monitorMode, monitor_count)) {
		success = true;
		goto exit;
	}

	disp_monitor_validate_and_compute_layout(peerCtx, monitorMode, monitor_count);

	int doneIndex = 0;
	disp_start_monitor_layout_change(client, monitorMode, monitor_count, &doneIndex);
	for (int i = 0; i < monitor_count; i++) {
		if ((doneIndex & (1 << i)) == 0)
			if (disp_set_monitor_layout_change(client, &monitorMode[i]) != 0) {
				success = true;
				goto exit;
			}
	}
	disp_end_monitor_layout_change(client);

exit:
	free(monitorMode);

	return success;
}

static inline void
to_weston_scale_only(RdpPeerContext *peer, struct weston_output *output, float scale, int *x, int *y)
{
	//rdp_matrix_transform_scale(&output->inverse_matrix, x, y);
	/* TODO: built-in to matrix */
	*x = (float)(*x) * scale;
	*y = (float)(*y) * scale;
}

/* Input x/y in client space, output x/y in weston space */
struct weston_output *
to_weston_coordinate(RdpPeerContext *peerContext, int32_t *x, int32_t *y, uint32_t *width, uint32_t *height)
{
	struct rdp_backend *b = peerContext->rdpBackend;
	int sx = *x, sy = *y;
	struct weston_head *head_iter;

	/* First, find which monitor contains this x/y. */
	wl_list_for_each(head_iter, &b->compositor->head_list, compositor_link) {
		struct rdp_head *head = to_rdp_head(head_iter);

		if (pixman_region32_contains_point(&head->regionClient, sx, sy, NULL)) {
			struct weston_output *output = head->base.output;
			float scale = 1.0f / head->monitorMode.clientScale;

			/* translate x/y to offset from this output on client space. */
			sx -= head->monitorMode.monitorDef.x;
			sy -= head->monitorMode.monitorDef.y;
			/* scale x/y to client output space. */
			to_weston_scale_only(peerContext, output, scale, &sx, &sy);
			if (width && height)
				to_weston_scale_only(peerContext, output, scale, width, height);
			/* translate x/y to offset from this output on weston space. */
			sx += output->x;
			sy += output->y;
			rdp_debug_verbose(b, "%s: (x:%d, y:%d) -> (sx:%d, sy:%d) at head:%s\n",
					  __func__, *x, *y, sx, sy, head->base.name);
			*x = sx;
			*y = sy;
			return output; // must be only 1 head per output.
		}
	}
	/* x/y is outside of any monitors. */
	return NULL;
}

static inline void
to_client_scale_only(RdpPeerContext *peer, struct weston_output *output, float scale, int *x, int *y)
{
	//rdp_matrix_transform_scale(&output->matrix, x, y);
	/* TODO: built-in to matrix */
	*x = (float)(*x) * scale;
	*y = (float)(*y) * scale;
}

/* Input x/y in weston space, output x/y in client space */
void
to_client_coordinate(RdpPeerContext *peerContext, struct weston_output *output, int32_t *x, int32_t *y, uint32_t *width, uint32_t *height)
{
	struct rdp_backend *b = peerContext->rdpBackend;
	int sx = *x, sy = *y;
	struct weston_head *head_iter;

	/* Pick first head from output. */
	wl_list_for_each(head_iter, &output->head_list, output_link) {
		struct rdp_head *head = to_rdp_head(head_iter);
		float scale = head->monitorMode.clientScale;

		/* translate x/y to offset from this output on weston space. */
		sx -= output->x;
		sy -= output->y;
		/* scale x/y to client output space. */
		to_client_scale_only(peerContext, output, scale, &sx, &sy);
		if (width && height)
			to_client_scale_only(peerContext, output, scale, width, height);
		/* translate x/y to offset from this output on client space. */
		sx += head->monitorMode.monitorDef.x;
		sy += head->monitorMode.monitorDef.y;
		rdp_debug_verbose(b, "%s: (x:%d, y:%d) -> (sx:%d, sy:%d) at head:%s\n",
				  __func__, *x, *y, sx, sy, head_iter->name);
		*x = sx;
		*y = sy;
		return; // must be only 1 head per output.
	}
}
