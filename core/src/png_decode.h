#ifndef NNN_PNG_DECODE_H
#define NNN_PNG_DECODE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Internal C-only libpng error boundary. The borrowed segments contain the
 * validated PNG signature and image chunks, plus PLTE/tRNS where applicable. */
struct nnn_png_segment {
    const unsigned char* bytes;
    size_t size;
};

enum nnn_png_decode_code {
    NNN_PNG_DECODE_OK = 0,
    NNN_PNG_DECODE_INVALID = 1,
    NNN_PNG_DECODE_LIMIT = 2
};

struct nnn_png_decode_report {
    int code;
    size_t peak_work_bytes;
    size_t remaining_work_bytes;
    char message[160];
};

int nnn_png_decode_segments(const struct nnn_png_segment* segments,
                            size_t segment_count, uint32_t width,
                            uint32_t height, unsigned source_depth,
                            unsigned source_color_type, unsigned source_interlace,
                            unsigned char* output, size_t output_size,
                            size_t work_limit,
                            struct nnn_png_decode_report* report);

#ifdef __cplusplus
}
#endif
#endif
