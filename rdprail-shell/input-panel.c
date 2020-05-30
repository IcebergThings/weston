/*
 * Copyright © 2010-2012 Intel Corporation
 * Copyright © 2011-2012 Collabora, Ltd.
 * Copyright © 2013 Raspberry Pi Foundation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "config.h"

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "shell.h"
#include "input-method-unstable-v1-server-protocol.h"
#include "shared/helpers.h"

struct input_panel_surface {
	struct wl_resource *resource;
	struct wl_signal destroy_signal;

	struct desktop_shell *shell;

	struct weston_surface *surface;
	struct wl_listener surface_destroy_listener;
};

static void
show_input_panels(struct wl_listener *listener, void *data)
{
	struct weston_surface *surface = (struct weston_surface*)data;

	/* Output who requested to show input panels */
	if (surface && surface->resource) {
		pid_t pid;
		uid_t uid;
		gid_t gid;
		struct wl_client *client = wl_resource_get_client(surface->resource);
		wl_client_get_credentials(client, &pid, &uid, &gid);
		weston_log("%s pid:%d, uid:%d, gid:%d is requesting to show input panel\n",
			    __func__, pid, uid, gid);
		if (pid > 0 && !is_system_distro()) {
			char path[32] = {};
			char image_name[256] = {};
			sprintf(path, "/proc/%d/exe", pid);
			if (readlink(path, image_name, sizeof image_name) > 0)
				weston_log("%s pid:%d, image_name:%s\n",__func__, pid, image_name);
		}
	}
}

static int
input_panel_get_label(struct weston_surface *surface, char *buf, size_t len)
{
	return snprintf(buf, len, "rdprail-shell input panel");
}

static void
input_panel_committed(struct weston_surface *surface, int32_t sx, int32_t sy)
{
	weston_log("%s is not expected to be called\n", __func__);
}

static void
destroy_input_panel_surface(struct input_panel_surface *input_panel_surface)
{
	wl_signal_emit(&input_panel_surface->destroy_signal, input_panel_surface);

	wl_list_remove(&input_panel_surface->surface_destroy_listener.link);

	input_panel_surface->surface->committed = NULL;
	input_panel_surface->surface->committed_private = NULL;
	weston_surface_set_label_func(input_panel_surface->surface, NULL);

	free(input_panel_surface);
}

static void
input_panel_handle_surface_destroy(struct wl_listener *listener, void *data)
{
	struct input_panel_surface *ipsurface = container_of(listener,
							     struct input_panel_surface,
							     surface_destroy_listener);

	if (ipsurface->resource) {
		wl_resource_destroy(ipsurface->resource);
	} else {
		destroy_input_panel_surface(ipsurface);
	}
}

static struct input_panel_surface *
create_input_panel_surface(struct desktop_shell *shell,
			   struct weston_surface *surface)
{
	struct input_panel_surface *input_panel_surface;

	input_panel_surface = calloc(1, sizeof *input_panel_surface);
	if (!input_panel_surface)
		return NULL;

	surface->committed = input_panel_committed;
	surface->committed_private = input_panel_surface;
	weston_surface_set_label_func(surface, input_panel_get_label);

	input_panel_surface->shell = shell;
	input_panel_surface->surface = surface;

	wl_signal_init(&input_panel_surface->destroy_signal);
	input_panel_surface->surface_destroy_listener.notify = input_panel_handle_surface_destroy;
	wl_signal_add(&surface->destroy_signal,
		      &input_panel_surface->surface_destroy_listener);

	return input_panel_surface;
}

static void
input_panel_surface_set_toplevel(struct wl_client *client,
				 struct wl_resource *resource,
				 struct wl_resource *output_resource,
				 uint32_t position)
{
}

static void
input_panel_surface_set_overlay_panel(struct wl_client *client,
				      struct wl_resource *resource)
{
}

static const struct zwp_input_panel_surface_v1_interface input_panel_surface_implementation = {
	input_panel_surface_set_toplevel,
	input_panel_surface_set_overlay_panel
};

static struct input_panel_surface *
get_input_panel_surface(struct weston_surface *surface)
{
	if (surface->committed == input_panel_committed) {
		return surface->committed_private;
	} else {
		return NULL;
	}
}

static void
destroy_input_panel_surface_resource(struct wl_resource *resource)
{
	struct input_panel_surface *ipsurf =
		wl_resource_get_user_data(resource);

	destroy_input_panel_surface(ipsurf);
}

static void
input_panel_get_input_panel_surface(struct wl_client *client,
				    struct wl_resource *resource,
				    uint32_t id,
				    struct wl_resource *surface_resource)
{
	struct weston_surface *surface =
		wl_resource_get_user_data(surface_resource);
	struct desktop_shell *shell = wl_resource_get_user_data(resource);
	struct input_panel_surface *ipsurf;

	if (get_input_panel_surface(surface)) {
		wl_resource_post_error(surface_resource,
				       WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "wl_input_panel::get_input_panel_surface already requested");
		return;
	}

	ipsurf = create_input_panel_surface(shell, surface);
	if (!ipsurf) {
		wl_resource_post_error(surface_resource,
				       WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "surface->committed already set");
		return;
	}

	ipsurf->resource =
		wl_resource_create(client,
				   &zwp_input_panel_surface_v1_interface,
				   1,
				   id);
	wl_resource_set_implementation(ipsurf->resource,
				       &input_panel_surface_implementation,
				       ipsurf,
				       destroy_input_panel_surface_resource);
}

static const struct zwp_input_panel_v1_interface input_panel_implementation = {
	input_panel_get_input_panel_surface
};

static void
unbind_input_panel(struct wl_resource *resource)
{
	struct desktop_shell *shell = wl_resource_get_user_data(resource);

	shell->input_panel.binding = NULL;
}

static void
bind_input_panel(struct wl_client *client,
	      void *data, uint32_t version, uint32_t id)
{
	struct desktop_shell *shell = data;
	struct wl_resource *resource;

	resource = wl_resource_create(client,
				      &zwp_input_panel_v1_interface, 1, id);

	if (shell->input_panel.binding == NULL) {
		wl_resource_set_implementation(resource,
					       &input_panel_implementation,
					       shell, unbind_input_panel);
		shell->input_panel.binding = resource;
		return;
	}

	wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
			       "interface object already bound");
}

void
input_panel_destroy(struct desktop_shell *shell)
{
	wl_list_remove(&shell->show_input_panel_listener.link);
}

int
input_panel_setup(struct desktop_shell *shell)
{
	struct weston_compositor *ec = shell->compositor;

	shell->show_input_panel_listener.notify = show_input_panels;
	wl_signal_add(&ec->show_input_panel_signal,
		      &shell->show_input_panel_listener);

	if (wl_global_create(shell->compositor->wl_display,
			     &zwp_input_panel_v1_interface, 1,
			     shell, bind_input_panel) == NULL)
		return -1;

	return 0;
}

