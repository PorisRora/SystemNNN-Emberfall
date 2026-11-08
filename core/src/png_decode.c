#include "png_decode.h"

#include <png.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

union allocation_header {
    size_t size;
    max_align_t alignment;
};

struct decode_state {
    png_structp png;
    png_infop info;
    const struct nnn_png_segment* segments;
    size_t segment_count;
    size_t segment_index;
    size_t segment_position;
    size_t work_limit;
    size_t work_used;
    size_t work_peak;
    int allocation_failed;
    char message[160];
};

static void copy_message(char* target, size_t capacity, const char* message) {
    size_t n = 0;
    if (capacity == 0) return;
    if (message != NULL) {
        while (n + 1 < capacity && message[n] != '\0') {
            target[n] = message[n];
            ++n;
        }
    }
    target[n] = '\0';
}

static void png_failure(png_structp png, png_const_charp message) {
    struct decode_state* state = (struct decode_state*)png_get_error_ptr(png);
    if (state != NULL) copy_message(state->message, sizeof(state->message), message);
    png_longjmp(png, 1);
}

static void png_warning_ignored(png_structp png, png_const_charp message) {
    /* The scanner filters image chunks and validates their framing and CRCs.
     * Decoder warnings do not write to stderr. */
    (void)png;
    (void)message;
}

static png_voidp bounded_malloc(png_structp png, png_alloc_size_t request) {
    struct decode_state* state = (struct decode_state*)png_get_mem_ptr(png);
    union allocation_header* header;
    const size_t bytes = (size_t)request;
    size_t charge;
    if (state == NULL || (png_alloc_size_t)bytes != request || bytes > SIZE_MAX - sizeof(*header) ||
        state->work_used > state->work_limit) {
        if (state != NULL) state->allocation_failed = 1;
        return NULL;
    }
    charge = sizeof(*header) + bytes;
    if (charge > state->work_limit - state->work_used) {
        state->allocation_failed = 1;
        return NULL;
    }
    header = (union allocation_header*)malloc(charge);
    if (header == NULL) {
        state->allocation_failed = 1;
        return NULL;
    }
    header->size = charge;
    state->work_used += charge;
    if (state->work_used > state->work_peak) state->work_peak = state->work_used;
    return (png_voidp)(header + 1);
}

static void bounded_free(png_structp png, png_voidp allocation) {
    struct decode_state* state;
    union allocation_header* header;
    if (allocation == NULL) return;
    state = (struct decode_state*)png_get_mem_ptr(png);
    header = ((union allocation_header*)allocation) - 1;
    if (state != NULL) state->work_used -= header->size;
    free(header);
}

static void read_segments(png_structp png, png_bytep destination, png_size_t requested) {
    struct decode_state* state = (struct decode_state*)png_get_io_ptr(png);
    size_t remaining = (size_t)requested;
    while (remaining != 0) {
        const struct nnn_png_segment* segment;
        size_t available;
        size_t count;
        while (state->segment_index < state->segment_count &&
               state->segment_position == state->segments[state->segment_index].size) {
            ++state->segment_index;
            state->segment_position = 0;
        }
        if (state->segment_index >= state->segment_count) png_error(png, "Truncated bounded PNG feed");
        segment = &state->segments[state->segment_index];
        available = segment->size - state->segment_position;
        count = remaining < available ? remaining : available;
        memcpy(destination, segment->bytes + state->segment_position, count);
        destination += count;
        state->segment_position += count;
        remaining -= count;
    }
}

static void expand_palette(struct decode_state* state, unsigned char* output,
                           uint32_t width, uint32_t height) {
    png_colorp palette = NULL;
    png_bytep alpha = NULL;
    int colors = 0, alpha_count = 0;
    uint32_t y;
    if (png_get_PLTE(state->png, state->info, &palette, &colors) == 0 || palette == NULL || colors <= 0) {
        png_error(state->png, "Indexed PNG is missing a palette");
    }
    png_get_tRNS(state->png, state->info, &alpha, &alpha_count, NULL);
    for (y = 0; y < height; ++y) {
        unsigned char* row = output + (size_t)y * width * 4U;
        size_t x = width;
        /* The unpacked indices occupy the first width bytes of each row. */
        while (x != 0U) {
            unsigned char* pixel;
            int index;
            --x;
            index = row[x];
            if (index >= colors) png_error(state->png, "PNG index exceeds the palette");
            pixel = row + x * 4U;
            pixel[0] = palette[index].red;
            pixel[1] = palette[index].green;
            pixel[2] = palette[index].blue;
            pixel[3] = alpha != NULL && index < alpha_count ? alpha[index] : 255U;
        }
    }
}

int nnn_png_decode_segments(const struct nnn_png_segment* segments,
                            size_t segment_count, uint32_t width,
                            uint32_t height, unsigned source_depth,
                            unsigned source_color_type, unsigned source_interlace,
                            unsigned char* output, size_t output_size,
                            size_t work_limit,
                            struct nnn_png_decode_report* report) {
    /* All state modified after setjmp is heap-owned. No changed, non-volatile
     * automatic variable is read after longjmp; no C++ object is in this frame. */
    struct decode_state* const state = (struct decode_state*)calloc(1, sizeof(struct decode_state));
    int result;
    if (report != NULL) memset(report, 0, sizeof(*report));
    if (state == NULL) {
        if (report != NULL) {
            report->code = NNN_PNG_DECODE_LIMIT;
            copy_message(report->message, sizeof(report->message), "Cannot allocate PNG error-boundary state");
        }
        return NNN_PNG_DECODE_LIMIT;
    }
    if (segments == NULL || segment_count == 0 || width == 0 || height == 0 ||
        width > 8192U || height > 8192U ||
        (size_t)width > SIZE_MAX / 4U / (size_t)height || output == NULL ||
        output_size != (size_t)width * (size_t)height * 4U) {
        if (report != NULL) report->code = NNN_PNG_DECODE_INVALID;
        free(state);
        return NNN_PNG_DECODE_INVALID;
    }
    state->segments = segments;
    state->segment_count = segment_count;
    state->work_limit = work_limit;
    state->png = png_create_read_struct_2(PNG_LIBPNG_VER_STRING, state,
                                        png_failure, png_warning_ignored,
                                        state, bounded_malloc, bounded_free);
    if (state->png == NULL) {
        result = NNN_PNG_DECODE_LIMIT;
        copy_message(state->message, sizeof(state->message), "Cannot allocate bounded libpng state");
    } else if (setjmp(png_jmpbuf(state->png)) != 0) {
        result = state->allocation_failed ? NNN_PNG_DECODE_LIMIT : NNN_PNG_DECODE_INVALID;
    } else {
        png_uint_32 actual_width = 0, actual_height = 0;
        int depth = 0, color = 0, interlace = 0, compression = 0, filter = 0;
        int pass, passes, channels;
        const png_size_t output_stride = (png_size_t)width * 4U;
        state->info = png_create_info_struct(state->png);
        if (state->info == NULL) png_error(state->png, "Cannot allocate bounded PNG info");
        png_set_user_limits(state->png, 8192U, 8192U);
        png_set_chunk_malloc_max(state->png, (png_alloc_size_t)work_limit);
        png_set_chunk_cache_max(state->png, 8U);
        png_set_crc_action(state->png, PNG_CRC_ERROR_QUIT, PNG_CRC_ERROR_QUIT);
        png_set_read_fn(state->png, state, read_segments);
        png_read_info(state->png, state->info);
        if (png_get_IHDR(state->png, state->info, &actual_width, &actual_height,
                         &depth, &color, &interlace, &compression, &filter) == 0 ||
            actual_width != width || actual_height != height || depth != (int)source_depth ||
            color != (int)source_color_type || interlace != (int)source_interlace || compression != 0 || filter != 0) {
            png_error(state->png, "PNG profile differs from checked input");
        }
        if (color == PNG_COLOR_TYPE_PALETTE) {
            png_set_packing(state->png);
            channels = 1;
        } else {
            const int has_trns = png_get_valid(state->png, state->info, PNG_INFO_tRNS) != 0U;
            if (has_trns) png_set_tRNS_to_alpha(state->png);
            if (depth == 16) png_set_strip_16(state->png);
            if (color == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(state->png);
            if ((color & PNG_COLOR_MASK_COLOR) == 0) png_set_gray_to_rgb(state->png);
            if (!has_trns && (color & PNG_COLOR_MASK_ALPHA) == 0) png_set_filler(state->png, 0xffffU, PNG_FILLER_AFTER);
            channels = 4;
        }
        passes = png_set_interlace_handling(state->png);
        png_read_update_info(state->png, state->info);
        if (png_get_rowbytes(state->png, state->info) != (png_size_t)width * (png_size_t)channels ||
            png_get_channels(state->png, state->info) != channels) png_error(state->png, "Unexpected PNG output layout");
        for (pass = 0; pass < passes; ++pass) {
            uint32_t row;
            for (row = 0; row < height; ++row) {
                png_read_row(state->png, output + (size_t)row * output_stride, NULL);
            }
        }
        png_read_end(state->png, NULL);
        if (color == PNG_COLOR_TYPE_PALETTE) expand_palette(state, output, width, height);
        result = NNN_PNG_DECODE_OK;
    }
    if (state->png != NULL) png_destroy_read_struct(&state->png, state->info != NULL ? &state->info : NULL, NULL);
    if (report != NULL) {
        report->code = result;
        report->peak_work_bytes = state->work_peak;
        report->remaining_work_bytes = state->work_used;
        copy_message(report->message, sizeof(report->message), state->message);
    }
    free(state);
    return result;
}
