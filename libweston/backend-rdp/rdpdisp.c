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
	const struct weston_head *whl = *(const struct weston_head **)p1;
	const struct weston_head *whr = *(const struct weston_head **)p2;
	const struct rdp_head *l = to_rdp_head((void *)whl);
	const struct rdp_head *r = to_rdp_head((void *)whr);

	return l->monitorMode.monitorDef.x > r->monitorMode.monitorDef.x;
}

static int
compare_monitors_y(const void *p1, const void *p2)
{
	const struct weston_head *whl = *(const struct weston_head **)p1;
	const struct weston_head *whr = *(const struct weston_head **)p2;
	const struct rdp_head *l = to_rdp_head((void *)whl);
	const struct rdp_head *r = to_rdp_head((void *)whr);

	return l->monitorMode.monitorDef.y > r->monitorMode.monitorDef.y;
}

float
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

static struct rdp_head *
get_first_head(struct weston_compositor *ec)
{
	struct weston_head *head;

	wl_list_for_each(head, &ec->head_list, compositor_link)
		return to_rdp_head(head);

	return NULL;
}

static bool
match_primary(struct rdp_backend *rdp, rdpMonitor *a, rdpMonitor *b)
{
	if (a->is_primary && b->is_primary)
		return true;

	return false;
}

static bool
match_dimensions(struct rdp_backend *rdp, rdpMonitor *a, rdpMonitor *b)
{
	int scale_a = disp_get_output_scale_from_monitor(rdp, a);
	int scale_b = disp_get_output_scale_from_monitor(rdp, b);

	if (a->width != b->width ||
	    a->height != b->height ||
	    scale_a != scale_b)
		return false;

	return true;
}

static bool
match_position(struct rdp_backend *rdp, rdpMonitor *a, rdpMonitor *b)
{
	if (a->x != b->x ||
	    a->y != b->y)
		return false;

	return true;
}

static bool
match_any(struct rdp_backend *rdp, rdpMonitor *a, rdpMonitor *b)
{
	return true;
}

static void
update_head(struct rdp_backend *rdp, struct rdp_head *head, struct rdp_monitor_mode *mm)
{
	struct weston_mode mode = {};
	int scale;
	bool changed = false;

	head->matched = true;
	scale = disp_get_output_scale_from_monitor(rdp, &mm->monitorDef);

	if (!match_position(rdp, &head->monitorMode.monitorDef, &mm->monitorDef))
		changed = true;

	if (!match_dimensions(rdp, &head->monitorMode.monitorDef, &mm->monitorDef)) {
		mode.flags = WL_OUTPUT_MODE_PREFERRED;
		mode.width = mm->monitorDef.width;
		mode.height = mm->monitorDef.height;
		mode.refresh = rdp->rdp_monitor_refresh_rate;
		weston_output_mode_set_native(head->base.output,
					      &mode, scale);
		changed = true;
	}

	if (changed) {
		weston_head_set_device_changed(&head->base);
	}
	head->monitorMode = *mm;
	/* update monitor region in client */
	pixman_region32_clear(&head->regionClient);
	pixman_region32_init_rect(&head->regionClient,
				  mm->monitorDef.x,
				  mm->monitorDef.y,
				  mm->monitorDef.width,
				  mm->monitorDef.height);
}

static void
match_heads(struct rdp_backend *rdp, struct rdp_monitor_mode *mm, uint32_t count,
	    int *done,
	    bool (*cmp)(struct rdp_backend *rdp, rdpMonitor *a, rdpMonitor *b))
{
	struct weston_head *iter;
	struct rdp_head *current;
	uint32_t i;

	wl_list_for_each(iter, &rdp->compositor->head_list, compositor_link) {
		current = to_rdp_head(iter);
		if (current->matched)
			continue;

		for (i = 0; i < count; i++) {
			if (*done & (1 << i))
				continue;

			if (cmp(rdp, &current->monitorMode.monitorDef, &mm[i].monitorDef)) {
				*done |= 1 << i;
				update_head(rdp, current, &mm[i]);
				break;
			}
		}
	}
}

static void
disp_start_monitor_layout_change(freerdp_peer *client, struct rdp_monitor_mode *monitorMode, UINT32 monitorCount)
{
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	rdpSettings *settings = client->context->settings;
	struct rdp_backend *b = peerCtx->rdpBackend;
	struct rdp_head *current;
	struct weston_head *iter, *tmp;
	pixman_region32_t desktop;
	int done = 0;

	assert_compositor_thread(b);

	pixman_region32_init(&desktop);

	/* Prune heads that were never enabled, and flag heads as unmatched  */
	wl_list_for_each_safe(iter, tmp, &b->compositor->head_list, compositor_link) {
		current = to_rdp_head(iter);
		if (!iter->output) {
			rdp_head_destroy(b->compositor, to_rdp_head(iter));
			continue;
		}
		current->matched = false;
	}

	/* We want the primary head to remain primary - it
	 * should always be rdp-0.
	  */
	match_heads(b, monitorMode, monitorCount, &done, match_primary);

	/* Match first head with the same dimensions */
	match_heads(b, monitorMode, monitorCount, &done, match_dimensions);

	/* Match head with the same position */
	match_heads(b, monitorMode, monitorCount, &done, match_position);

	/* Pick any available head */
	match_heads(b, monitorMode, monitorCount, &done, match_any);

	/* Destroy any heads we won't be using */
	wl_list_for_each_safe(iter, tmp, &b->compositor->head_list, compositor_link) {
		current = to_rdp_head(iter);
		if (!current->matched)
			rdp_head_destroy(b->compositor, to_rdp_head(iter));
	}


	for (uint32_t i = 0; i < monitorCount; i++) {
		/* accumulate monitor layout */
		if (monitorMode[i].monitorDef.is_primary) {
			/* it looks settings's desktopWidth/Height only represents primary */
			settings->DesktopWidth = monitorMode[i].monitorDef.width;
			settings->DesktopHeight = monitorMode[i].monitorDef.height;
		}
		pixman_region32_union_rect(&desktop, &desktop,
					   monitorMode[i].monitorDef.x,
					   monitorMode[i].monitorDef.y,
					   monitorMode[i].monitorDef.width,
					   monitorMode[i].monitorDef.height);

		/* Create new heads for any without matches */
		if (!(done & (1 << i)))
			rdp_head_create(b->compositor, monitorMode[i].monitorDef.is_primary, &monitorMode[i]);
	}
	peerCtx->desktop_left = desktop.extents.x1;
	peerCtx->desktop_top = desktop.extents.y1;
	peerCtx->desktop_width = desktop.extents.x2 - desktop.extents.x1;
	peerCtx->desktop_height = desktop.extents.y2 - desktop.extents.y1;
	pixman_region32_fini(&desktop);
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
		float client_scale = disp_get_client_scale_from_monitor(b, &monitorMode[i].monitorDef);

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
			i, monitorMode[i].scale, client_scale);
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
sort_head_list(struct weston_compositor *ec, int (*compar)(const void *, const void *))
{
	int count = wl_list_length(&ec->head_list);
	struct weston_head *head_array[count];
	struct weston_head *iter, *tmp;
	int i = 0;

	wl_list_for_each_safe(iter, tmp, &ec->head_list, compositor_link) {
		head_array[i++] = iter;
		wl_list_remove(&iter->compositor_link);
	}

	qsort(head_array, count, sizeof(struct weston_head *), compar);

	wl_list_init(&ec->head_list);
	for (i = 0; i < count; i++)
		wl_list_insert(ec->head_list.prev, &head_array[i]->compositor_link);
}

static void
disp_monitor_validate_and_compute_layout(struct weston_compositor *ec)
{
	struct rdp_backend *b = to_rdp_backend(ec);
	struct weston_head *iter;
	bool isConnected_H = false;
	bool isConnected_V = false;
	bool isScalingUsed = false;
	bool isScalingSupported = true;
	int upperLeftX = 0;
	int upperLeftY = 0;
	int i;
	int count = wl_list_length(&ec->head_list);
	pixman_rectangle32_t rectWeston[count];

	wl_list_for_each(iter, &ec->head_list, compositor_link) {
		struct rdp_head *head = to_rdp_head(iter);
		float client_scale = disp_get_client_scale_from_monitor(b, &head->monitorMode.monitorDef);

		/* check if any monitor has scaling enabled */
		if (client_scale != 1.0f)
			isScalingUsed = true;

		/* find upper-left corner of combined monitors in client space */
		if (upperLeftX > head->monitorMode.monitorDef.x)
			upperLeftX = head->monitorMode.monitorDef.x;
		if (upperLeftY > head->monitorMode.monitorDef.y)
			upperLeftY = head->monitorMode.monitorDef.y;
	}
	assert(upperLeftX <= 0);
	assert(upperLeftY <= 0);
	weston_log("Client desktop upper left coordinate (%d,%d)\n", upperLeftX, upperLeftY);

	count = wl_list_length(&ec->head_list);

	if (count > 1) {
		struct rdp_head *head, *last;
		int32_t offsetFromOriginClient;

		/* first, sort monitors horizontally */
		sort_head_list(ec, compare_monitors_x);
		head = get_first_head(ec);
		last = head;
		assert(upperLeftX == head->monitorMode.monitorDef.x);

		/* check if monitors are horizontally connected each other */
		offsetFromOriginClient = head->monitorMode.monitorDef.x + head->monitorMode.monitorDef.width;
		i = 0;
		wl_list_for_each(iter, &ec->head_list, compositor_link) {
			struct rdp_head *cur = to_rdp_head(iter);

			i++;
			if (i == 1)
				continue;

			if (offsetFromOriginClient != cur->monitorMode.monitorDef.x) {
				weston_log("\tRDP client reported monitors not horizontally connected each other at %d (x check)\n", i);
				break;
			}
			offsetFromOriginClient += cur->monitorMode.monitorDef.width;

			if (!is_line_intersected(last->monitorMode.monitorDef.y,
						 last->monitorMode.monitorDef.y + last->monitorMode.monitorDef.height,
						 cur->monitorMode.monitorDef.y,
						 cur->monitorMode.monitorDef.y + cur->monitorMode.monitorDef.height)) {
				weston_log("\tRDP client reported monitors not horizontally connected each other at %d (y check)\n\n", i);
				break;
			}
			last = cur;
		}
		if (i == count) {
			weston_log("\tAll monitors are horizontally placed\n");
			isConnected_H = true;
		} else {
			struct rdp_head *head, *last;
			/* next, trying sort monitors vertically */
			sort_head_list(ec, compare_monitors_y);
			head = get_first_head(ec);
			last = head;
			assert(upperLeftY == head->monitorMode.monitorDef.y);

			/* make sure monitors are horizontally connected each other */
			offsetFromOriginClient = head->monitorMode.monitorDef.y + head->monitorMode.monitorDef.height;
			i = 0;
			wl_list_for_each(iter, &ec->head_list, compositor_link) {
				struct rdp_head *cur = to_rdp_head(iter);

				i++;
				if (i == 1)
					continue;

				if (offsetFromOriginClient != cur->monitorMode.monitorDef.y) {
					weston_log("\tRDP client reported monitors not vertically connected each other at %d (y check)\n", i);
					break;
				}
				offsetFromOriginClient += cur->monitorMode.monitorDef.height;

				if (!is_line_intersected(last->monitorMode.monitorDef.x,
							 last->monitorMode.monitorDef.x + last->monitorMode.monitorDef.width,
							 cur->monitorMode.monitorDef.x,
							 cur->monitorMode.monitorDef.x + cur->monitorMode.monitorDef.width)) {
					weston_log("\tRDP client reported monitors not horizontally connected each other at %d (x check)\n\n", i);
					break;
				}
				last = cur;
			}

			if (i == count) {
				weston_log("\tAll monitors are vertically placed\n");
				isConnected_V = true;
			}
		}
	} else {
		isConnected_H = true;
	}

	if (isScalingUsed && (!isConnected_H && !isConnected_V)) {
		/* scaling can't be supported in complex monitor placement */
		weston_log("\nWARNING\nWARNING\nWARNING: Scaling is used, but can't be supported in complex monitor placement\nWARNING\nWARNING\n");
		isScalingSupported = false;
	}

	if (isScalingUsed && isScalingSupported) {
		uint32_t offsetFromOriginWeston = 0;
		i = 0;

		wl_list_for_each(iter, &ec->head_list, compositor_link) {
			struct rdp_head *head = to_rdp_head(iter);

			rectWeston[i].width = head->monitorMode.monitorDef.width / head->monitorMode.scale;
			rectWeston[i].height = head->monitorMode.monitorDef.height / head->monitorMode.scale;
			if (isConnected_H) {
				assert(isConnected_V == false);
				rectWeston[i].x = offsetFromOriginWeston;
				rectWeston[i].y = abs((upperLeftY - head->monitorMode.monitorDef.y) / head->monitorMode.scale);
				offsetFromOriginWeston += rectWeston[i].width;
			} else {
				assert(isConnected_V == true);
				rectWeston[i].x = abs((upperLeftX - head->monitorMode.monitorDef.x) / head->monitorMode.scale);
				rectWeston[i].y = offsetFromOriginWeston;
				offsetFromOriginWeston += rectWeston[i].height;
			}
			assert(rectWeston[i].x >= 0);
			assert(rectWeston[i].y >= 0);
			i++;
		}
	} else {
		i = 0;

		/* no scaling is used or monitor placement is too complex to scale in weston space, fallback to 1.0f */
		wl_list_for_each(iter, &ec->head_list, compositor_link) {
			struct rdp_head *head = to_rdp_head(iter);

			rectWeston[i].width = head->monitorMode.monitorDef.width;
			rectWeston[i].height = head->monitorMode.monitorDef.height;
			rectWeston[i].x = head->monitorMode.monitorDef.x + abs(upperLeftX);
			rectWeston[i].y = head->monitorMode.monitorDef.y + abs(upperLeftY);
			head->monitorMode.monitorDef.attributes.desktopScaleFactor = 0.0;
			assert(rectWeston[i].x >= 0);
			assert(rectWeston[i].y >= 0);
			head->monitorMode.scale = 1;
			i++;
		}
	}

	weston_log("%s:---OUTPUT---\n", __func__);
	i = 0;
	wl_list_for_each(iter, &ec->head_list, compositor_link) {
		struct rdp_head *head = to_rdp_head(iter);
		struct rdp_backend *b = to_rdp_backend(ec);
		float client_scale = disp_get_client_scale_from_monitor(b, &head->monitorMode.monitorDef);

		weston_log("	rdpMonitor[%d]: x:%d, y:%d, width:%d, height:%d, is_primary:%d\n",
			i, head->monitorMode.monitorDef.x, head->monitorMode.monitorDef.y,
			   head->monitorMode.monitorDef.width, head->monitorMode.monitorDef.height,
			   head->monitorMode.monitorDef.is_primary);
		weston_log("	rdpMonitor[%d]: weston x:%d, y:%d, width:%d, height:%d\n",
			i, rectWeston[i].x, rectWeston[i].y,
			   rectWeston[i].width, rectWeston[i].height);
		weston_log("	rdpMonitor[%d]: physicalWidth:%d, physicalHeight:%d, orientation:%d\n",
			i, head->monitorMode.monitorDef.attributes.physicalWidth,
			   head->monitorMode.monitorDef.attributes.physicalHeight,
			   head->monitorMode.monitorDef.attributes.orientation);
		weston_log("	rdpMonitor[%d]: desktopScaleFactor:%d, deviceScaleFactor:%d\n",
			i, head->monitorMode.monitorDef.attributes.desktopScaleFactor,
			   head->monitorMode.monitorDef.attributes.deviceScaleFactor);
		weston_log("	rdpMonitor[%d]: scale:%d, clientScale:%3.2f\n",
			i, head->monitorMode.scale, client_scale);
		i++;
	}

	i = 0;
	wl_list_for_each(iter, &ec->head_list, compositor_link) {
		struct rdp_head *current = to_rdp_head(iter);
		struct weston_output *output = iter->output;

		if (output) {
			/* ask weston to adjust size */
			struct weston_mode new_mode = {};
			struct rdp_backend *b = to_rdp_backend(ec);
			float client_scale = disp_get_client_scale_from_monitor(b, &current->monitorMode.monitorDef);

			new_mode.width = current->monitorMode.monitorDef.width;
			new_mode.height = current->monitorMode.monitorDef.height;
			weston_log("Head mode change:%s NEW width:%d, height:%d, scale:%d, clientScale:%f\n",
				  output->name, current->monitorMode.monitorDef.width,
				  current->monitorMode.monitorDef.height,
				  current->monitorMode.scale,
				  client_scale);
			if (output->scale != current->monitorMode.scale) {
				weston_output_disable(output);
				output->scale = 0; /* reset scale first, otherwise assert */
				weston_output_set_scale(output, current->monitorMode.scale);
				weston_output_enable(output);
			}
			weston_output_mode_set_native(iter->output, &new_mode, current->monitorMode.scale);
			weston_head_set_physical_size(iter,
						      current->monitorMode.monitorDef.attributes.physicalWidth,
						      current->monitorMode.monitorDef.attributes.physicalHeight);
			/* Notify clients for updated resolution/scale. */
			weston_output_set_transform(output, WL_OUTPUT_TRANSFORM_NORMAL);
			/* output size must match with monitor's rect in weston space */
			assert(output->width == (int32_t)rectWeston[i].width);
			assert(output->height == (int32_t)rectWeston[i].height);
		} else {
			/* if head doesn't have output yet, mode is set at rdp_output_set_size */
			weston_log("output doesn't exist for head %s\n", iter->name);
		}
		i++;
	}

	/* move output to final location */
	i = 0;
	wl_list_for_each(iter, &ec->head_list, compositor_link) {
		struct rdp_head *current = to_rdp_head(iter);

		if (current->base.output) {
			weston_log("move head/output %s (%d,%d) -> (%d,%d)\n",
				current->base.name,
				current->base.output->x,
				current->base.output->y,
				rectWeston[i].x,
				rectWeston[i].y);
			/* Notify clients for updated output position. */
			weston_output_move(current->base.output,
				rectWeston[i].x,
				rectWeston[i].y);
		} else {
			/* newly created head doesn't have output yet */
			/* position will be set at rdp_output_enable */
		}
		i++;
	}

	/* make sure head list is not empty */
	assert(!wl_list_empty(&ec->head_list));

	BOOL is_primary_found = FALSE;
	i = 0;
	wl_list_for_each(iter, &ec->head_list, compositor_link) {
		struct rdp_head *current = to_rdp_head(iter);

		if (current->monitorMode.monitorDef.is_primary) {
			weston_log("client origin (0,0) is (%d,%d) in Weston space\n",
				rectWeston[i].x,
				rectWeston[i].y);
			/* primary must be at (0,0) in client space */
			assert(current->monitorMode.monitorDef.x == 0);
			assert(current->monitorMode.monitorDef.y == 0);
			/* there must be only one primary */
			assert(is_primary_found == FALSE);
			is_primary_found = TRUE;
		}
		i++;
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
	}

	if (!disp_monitor_sanity_check_layout(peerCtx, monitorMode, monitor_count)) {
		success = true;
		goto exit;
	}

	disp_start_monitor_layout_change(client, monitorMode, monitor_count);

	disp_monitor_validate_and_compute_layout(b->compositor);

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
			float client_scale = disp_get_client_scale_from_monitor(b, &head->monitorMode.monitorDef);
			float scale = 1.0f / client_scale;

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
		float scale = disp_get_client_scale_from_monitor(b, &head->monitorMode.monitorDef);

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
