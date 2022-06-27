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
disp_get_client_scale_from_monitor(RdpPeerContext *peerCtx, struct rdp_monitor_mode *monitorMode)
{
	struct rdp_backend *b = peerCtx->rdpBackend;
	if (b->enable_hi_dpi_support) {
		if (b->debug_desktop_scaling_factor)
			return (float)b->debug_desktop_scaling_factor / 100.f;
		else if (b->enable_fractional_hi_dpi_support)
			return (float)monitorMode->monitorDef.attributes.desktopScaleFactor / 100.0f;
		else if (b->enable_fractional_hi_dpi_roundup)
			return (float)(int)((monitorMode->monitorDef.attributes.desktopScaleFactor + 50) / 100);
		else
			return (float)(int)(monitorMode->monitorDef.attributes.desktopScaleFactor / 100);
	} else {
		return 1.0f;
	}
}

static int
disp_get_output_scale_from_monitor(RdpPeerContext *peerCtx, struct rdp_monitor_mode *monitorMode)
{
	return (int) disp_get_client_scale_from_monitor(peerCtx, monitorMode);
}

static void
disp_start_monitor_layout_change(freerdp_peer *client, struct rdp_monitor_mode *monitorMode, UINT32 monitorCount, int *doneIndex)
{
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;

	assert_compositor_thread(b);

	pixman_region32_clear(&peerCtx->regionClientHeads);
	pixman_region32_clear(&peerCtx->regionWestonHeads);
	/* move all heads to pending list */
	b->head_pending_list = b->head_list;
	b->head_pending_list.next->prev = &b->head_pending_list; 
	b->head_pending_list.prev->next = &b->head_pending_list;
	/* init move pending list */
	wl_list_init(&b->head_move_pending_list);
	/* clear head list */
	wl_list_init(&b->head_list);
	for (UINT32 i = 0; i < monitorCount; i++, monitorMode++) {
		struct rdp_head *current;
		wl_list_for_each(current, &b->head_pending_list, link) {
			if (memcmp(&current->monitorMode, monitorMode, sizeof(*monitorMode)) == 0) {
				rdp_debug_verbose(b, "Head mode exact match:%s, x:%d, y:%d, width:%d, height:%d, is_primary: %d\n",
					current->base.name,
					current->monitorMode.monitorDef.x, current->monitorMode.monitorDef.y,
					current->monitorMode.monitorDef.width, current->monitorMode.monitorDef.height,
					current->monitorMode.monitorDef.is_primary);
				/* move from pending list to move pending list */
				wl_list_remove(&current->link);
				wl_list_insert(&b->head_move_pending_list, &current->link);
				/* accumulate monitor layout */
				pixman_region32_union_rect(&peerCtx->regionClientHeads, &peerCtx->regionClientHeads,
					current->monitorMode.monitorDef.x, current->monitorMode.monitorDef.y,
					current->monitorMode.monitorDef.width, current->monitorMode.monitorDef.height);
				pixman_region32_union_rect(&peerCtx->regionWestonHeads, &peerCtx->regionWestonHeads,
					current->monitorMode.rectWeston.x, current->monitorMode.rectWeston.y,
					current->monitorMode.rectWeston.width, current->monitorMode.rectWeston.height);
				*doneIndex |= (1 << i);
				break;
			}
		}
	}
}

static void
disp_end_monitor_layout_change(freerdp_peer *client)
{
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	struct rdp_head *current, *next;

	assert_compositor_thread(b);

	/* move output to final location */
	wl_list_for_each_safe(current, next, &b->head_move_pending_list, link) {
		/* move from move pending list to current list */
		wl_list_remove(&current->link);
		wl_list_insert(&b->head_list, &current->link);
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
	assert(wl_list_empty(&b->head_move_pending_list));
	wl_list_init(&b->head_move_pending_list);
	/* remove all unsed head from pending list */
	if (!wl_list_empty(&b->head_pending_list)) {
		wl_list_for_each_safe(current, next, &b->head_pending_list, link)
			rdp_head_destroy(b->compositor, current);
		/* make sure nothing left in pending list */
		assert(wl_list_empty(&b->head_pending_list));
		wl_list_init(&b->head_pending_list);
	}
	/* make sure head list is not empty */
	assert(!wl_list_empty(&b->head_list));

	BOOL is_primary_found = FALSE;
	wl_list_for_each(current, &b->head_list, link) {
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
	rdp_debug(b, "client virtual desktop is (%d,%d) - (%d,%d)\n", 
		peerCtx->regionClientHeads.extents.x1, peerCtx->regionClientHeads.extents.y1,
		peerCtx->regionClientHeads.extents.x2, peerCtx->regionClientHeads.extents.y2);
	rdp_debug(b, "weston virtual desktop is (%d,%d) - (%d,%d)\n", 
		peerCtx->regionWestonHeads.extents.x1, peerCtx->regionWestonHeads.extents.y1,
		peerCtx->regionWestonHeads.extents.x2, peerCtx->regionWestonHeads.extents.y2);
}

static UINT
disp_set_monitor_layout_change(freerdp_peer *client, struct rdp_monitor_mode *monitorMode)
{
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	rdpSettings *settings = client->context->settings;
	struct weston_output *output = NULL;
	struct weston_head *head = NULL;
	struct rdp_head *current;
	BOOL updateMode = FALSE;

	assert_compositor_thread(b);

	if (monitorMode->monitorDef.is_primary) {
		assert(b->head_default);
		assert(b->output_default);

		/* use default output and head for primary */
		output = &b->output_default->base;
		head = &b->head_default->base;
		current = to_rdp_head(head);

		if (current->monitorMode.monitorDef.width != monitorMode->monitorDef.width ||
			current->monitorMode.monitorDef.height != monitorMode->monitorDef.height ||
			current->monitorMode.scale != monitorMode->scale)
			updateMode = TRUE;
	} else {
		/* search head match configuration from pending list */
		wl_list_for_each(current, &b->head_pending_list, link) {
			if (current->monitorMode.monitorDef.is_primary) {
				/* primary is only re-used for primary */
			} else if (current->monitorMode.monitorDef.width == monitorMode->monitorDef.width &&
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
			wl_list_for_each(current, &b->head_pending_list, link) {
				/* primary is only re-used for primary */
				if (!current->monitorMode.monitorDef.is_primary) {
					head = &current->base;
					output = head->output;
					updateMode = TRUE;
					break;
				}
			}
		}
	}

	if (head) {
		assert(output);
		assert(to_rdp_head(head) == current);
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
		pixman_region32_clear(&current->regionWeston);
		pixman_region32_init_rect(&current->regionWeston,
			monitorMode->rectWeston.x, monitorMode->rectWeston.y,
			monitorMode->rectWeston.width, monitorMode->rectWeston.height);
		/* move from pending list to move pending list */
		wl_list_remove(&current->link);
		wl_list_insert(&b->head_move_pending_list, &to_rdp_head(head)->link);
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

	/* accumulate monitor layout */
	pixman_region32_union_rect(&peerCtx->regionClientHeads, &peerCtx->regionClientHeads,
		monitorMode->monitorDef.x, monitorMode->monitorDef.y,
		monitorMode->monitorDef.width, monitorMode->monitorDef.height);
	pixman_region32_union_rect(&peerCtx->regionWestonHeads, &peerCtx->regionWestonHeads,
		monitorMode->rectWeston.x, monitorMode->rectWeston.y,
		monitorMode->rectWeston.width, monitorMode->rectWeston.height);

	return 0;
}

static void
disp_force_recreate_iter(void *element, void *data)
{
	struct weston_surface *surface = (struct weston_surface *)element;
	struct weston_surface_rail_state *rail_state = (struct weston_surface_rail_state *)surface->backend_state;
	rail_state->forceRecreateSurface = TRUE;
	rail_state->forceUpdateWindowState = TRUE;
}

static BOOL
disp_monitor_validate_and_compute_layout(RdpPeerContext *peerCtx, struct rdp_monitor_mode *monitorMode, UINT32 monitorCount)
{
	struct rdp_backend *b = peerCtx->rdpBackend;
	bool isConnected_H = false;
	bool isConnected_V = false;
	bool isScalingUsed = false;
	bool isScalingSupported = true;
	uint32_t primaryCount = 0;
	int upperLeftX = 0;
	int upperLeftY = 0;
	uint32_t i;

	/* dump client monitor topology */
	rdp_debug(b, "%s:---INPUT---\n", __func__);
	for (i = 0; i < monitorCount; i++) {
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

	for (i = 0; i < monitorCount; i++) {
		/* make sure there is only one primary and its position at client */
		if (monitorMode[i].monitorDef.is_primary) {
			/* count number of primary */
			if (++primaryCount > 1) {
				rdp_debug_error(b, "%s: RDP client reported unexpected primary count (%d)\n",__func__, primaryCount);
				return FALSE;
			}
			/* primary must be at (0,0) in client space */
			if (monitorMode[i].monitorDef.x != 0 || monitorMode[i].monitorDef.y != 0) {
				rdp_debug_error(b, "%s: RDP client reported primary is not at (0,0) but (%d,%d).\n",
					__func__, monitorMode[i].monitorDef.x, monitorMode[i].monitorDef.y);
				return FALSE;
			}
		}

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

	return TRUE;
}

void
disp_monitor_layout_change(DispServerContext* context, int monitor_count, rdpMonitor *monitors)
{
	freerdp_peer *client = (freerdp_peer*)context->custom;
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	rdpSettings *settings = client->context->settings;
	struct rdp_backend *b = peerCtx->rdpBackend;
	struct rdp_monitor_mode *monitorMode;
	MONITOR_DEF *resetMonitorDef;

	assert_compositor_thread(b);

	rdp_debug(b, "Client: DisplayControl: monitor count:0x%x\n", monitor_count);

	assert(settings->HiDefRemoteApp);

	if (monitor_count > RDP_MAX_MONITOR) {
		rdp_debug_error(b, "\nWARNING\nWARNING\nWARNING: client reports more monitors then expected:(%d)\nWARNING\nWARNING\n",
				monitor_count);
		return;
	}

	monitorMode = xmalloc(sizeof(struct rdp_monitor_mode) * monitor_count);
	resetMonitorDef = malloc(sizeof(MONITOR_DEF) * monitor_count);
	if (!resetMonitorDef) {
		free(monitorMode);
		return;
	}

	for (int i = 0; i < monitor_count; i++) {
		monitorMode[i].monitorDef = monitors[i];
		resetMonitorDef[i].left = monitors[i].x;
		resetMonitorDef[i].top = monitors[i].y;
		resetMonitorDef[i].right = monitors[i].width;
		resetMonitorDef[i].bottom = monitors[i].height;
		resetMonitorDef[i].flags = monitors[i].is_primary;
		monitorMode[i].monitorDef.orig_screen = 0;
		monitorMode[i].scale = disp_get_output_scale_from_monitor(peerCtx, &monitorMode[i]);
		monitorMode[i].clientScale = disp_get_client_scale_from_monitor(peerCtx, &monitorMode[i]);
	}

	if (!disp_monitor_validate_and_compute_layout(peerCtx, monitorMode, monitor_count))
		goto Exit;

	int doneIndex = 0;
	disp_start_monitor_layout_change(client, monitorMode, monitor_count, &doneIndex);
	for (int i = 0; i < monitor_count; i++) {
		if ((doneIndex & (1 << i)) == 0) {
			if (disp_set_monitor_layout_change(client, &monitorMode[i]) != 0)
				goto Exit;
		}
	}
	disp_end_monitor_layout_change(client);

	/* tell client the server updated the monitor layout */
	RDPGFX_RESET_GRAPHICS_PDU resetGraphics = {};
	resetGraphics.width = peerCtx->regionClientHeads.extents.x2 - peerCtx->regionClientHeads.extents.x1;
	resetGraphics.height = peerCtx->regionClientHeads.extents.y2 - peerCtx->regionClientHeads.extents.x1;
	resetGraphics.monitorCount = monitor_count;
	resetGraphics.monitorDefArray = resetMonitorDef;
	peerCtx->rail_grfx_server_context->ResetGraphics(peerCtx->rail_grfx_server_context, &resetGraphics);

	/* force recreate all surface and redraw. */
	rdp_id_manager_for_each(&peerCtx->windowId, disp_force_recreate_iter, NULL);
	weston_compositor_damage_all(b->compositor);

Exit:
	free(monitorMode);
	free(resetMonitorDef);
	return;
}

bool
handle_adjust_monitor_layout(freerdp_peer *client, int monitor_count, rdpMonitor *monitors)
{
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	bool success = true;
	struct rdp_monitor_mode *monitorMode = NULL;
	int i;

	monitorMode = malloc(sizeof(struct rdp_monitor_mode) * monitor_count);
	if (!monitorMode)
		return true;

	for (i = 0; i < monitor_count; i++) {
		monitorMode[i].monitorDef = monitors[i];
		monitorMode[i].scale = disp_get_output_scale_from_monitor(peerCtx, &monitorMode[i]);
		monitorMode[i].clientScale = disp_get_client_scale_from_monitor(peerCtx, &monitorMode[i]);
	}

	if (!disp_monitor_validate_and_compute_layout(peerCtx, monitorMode, monitor_count)) {
		success = true;
		goto exit;
	}

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
