#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <systemd/sd-bus.h>
#include <png.h>

typedef struct {
    int x, y;
    uint32_t width, height;
} Region;

typedef struct {
    uint32_t width, height;
    uint32_t stride; // Bytes per row, including any padding supplied by KWin.
    unsigned char *pixels;
} Image;

typedef enum {
    SELECTION_OK = 0,
    SELECTION_FAILED = 1,
    SELECTION_CANCELLED = 2
} SelectionResult;

static SelectionResult select_region(Region *region) {
    FILE *selection_output = popen("slurp -f '%x %y %w %h'", "r");
    if (!selection_output) {
        return SELECTION_FAILED;
    }
    if (fscanf(selection_output, "%d %d %u %u", &region->x, &region->y, &region->width, &region->height) != 4 || region->width == 0 || region->height == 0) {
        pclose(selection_output);
        return SELECTION_CANCELLED;
    }
    pclose(selection_output);
    return SELECTION_OK;
}

static void read_capture_dimensions(sd_bus_message *reply, Image *image) {
    sd_bus_message_enter_container(reply, 'a', "{sv}");
    while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
        const char *key = NULL;
        sd_bus_message_read(reply, "s", &key);
        sd_bus_message_enter_container(reply, 'v', NULL);
        if (strcmp(key, "width") == 0) {
            sd_bus_message_read(reply, "u", &image->width);
        } else if (strcmp(key, "height") == 0) {
            sd_bus_message_read(reply, "u", &image->height);
        } else if (strcmp(key, "stride") == 0) {
            sd_bus_message_read(reply, "u", &image->stride);
        } else {
            sd_bus_message_skip(reply, NULL);
        }
        sd_bus_message_exit_container(reply);
        sd_bus_message_exit_container(reply);
    }
    sd_bus_message_exit_container(reply);
}

// Consumes and closes pixels_fd on both success and failure.
static int read_capture_pixels(int pixels_fd, Image *image) {
    if (image->width == 0 || image->height == 0 || image->width > image->stride / 4) {
        fprintf(stderr, "fast-crop: invalid capture dimensions\n");
        close(pixels_fd);
        return 1;
    }

    size_t total_bytes = (size_t)image->stride * image->height;
    unsigned char *pixels = malloc(total_bytes);
    if (!pixels) {
        perror("fast-crop: capture buffer");
        close(pixels_fd);
        return 1;
    }

    size_t bytes_read = 0;
    while (bytes_read < total_bytes) {
        ssize_t bytes_received = read(pixels_fd, pixels + bytes_read, total_bytes - bytes_read);
        if (bytes_received < 0 && errno == EINTR) {
            continue;
        }
        if (bytes_received < 0) {
            perror("fast-crop: read capture");
        }
        if (bytes_received <= 0) {
            break;
        }
        bytes_read += (size_t)bytes_received;
    }
    close(pixels_fd);
    if (bytes_read != total_bytes) {
        fprintf(stderr, "fast-crop: incomplete capture (%zu of %zu bytes)\n", bytes_read, total_bytes);
        free(pixels);
        return 1;
    }

    image->pixels = pixels;
    return 0;
}

static int capture_region(const Region *region, Image *image) {
    sd_bus *bus = NULL;
    int status = sd_bus_default_user(&bus);
    if (status < 0) {
        fprintf(stderr, "fast-crop: session bus: %s\n", strerror(-status));
        return 1;
    }

    // KWin writes raw pixels to the pipe; the D-Bus reply contains their dimensions.
    int capture_pipe[2];
    if (pipe(capture_pipe) < 0) {
        perror("fast-crop: capture pipe");
        sd_bus_unref(bus);
        return 1;
    }

    sd_bus_message *request = NULL;
    status = sd_bus_message_new_method_call(bus, &request,
        "org.kde.KWin", "/org/kde/KWin/ScreenShot2",
        "org.kde.KWin.ScreenShot2", "CaptureArea");
    if (status >= 0) {
        sd_bus_message_append(request, "iiuu", region->x, region->y, region->width, region->height);
        sd_bus_message_open_container(request, 'a', "{sv}");
        sd_bus_message_close_container(request);
        sd_bus_message_append(request, "h", capture_pipe[1]);
    }

    if (status < 0) {
        fprintf(stderr, "fast-crop: capture request: %s\n", strerror(-status));
        close(capture_pipe[0]);
        close(capture_pipe[1]);
        sd_bus_message_unref(request);
        sd_bus_unref(bus);
        return 1;
    }

    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    status = sd_bus_call(bus, request, 0, &error, &reply);
    close(capture_pipe[1]);
    sd_bus_message_unref(request);

    if (status < 0) {
        fprintf(stderr, "fast-crop: KWin: %s\n", error.message ? error.message : strerror(-status));
        close(capture_pipe[0]);
        sd_bus_error_free(&error);
        sd_bus_unref(bus);
        return 1;
    }

    read_capture_dimensions(reply, image);
    sd_bus_message_unref(reply);
    sd_bus_unref(bus);
    return read_capture_pixels(capture_pipe[0], image);
}

static int write_png_to_stream(FILE *output, unsigned char *pixels, uint32_t width, uint32_t height, uint32_t stride) {
    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        return 1;
    }
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_write_struct(&png_ptr, NULL);
        return 1;
    }
    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        return 1;
    }
    png_init_io(png_ptr, output);
    png_set_compression_level(png_ptr, 1);
    png_set_sRGB_gAMA_and_cHRM(png_ptr, info_ptr, PNG_sRGB_INTENT_PERCEPTUAL);
    png_set_IHDR(png_ptr, info_ptr, width, height, 8,
                 PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png_ptr, info_ptr);

    for (uint32_t y = 0; y < height; y++) {
        png_write_row(png_ptr, pixels + (size_t)y * stride);
    }
    png_write_end(png_ptr, NULL);
    png_destroy_write_struct(&png_ptr, &info_ptr);
    return fflush(output) != 0;
}

static int copy_image_to_clipboard(const Image *image) {
    signal(SIGPIPE, SIG_IGN);
    FILE *clipboard_input = popen("wl-copy --type image/png", "w");
    if (!clipboard_input) {
        perror("fast-crop: start wl-copy");
        return 1;
    }
    int png_failed = write_png_to_stream(clipboard_input, image->pixels, image->width, image->height, image->stride);
    int copy_status = pclose(clipboard_input);
    if (png_failed || copy_status != 0) {
        fprintf(stderr, "fast-crop: PNG encoding or clipboard copy failed\n");
        return 1;
    }
    return 0;
}

int main(void) {
    Region region = {0};
    Image image = {0};

    SelectionResult selection = select_region(&region);
    if (selection == SELECTION_FAILED) {
        return EXIT_FAILURE;
    }
    if (selection == SELECTION_CANCELLED) {
        return EXIT_SUCCESS;
    }

    if (capture_region(&region, &image) != 0) {
        return EXIT_FAILURE;
    }

    int copy_status = copy_image_to_clipboard(&image);
    free(image.pixels);
    if (copy_status != 0) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
