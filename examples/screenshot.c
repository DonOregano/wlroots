/*
 * Copyright © 2008 Kristian Høgsberg
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

#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <wayland-client.h>
#include <limits.h>
#include <sys/param.h>
#include <screenshooter-client-protocol.h>
#include "util/os-compatibility.h"
#include <wlr/util/log.h>

static struct wl_shm *shm = NULL;
static struct orbital_screenshooter *screenshooter = NULL;
static struct wl_list output_list;
int min_x, min_y, max_x, max_y;
int buffer_copy_done;

struct screenshooter_output {
	struct wl_output *output;
	struct wl_buffer *buffer;
	int width, height, offset_x, offset_y;
	enum wl_output_transform transform;
	void *data;
	struct wl_list link;
};

static void output_handle_geometry(void *data, struct wl_output *wl_output,
		int x, int y, int physical_width, int physical_height, int subpixel,
		const char *make, const char *model, int transform) {
	struct screenshooter_output *output = wl_output_get_user_data(wl_output);

	if (wl_output == output->output) {
		output->offset_x = x;
		output->offset_y = y;
		output->transform = transform;
	}
}

static void output_handle_mode(void *data, struct wl_output *wl_output,
		uint32_t flags, int width, int height, int refresh) {
	struct screenshooter_output *output = wl_output_get_user_data(wl_output);

	if (wl_output == output->output && (flags & WL_OUTPUT_MODE_CURRENT)) {
		output->width = width;
		output->height = height;
	}
}

static void output_handle_done(void *data, struct wl_output *wl_output) {
	// No-op
}

static const struct wl_output_listener output_listener = {
	.geometry = output_handle_geometry,
	.mode = output_handle_mode,
	.done = output_handle_done,
};

static void screenshot_done(void *data, struct orbital_screenshot *screenshot) {
	buffer_copy_done = 1;
}

static const struct orbital_screenshot_listener screenshot_listener = {
	.done = screenshot_done,
};

static void handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	static struct screenshooter_output *output;

	if (strcmp(interface, "wl_output") == 0) {
		output = calloc(1, sizeof(*output));
		output->output = wl_registry_bind(registry, name, &wl_output_interface,
			1);
		wl_list_insert(&output_list, &output->link);
		wl_output_add_listener(output->output, &output_listener, output);
	} else if (strcmp(interface, "wl_shm") == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, "orbital_screenshooter") == 0) {
		screenshooter = wl_registry_bind(registry, name,
			&orbital_screenshooter_interface, 1);
	}
}

static void handle_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
	// Unimplemented
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

static struct wl_buffer *create_shm_buffer(int width, int height,
		void **data_out) {
	int stride = width * 4;
	int size = stride * height;

	int fd = os_create_anonymous_file(size);
	if (fd < 0) {
		fprintf(stderr, "creating a buffer file for %d B failed: %m\n", size);
		return NULL;
	}

	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		fprintf(stderr, "mmap failed: %m\n");
		close(fd);
		return NULL;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	close(fd);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
		stride, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);

	*data_out = data;

	return buffer;
}

static void write_image(const char *filename, int width, int height) {
	int buffer_stride = width * 4;

	void *data = calloc(1, buffer_stride * height);
	if (!data) {
		return;
	}

	struct screenshooter_output *output, *next;
	wl_list_for_each_safe(output, next, &output_list, link) {
		int output_stride = output->width * 4;
		uint32_t *src = (uint32_t *)output->data;
		uint32_t *dst = (uint32_t *)(data +
			(output->offset_y - min_y) * buffer_stride +
			(output->offset_x - min_x) * 4);

		switch (output->transform) {
		case WL_OUTPUT_TRANSFORM_NORMAL:
			for (int i = 0; i < output->height; i++) {
				memcpy(dst, src, output_stride);
				dst += width;
				src += output->width;
			}
			break;
		case WL_OUTPUT_TRANSFORM_FLIPPED:
			for (int i = 0; i < output->height; ++i) {
				for (int j = 0; j < output->width; ++j) {
					dst[i * width + j] =
						src[i * output->width + output->width - 1 - j];
				}
			}
			break;
		case WL_OUTPUT_TRANSFORM_90:
			for (int i = 0; i < output->width; ++i) {
				for (int j = 0; j < output->height; ++j) {
					dst[i * width + j] =
						src[j * output->width + output->width - 1 - i];
				}
			}
			break;
		case WL_OUTPUT_TRANSFORM_FLIPPED_90:
			for (int i = 0; i < output->width; ++i) {
				for (int j = 0; j < output->height; ++j) {
					dst[i * width + j] =
						src[(output->height - 1 - j) * output->width + output->width - 1 - i];
				}
			}
			break;
		case WL_OUTPUT_TRANSFORM_180:
			for (int i = 0; i < output->height; ++i) {
				for (int j = 0; j < output->width; ++j) {
					dst[i * width + j] =
						src[(output->height - 1 - i) * output->width + output->width - 1 - j];
				}
			}
			break;
		case WL_OUTPUT_TRANSFORM_FLIPPED_180:
			for (int i = 0; i < output->height; ++i) {
				for (int j = 0; j < output->width; ++j) {
					dst[i * width + j] =
						src[(output->height - 1 - i) * output->width + j];
				}
			}
			break;
		case WL_OUTPUT_TRANSFORM_270:
			for (int i = 0; i < output->width; ++i) {
				for (int j = 0; j < output->height; ++j) {
					dst[i * width + j] =
						src[(output->height - 1 - j) * output->width + i];
				}
			}
			break;
		case WL_OUTPUT_TRANSFORM_FLIPPED_270:
			for (int i = 0; i < output->width; ++i) {
				for (int j = 0; j < output->height; ++j) {
					dst[i * width + j] =
						src[j * output->width + i];
				}
			}
			break;
		}

		free(output);
	}

	char size[10 + 1 + 10 + 2 + 1]; // int32_t are max 10 digits
	sprintf(size, "%dx%d+0", width, height);

	int fd[2];
	if (pipe(fd) != 0) {
		fprintf(stderr, "cannot create pipe: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	pid_t child = fork();
	if (child < 0) {
		fprintf(stderr, "fork() failed\n");
		exit(EXIT_FAILURE);
	} else if (child != 0) {
		close(fd[0]);
		if (write(fd[1], data, buffer_stride * height) < 0) {
			fprintf(stderr, "write() failed: %s\n", strerror(errno));
			exit(EXIT_FAILURE);
		}
		close(fd[1]);
		free(data);
		waitpid(child, NULL, 0);
	} else {
		close(fd[1]);
		if (dup2(fd[0], 0) != 0) {
			fprintf(stderr, "cannot dup the pipe\n");
			exit(EXIT_FAILURE);
		}
		close(fd[0]);
		// We requested WL_SHM_FORMAT_XRGB8888 in little endian, so that's BGRA
		// in big endian.
		execlp("convert", "convert", "-depth", "8", "-size", size, "bgra:-",
			"-alpha", "opaque", filename, NULL);
		fprintf(stderr, "cannot execute convert\n");
		exit(EXIT_FAILURE);
	}
}

static int set_buffer_size(int *width, int *height) {
	int owidth, oheight;
	min_x = min_y = INT_MAX;
	max_x = max_y = INT_MIN;

	struct screenshooter_output *output;
	wl_list_for_each(output, &output_list, link) {
		if (output->transform & 0x1) {
			owidth = output->height;
			oheight = output->width;
		} else {
			owidth = output->width;
			oheight = output->height;
		}
		min_x = MIN(min_x, output->offset_x);
		min_y = MIN(min_y, output->offset_y);
		max_x = MAX(max_x, output->offset_x + owidth);
		max_y = MAX(max_y, output->offset_y + oheight);
	}

	if (max_x <= min_x || max_y <= min_y) {
		return -1;
	}

	*width = max_x - min_x;
	*height = max_y - min_y;

	return 0;
}

int main(int argc, char *argv[]) {
	wlr_log_init(L_DEBUG, NULL);
	struct wl_display * display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "failed to create display: %m\n");
		return -1;
	}

	wl_list_init(&output_list);
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_dispatch(display);
	wl_display_roundtrip(display);

	if (screenshooter == NULL) {
		fprintf(stderr, "display doesn't support screenshooter\n");
		return -1;
	}

	int width, height;
	if (set_buffer_size(&width, &height)) {
		fprintf(stderr, "cannot set buffer size\n");
		return -1;
	}

	struct screenshooter_output *output;
	wl_list_for_each(output, &output_list, link) {
		output->buffer = create_shm_buffer(output->width, output->height, &output->data);
		if (output->buffer == NULL) {
			return -1;
		}
		struct orbital_screenshot *screenshot = orbital_screenshooter_shoot(
			screenshooter, output->output, output->buffer);
		orbital_screenshot_add_listener(screenshot, &screenshot_listener, screenshot);
		buffer_copy_done = 0;
		while (!buffer_copy_done) {
			wl_display_roundtrip(display);
		}
	}

	write_image("wayland-screenshot.png", width, height);
	return EXIT_SUCCESS;
}
