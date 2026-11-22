/* image.h - picture decoding (PNG and BMP), shared by the kernel (wallpaper)
 * and programs (Picture viewer). JPEG lives in jpeg.c. It needs nothing but a memory allocator. */
#ifndef IMAGE_H
#define IMAGE_H
#include <stdint.h>

#define IMAGE_MAX_PIXELS (6u * 1024 * 1024)   /* e.g. 3000 x 2000 */

struct image {
    int w, h;
    uint32_t *px;          /* w * h pixels, 0xAARRGGBB (AA = 255 is solid, 0 is see-through) */
    int has_alpha;         /* some pixels are not fully solid */
    const char *format;    /* "PNG", "JPEG", "GIF" or "BMP" */
};

/* Decode a whole picture file held in memory. Returns 0 on success, or a short
 * message saying what is wrong. On success, free out->px with the same release
 * function that goes with alloc. */
const char *image_decode(const uint8_t *data, uint32_t len, struct image *out,
                         void *(*alloc)(uint32_t), void (*release)(void *));

/* Resize the box (sx, sy, sw, sh) of src into dst (dw x dh pixels): smooth when shrinking. */
void image_resample(const struct image *src, int sx, int sy, int sw, int sh, uint32_t *dst, int dw, int dh);

/* JPEG (jpeg.c); normally called through image_decode. */
const char *jpeg_decode(const uint8_t *data, uint32_t len, struct image *out,
                        void *(*alloc)(uint32_t), void (*release)(void *));

/* Like jpeg_decode; small = 1 gives a 1/8 size picture, very fast (for previews). */
const char *jpeg_decode_ex(const uint8_t *data, uint32_t len, struct image *out,
                           void *(*alloc)(uint32_t), void (*release)(void *), int small);

/* A preview: the picture shrunk to fit max_w x max_h (big JPEGs are read in the fast 1/8 mode). */
const char *image_thumbnail(const uint8_t *data, uint32_t len, struct image *out, int max_w, int max_h,
                            void *(*alloc)(uint32_t), void (*release)(void *));

/* Does this file name end in .png, .jpg, .jpeg, .gif or .bmp? */
int image_is_picture_name(const char *name);
#endif
