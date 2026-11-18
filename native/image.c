//------------------------------------------------------------------------
// image.c -- the Clay plugin for stb images
//
//  It registers one prototype, ImageCore, whose objects each own a C struct.
//  src/main.clay wraps it in a Clay API: Image is a clone of ImageCore, so
//  images have these methods. The ones named with a leading _ are internal
//  (the constructors use them); the rest are public. The C API is in 
//  clay_plugin.h (see "Native plugins" in the Clay guide): values cross as
//  opaque handles, arguments are borrowed, and what a method returns is a 
//  new reference (or its own receiver). The entry point must be named 
//  <plugin>_plugin_init.
//------------------------------------------------------------------------

#include "clay_plugin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//------------------------------------------------------------------------
// stb image implementation
//------------------------------------------------------------------------
#define STB_IMAGE_IMPLEMENTATION
// stbi_failure_reason() gives messages written for users ("Image not of any
// known type, or corrupt") instead of terse ones ("unknown image type")
#define STBI_FAILURE_USERMSG
#   include "stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#   include "stb/stb_image_write.h"

//------------------------------------------------------------------------
// the Clay plugin API instance
//------------------------------------------------------------------------
static const ClayPluginAPI *api;

//------------------------------------------------------------------------
// minimum API version required by this plugin
//------------------------------------------------------------------------
#define MIN_API_VERSION 2

#define SMALL_BUF 128
#define LARGE_BUF 512

//------------------------------------------------------------------------
// what each image owns
//------------------------------------------------------------------------
typedef struct {
    unsigned char *data;    // pixel data
    int width;
    int height;
    int channels;
} Image;

static void replace_pixels(Image *img, unsigned char *data, int w, int h, int c);

// canvas.c registers CanvasCore
void canvas_register(const ClayPluginAPI *clay_api);

//------------------------------------------------------------------------
// Image free callback
//
// It runs when the Clay object is freed and releases the image's data
//------------------------------------------------------------------------
static void image_free(void *data) { 
    Image *img = (Image *)data;

    if (img) {
        if (img->data) 
            stbi_image_free(img->data);
        img->data = NULL;
        free(img);
    }
}

//------------------------------------------------------------------------
// Image 'of' helper function
//
// It retrieves the C struct from the Clay object
//------------------------------------------------------------------------
static Image *image_of(ClayPluginObject self) {
    Image *img = (Image *)api->get_data(self);

    if (!img) 
        api->raise("Image: not an image", "RuntimeError");

    return img;
}

//------------------------------------------------------------------------
// ImageCore _create -> a new, empty image; self is the prototype
//
// It allocates the C struct and associates it with the Clay object
//------------------------------------------------------------------------
static ClayPluginValue image_create(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)argc; (void)argv;

    Image *img = (Image *)calloc(1, sizeof(Image));

    if (!img) {
        api->raise("Image: out of memory", "RuntimeError");
        return 0;
    }

    ClayPluginObject obj = api->clone(self);

    api->set_data(obj, img);

    return (ClayPluginValue)obj;
}

//------------------------------------------------------------------------
// image _read(path)
//
// It loads the image from the specified path and updates the image's 
// data, width, height, and channels
//------------------------------------------------------------------------
static ClayPluginValue image_read(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    Image *img = image_of(self);
    
    if (!img) 
        return 0;
    
    // check arguments
    if (argc < 1 || !api->is_string(argv[0])) {
        api->raise("Image: load expects a path String", "ArgumentError");
        return 0;
    }

    // load the image and set its data, width, height and channels
    // load into locals so a failed load leaves the image as it was
    int w, h, c;
    const char *path = api->to_string(argv[0]);
    unsigned char *data = stbi_load(path, &w, &h, &c, 0);

    // check if the image was loaded successfully, and if not say why
    if (!data) {
        const char *reason = stbi_failure_reason();
        char msg[LARGE_BUF];

        // a very long path is cut short by snprintf, which is fine for a message
        snprintf(msg, sizeof msg, "Image: can't load '%s': %s", path, reason ? reason : "unknown error");
        api->raise(msg, "RuntimeError");
        return 0;
    }
    
    // release any image loaded before
    replace_pixels(img, data, w, h, c);

    // on success, return self for message chaining
    return (ClayPluginValue)self;
}

//------------------------------------------------------------------------
// Image savePng(path)
//
// It saves the image as a PNG file
//------------------------------------------------------------------------
static ClayPluginValue image_save_png(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    Image *img = image_of(self);

    // ensure the image object is valid
    if (!img) 
        return 0;

    // check arguments
    if (argc < 1 || !api->is_string(argv[0])) {
        api->raise("Image: savePng expects a string", "ArgumentError");
        return 0;
    }

    // ensure the image has data before saving
    if (!img->data) {
        api->raise("Image: no image loaded", "RuntimeError");
        return 0;
    }

    // attempt to write the image as a PNG file
    if (!stbi_write_png(api->to_string(argv[0]), img->width, img->height, img->channels, img->data, img->width * img->channels)) {
        api->raise("Image: Failed to save image", "RuntimeError");
        return 0;
    }

    // on success, return self for message chaining
    return (ClayPluginValue)self;
}

//------------------------------------------------------------------------
// ImageCore _probe(path) -> [width, height, channels], or nil
//
// It reads only the file's header, without decoding the pixels, so it is
// cheap. nil means stb can't read the file as an image. Image info wraps
// it in a Dict. It needs no image, so self can be any ImageCore object.
//------------------------------------------------------------------------
static ClayPluginValue image_probe(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)self;

    // check arguments
    if (argc < 1 || !api->is_string(argv[0])) {
        api->raise("Image: info expects a path String", "ArgumentError");
        return 0;
    }

    int w, h, c;

    if (!stbi_info(api->to_string(argv[0]), &w, &h, &c))
        return api->make_nil();

    ClayPluginValue arr = api->array_new();

    api->array_push(arr, api->make_int(w));
    api->array_push(arr, api->make_int(h));
    api->array_push(arr, api->make_int(c));

    return arr;
}

//------------------------------------------------------------------------
// Image width
//
// -> the image's width
//------------------------------------------------------------------------
static ClayPluginValue image_width(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)argc; (void)argv;

    // retrieve the image object from the plugin object
    Image *img = image_of(self);

    // ensure the image object is valid
    if (!img) 
        return 0;

    // return the image's width
    return api->make_int(img->width);
}

//------------------------------------------------------------------------
// Image height
//
// -> the image's height
//------------------------------------------------------------------------
static ClayPluginValue image_height(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)argc; (void)argv;

    // retrieve the image object from the plugin object
    Image *img = image_of(self);

    // ensure the image object is valid
    if (!img) 
        return 0;

    // return the image's height
    return api->make_int(img->height);
}

//------------------------------------------------------------------------
// Image channels
//
// -> the number of channels (1 gray, 2 gray+alpha, 3 RGB, 4 RGBA)
//------------------------------------------------------------------------
static ClayPluginValue image_channels(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)argc; (void)argv;

    // retrieve the image object from the plugin object
    Image *img = image_of(self);

    // ensure the image object is valid
    if (!img)
        return 0;

    // return the number of channels
    return api->make_int(img->channels);
}

//[]------------------------------------------------------------------------[]
// shared helpers for reading and replacing pixels
//[]------------------------------------------------------------------------[]

//------------------------------------------------------------------------
// the image's loaded pixels, or raise
//------------------------------------------------------------------------
static Image *loaded_image_of(ClayPluginObject self) {
    // retrieve the image object from the plugin object
    Image *img = image_of(self);

    // ensure the image object is valid
    if (img && !img->data) {
        api->raise("Image: no image loaded", "RuntimeError");
        return NULL;
    }
    
    // return the image object if it has loaded pixels, or NULL if not
    return img;
}

//------------------------------------------------------------------------
// read and check (width, height, channels) from argv[0..2]; returns the
// pixel byte count, or 0 after raising
//------------------------------------------------------------------------
static size_t read_shape(const char *method, int argc, ClayPluginValue *argv, int *w, int *h, int *c) {
    char msg[SMALL_BUF];

    if (argc < 4 || !api->is_int(argv[0]) || !api->is_int(argv[1]) || !api->is_int(argv[2])) {
        snprintf(msg, sizeof msg, "Image: %s expects (width, height, channels, data)", method);
        api->raise(msg, "ArgumentError");
        return 0;
    }

    intptr_t iw = api->to_int(argv[0]), ih = api->to_int(argv[1]), ic = api->to_int(argv[2]);

    // limit each side so width * height * channels can't overflow
    if (iw < 1 || ih < 1 || iw > 65535 || ih > 65535 || ic < 1 || ic > 4) {
        snprintf(msg, sizeof msg, "Image: %s needs width and height 1..65535 and channels 1..4", method);
        api->raise(msg, "ArgumentError");
        return 0;
    }

    *w = (int)iw; *h = (int)ih; *c = (int)ic;

    return (size_t)iw * (size_t)ih * (size_t)ic;
}

//------------------------------------------------------------------------
// give the image new pixels, freeing the old ones; data must come from STBI_MALLOC
//------------------------------------------------------------------------
static void replace_pixels(Image *img, unsigned char *data, int w, int h, int c) {
    if (img->data)
        stbi_image_free(img->data);

    img->data = data;
    img->width = w;
    img->height = h;
    img->channels = c;
}

//------------------------------------------------------------------------
// Image pixels -> the raw pixels as a String of bytes: row by row, top to
// bottom, channels interleaved (RGBARGBA...), width * height * channels long
//------------------------------------------------------------------------
static ClayPluginValue image_pixels(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)argc; (void)argv;

    if (api->api_version < 4) {
        api->raise("Image: pixels needs Clay plugin API 4", "RuntimeError");
        return 0;
    }

    Image *img = loaded_image_of(self);

    if (!img)
        return 0;

    return api->make_bytes(img->data, (size_t)img->width * img->height * img->channels);
}

//------------------------------------------------------------------------
// image _setPixels(width, height, channels, bytes) 
//
// -> the image (behind Image fromBytes) bytes is a String laid out 
// as `pixels` returns
//------------------------------------------------------------------------
static ClayPluginValue image_set_pixels(ClayPluginObject self, int argc, ClayPluginValue *argv) {

    // ensure the image object is valid and the API version is sufficient
    if (api->api_version < 4) {
        api->raise("Image: fromBytes needs Clay plugin API 4", "RuntimeError");
        return 0;
    }

    // ensure the method name matches the function being implemented
    Image *img = image_of(self);

    // ensure the image object is valid
    if (!img)
        return 0;

    int w, h, c;
    size_t n = read_shape("fromBytes", argc, argv, &w, &h, &c);

    // ensure the shape was read successfully
    if (!n)
        return 0;

    size_t len = 0;
    const char *bytes = api->to_bytes(argv[3], &len);

    // ensure the bytes argument is valid
    if (!bytes) {
        api->raise("Image: fromBytes expects a String of bytes", "ArgumentError");
        return 0;
    }

    // ensure the byte count matches the expected shape
    if (len != n) {
        api->raise("Image: fromBytes data must be width * height * channels bytes", "ArgumentError");
        return 0;
    }

    // allocate memory for the new pixel data
    unsigned char *data = (unsigned char *)STBI_MALLOC(n);

    // check if memory allocation succeeded
    if (!data) {
        api->raise("Image: out of memory", "RuntimeError");
        return 0;
    }

    // copy the bytes into the newly allocated memory and replace the image's pixels
    memcpy(data, bytes, n);
    replace_pixels(img, data, w, h, c);

    // return the image object itself for method chaining
    return (ClayPluginValue)self;
}

//------------------------------------------------------------------------
// Image floats
//
// Return the pixels as a flat Array of numbers in 0..1 (byte / 255),
// in the same order as `pixels`: ready to feed a neural network
//------------------------------------------------------------------------
static ClayPluginValue image_floats(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(argc); 
    CLAY_UNUSED(argv);

    // get the loaded image object
    Image *img = loaded_image_of(self);

    // ensure the image is loaded
    if (!img)
        return 0;

    // compute the total number of pixels times channels
    size_t n = (size_t)img->width * img->height * img->channels;
    float *f = (float *)malloc(n * sizeof(float));

    // allocate memory for the float array
    if (!f) {
        api->raise("Image: out of memory", "RuntimeError");
        return 0;
    }

    // convert each byte to a float in 0..1
    for (size_t i = 0; i < n; i++)
        f[i] = img->data[i] / 255.0f;

    // create a Clay array from the float data
    ClayPluginValue arr = api->from_floats(f, n, 0);

    // free the temporary float array after creating the Clay array
    free(f);

    // return the Clay array containing the float representation of the image pixels
    return arr;
}

//------------------------------------------------------------------------
// image _setFloats(width, height, channels, numbers)
//
// -> the image (behind Image fromFloats)
// numbers is an Array (flat, or rows) of values in 0..1, laid out as `floats`
// returns; values outside 0..1 are clamped. A spectrogram makes a 1-channel
// (grayscale) image this way.
//------------------------------------------------------------------------
static ClayPluginValue image_set_floats(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    // get the image object associated with this Clay object
    Image *img = image_of(self);

    // ensure the image object is valid
    if (!img)
        return 0;

    int w, h, c;
    // read the shape (width, height, channels) from the arguments
    size_t n = read_shape("fromFloats", argc, argv, &w, &h, &c);

    if (!n)
        return 0;

    // ensure the fourth argument is an array of numbers
    if (!api->is_array(argv[3])) {
        api->raise("Image: fromFloats expects an Array of numbers", "ArgumentError");
        return 0;
    }

    // ensure the array has the correct number of elements
    if (api->to_floats(argv[3], NULL, 0) != n) {
        api->raise("Image: fromFloats data must be width * height * channels numbers", "ArgumentError");
        return 0;
    }

    // allocate memory for the float array and the image data
    float *f = (float *)malloc(n * sizeof(float));
    unsigned char *data = (unsigned char *)STBI_MALLOC(n);

    // check if memory allocation succeeded
    if (!f || !data) {
        free(f);
        STBI_FREE(data);
        api->raise("Image: out of memory", "RuntimeError");
        return 0;
    }

    // convert the Clay array to floats
    api->to_floats(argv[3], f, n);

    // clamp the float values to 0..1 and convert to bytes
    for (size_t i = 0; i < n; i++) {
        float v = f[i];

        // !(v > 0) also catches NaN
        v = !(v > 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v);
        data[i] = (unsigned char)(v * 255.0f + 0.5f);
    }

    // free the temporary float array and replace the image's pixels
    free(f);
    replace_pixels(img, data, w, h, c);

    // return the updated image object
    return (ClayPluginValue)self;
}

//------------------------------------------------------------------------
// a growing buffer stb_image_write's PNG encoder writes into
//------------------------------------------------------------------------
typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
    int failed;
} ByteBuf;

static void bytebuf_write(void *context, void *data, int size) {
    ByteBuf *b = (ByteBuf *)context;

    if (b->failed || size <= 0)
        return;

    if (b->len + (size_t)size > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;

        while (cap < b->len + (size_t)size)
            cap *= 2;

        unsigned char *grown = (unsigned char *)realloc(b->data, cap);
        if (!grown) {
            b->failed = 1;
            return;
        }

        b->data = grown;
        b->cap = cap;
    }

    memcpy(b->data + b->len, data, (size_t)size);
    b->len += (size_t)size;
}

//------------------------------------------------------------------------
// image pngBytes
//
// -> the image encoded as a PNG file, a String of bytes: what savePng
// writes, without the file (to embed in an SVG or send over HTTP)
//------------------------------------------------------------------------
static ClayPluginValue image_png_bytes(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(argc);
    CLAY_UNUSED(argv);

    if (api->api_version < 4) {
        api->raise("Image: pngBytes needs Clay plugin API 4", "RuntimeError");
        return 0;
    }

    Image *img = loaded_image_of(self);

    if (!img)
        return 0;

    ByteBuf buf = { NULL, 0, 0, 0 };
    int ok = stbi_write_png_to_func(bytebuf_write, &buf, img->width, img->height, img->channels,
                                    img->data, img->width * img->channels);

    if (!ok || buf.failed) {
        free(buf.data);
        api->raise("Image: couldn't encode the PNG", "RuntimeError");
        return 0;
    }

    ClayPluginValue bytes = api->make_bytes(buf.data, buf.len);
    free(buf.data);
    return bytes;
}

//------------------------------------------------------------------------
// image _setMapped(width, height, numbers, palette, lo, hi)
//
// -> the image (behind Image colormap)
// Each of the width * height numbers (flat, or rows) picks a colour from
// palette, a String of r, g, b bytes for 2 or more colours: lo the first,
// hi the last, and the ones between in proportion, blending neighbouring
// colours. Values outside lo..hi take the end colours. lo and hi may be nil:
// then they are the smallest and largest numbers. A NaN leaves its pixel
// transparent, and makes the image RGBA; otherwise it is RGB.
//------------------------------------------------------------------------
static ClayPluginValue image_set_mapped(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    if (api->api_version < 4) {
        api->raise("Image: colormap needs Clay plugin API 4", "RuntimeError");
        return 0;
    }

    Image *img = image_of(self);

    if (!img)
        return 0;

    if (argc < 4 || !api->is_int(argv[0]) || !api->is_int(argv[1])) {
        api->raise("Image: colormap expects (width, height, numbers, palette, lo, hi)", "ArgumentError");
        return 0;
    }

    intptr_t iw = api->to_int(argv[0]), ih = api->to_int(argv[1]);

    if (iw < 1 || ih < 1 || iw > 65536 || ih > 65536 || (size_t)iw * (size_t)ih > ((size_t)1 << 28)) {
        api->raise("Image: colormap width and height must be 1 to 65536 and not too large together", "ArgumentError");
        return 0;
    }

    int w = (int)iw, h = (int)ih;
    size_t n = (size_t)w * h;

    if (!api->is_array(argv[2]) || api->to_floats(argv[2], NULL, 0) != n) {
        api->raise("Image: colormap needs width * height numbers (flat, or rows)", "ArgumentError");
        return 0;
    }

    size_t plen = 0;
    const unsigned char *pal = (const unsigned char *)api->to_bytes(argv[3], &plen);

    if (!pal || plen < 6 || plen % 3 != 0) {
        api->raise("Image: colormap palette must be a String of r, g, b bytes for 2 or more colours", "ArgumentError");
        return 0;
    }

    float *f = (float *)malloc(n * sizeof(float));

    if (!f) {
        api->raise("Image: out of memory", "RuntimeError");
        return 0;
    }

    api->to_floats(argv[2], f, n);

    // the range: given, or the data's own (ignoring NaN)
    int has_nan = 0;
    float lo = 0.0f, hi = 0.0f;
    int seen = 0;

    for (size_t i = 0; i < n; i++) {
        float v = f[i];

        if (v != v) {
            has_nan = 1;
            continue;
        }

        if (!seen || v < lo) lo = v;
        if (!seen || v > hi) hi = v;
        seen = 1;
    }

    if (argc > 4 && !api->is_nil(argv[4])) lo = api->is_int(argv[4]) ? (float)api->to_int(argv[4]) : api->to_float(argv[4]);
    if (argc > 5 && !api->is_nil(argv[5])) hi = api->is_int(argv[5]) ? (float)api->to_int(argv[5]) : api->to_float(argv[5]);

    int c = has_nan ? 4 : 3;
    unsigned char *data = (unsigned char *)STBI_MALLOC(n * (size_t)c);

    if (!data) {
        free(f);
        api->raise("Image: out of memory", "RuntimeError");
        return 0;
    }

    int colors = (int)(plen / 3);
    float span = hi - lo;

    for (size_t i = 0; i < n; i++) {
        unsigned char *px = data + i * (size_t)c;
        float v = f[i];

        if (v != v) {
            px[0] = px[1] = px[2] = px[3] = 0;
            continue;
        }

        // position along the palette, 0 .. colors - 1
        float t = span > 0.0f ? (v - lo) / span : 0.5f;
        t = !(t > 0.0f) ? 0.0f : (t > 1.0f ? 1.0f : t);

        float pos = t * (float)(colors - 1);
        int k = (int)pos;

        if (k >= colors - 1)
            k = colors - 2;

        float frac = pos - (float)k;
        const unsigned char *a = pal + 3 * k, *b = a + 3;

        for (int ch = 0; ch < 3; ch++)
            px[ch] = (unsigned char)(a[ch] + (b[ch] - a[ch]) * frac + 0.5f);

        if (c == 4)
            px[3] = 255;
    }

    free(f);
    replace_pixels(img, data, w, h, c);
    return (ClayPluginValue)self;
}

//------------------------------------------------------------------------
// animation and palettes (anim.c)
//------------------------------------------------------------------------
unsigned char *anim_gif(const unsigned char **frames, const int *chans, int nframes, int w, int h,
                        const int *delays, int loop, int maxcolors, int samplefac, size_t *outLen);
unsigned char *anim_apng(const unsigned char **frames, const int *chans, int nframes, int w, int h,
                         const int *delays, int loop, size_t *outLen);
int anim_quantize(const unsigned char *px, int w, int h, int c, int maxcolors, int samplefac, unsigned char *out);

// An integer argument, or def when it is missing or nil
static int int_arg(int argc, ClayPluginValue *argv, int i, int def) {
    if (i >= argc || api->is_nil(argv[i]))
        return def;

    if (api->is_int(argv[i]))
        return (int)api->to_int(argv[i]);

    if (api->is_float(argv[i]))
        return (int)api->to_float(argv[i]);

    return def;
}

// The frames for _gif and _apng: (width, height, pixels, channels, delays):
// pixels an Array of byte Strings, channels and delays Arrays of integers,
// one each a frame. Fills the arrays (malloc'd; the caller frees them) and
// returns the number of frames, or 0 after raising.
static int gather_frames(int argc, ClayPluginValue *argv, const char *who,
                         int *w, int *h, const unsigned char ***frames, int **chans, int **delays) {
    if (argc < 5 || !api->is_int(argv[0]) || !api->is_int(argv[1]) ||
        !api->is_array(argv[2]) || !api->is_array(argv[3]) || !api->is_array(argv[4])) {
        api->raise(who, "ArgumentError");
        return 0;
    }

    intptr_t iw = api->to_int(argv[0]), ih = api->to_int(argv[1]);

    if (iw < 1 || ih < 1 || iw > 65535 || ih > 65535 || (size_t)iw * (size_t)ih > ((size_t)1 << 28)) {
        api->raise("Image: animation frames must be 1 to 65535 pixels wide and high", "ArgumentError");
        return 0;
    }

    size_t n = api->array_size(argv[2]);

    if (n < 1 || n > 100000 || api->array_size(argv[3]) != n || api->array_size(argv[4]) != n) {
        api->raise("Image: an animation needs 1 or more frames, with channels and a delay for each", "ArgumentError");
        return 0;
    }

    *w = (int)iw;
    *h = (int)ih;
    *frames = (const unsigned char **)malloc(n * sizeof(unsigned char *));
    *chans = (int *)malloc(n * sizeof(int));
    *delays = (int *)malloc(n * sizeof(int));

    if (!*frames || !*chans || !*delays) {
        free((void *)*frames);
        free(*chans);
        free(*delays);
        api->raise("Image: out of memory", "RuntimeError");
        return 0;
    }

    for (size_t i = 0; i < n; i++) {
        ClayPluginValue c = api->array_get(argv[3], i);
        ClayPluginValue d = api->array_get(argv[4], i);
        size_t len = 0;
        const char *px = api->to_bytes(api->array_get(argv[2], i), &len);
        int ch = api->is_int(c) ? (int)api->to_int(c) : 0;

        if (!px || ch < 1 || ch > 4 || len != (size_t)iw * (size_t)ih * (size_t)ch) {
            free((void *)*frames);
            free(*chans);
            free(*delays);
            api->raise("Image: every animation frame must be the same size as the first", "ArgumentError");
            return 0;
        }

        (*frames)[i] = (const unsigned char *)px;
        (*chans)[i] = ch;
        (*delays)[i] = api->is_int(d) ? (int)api->to_int(d) : (api->is_float(d) ? (int)api->to_float(d) : 100);
    }

    return (int)n;
}

//------------------------------------------------------------------------
// Image _gif(width, height, pixels, channels, delays, loop, colors, sample)
//
// -> an animated GIF file as a String of bytes (behind Image gifBytes)
//------------------------------------------------------------------------
static ClayPluginValue image_gif(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(self);

    if (api->api_version < 4) {
        api->raise("Image: gifBytes needs Clay plugin API 4", "RuntimeError");
        return 0;
    }

    int w, h;
    const unsigned char **frames;
    int *chans, *delays;
    int n = gather_frames(argc, argv, "Image: _gif expects (width, height, pixels, channels, delays, loop, colors, sample)",
                          &w, &h, &frames, &chans, &delays);

    if (n == 0)
        return 0;

    size_t len = 0;
    unsigned char *gif = anim_gif(frames, chans, n, w, h, delays, int_arg(argc, argv, 5, 0),
                                  int_arg(argc, argv, 6, 256), int_arg(argc, argv, 7, 10), &len);
    free((void *)frames);
    free(chans);
    free(delays);

    if (!gif) {
        api->raise("Image: couldn't encode the GIF (out of memory)", "RuntimeError");
        return 0;
    }

    ClayPluginValue bytes = api->make_bytes(gif, len);
    free(gif);
    return bytes;
}

//------------------------------------------------------------------------
// Image _apng(width, height, pixels, channels, delays, loop)
//
// -> an animated PNG file as a String of bytes (behind Image apngBytes)
//------------------------------------------------------------------------
static ClayPluginValue image_apng(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(self);

    if (api->api_version < 4) {
        api->raise("Image: apngBytes needs Clay plugin API 4", "RuntimeError");
        return 0;
    }

    int w, h;
    const unsigned char **frames;
    int *chans, *delays;
    int n = gather_frames(argc, argv, "Image: _apng expects (width, height, pixels, channels, delays, loop)",
                          &w, &h, &frames, &chans, &delays);

    if (n == 0)
        return 0;

    size_t len = 0;
    unsigned char *png = anim_apng(frames, chans, n, w, h, delays, int_arg(argc, argv, 5, 0), &len);
    free((void *)frames);
    free(chans);
    free(delays);

    if (!png) {
        api->raise("Image: couldn't encode the APNG (out of memory)", "RuntimeError");
        return 0;
    }

    ClayPluginValue bytes = api->make_bytes(png, len);
    free(png);
    return bytes;
}

//------------------------------------------------------------------------
// image _setQuantized(width, height, channels, bytes, colors, sample)
//
// -> the image (behind img quantize): those pixels in at most colors
// colours, chosen by NeuQuant (or exactly, when there are few enough)
//------------------------------------------------------------------------
static ClayPluginValue image_set_quantized(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    if (api->api_version < 4) {
        api->raise("Image: quantize needs Clay plugin API 4", "RuntimeError");
        return 0;
    }

    Image *img = image_of(self);

    if (!img)
        return 0;

    if (argc < 4 || !api->is_int(argv[0]) || !api->is_int(argv[1]) || !api->is_int(argv[2])) {
        api->raise("Image: _setQuantized expects (width, height, channels, bytes, colors, sample)", "ArgumentError");
        return 0;
    }

    int w = (int)api->to_int(argv[0]), h = (int)api->to_int(argv[1]), c = (int)api->to_int(argv[2]);
    size_t len = 0;
    const char *px = api->to_bytes(argv[3], &len);

    if (!px || w < 1 || h < 1 || c < 1 || c > 4 || len != (size_t)w * (size_t)h * (size_t)c) {
        api->raise("Image: quantize needs the image's own pixels", "ArgumentError");
        return 0;
    }

    unsigned char *data = (unsigned char *)STBI_MALLOC(len);

    if (!data || !anim_quantize((const unsigned char *)px, w, h, c, int_arg(argc, argv, 4, 256), int_arg(argc, argv, 5, 10), data)) {
        STBI_FREE(data);
        api->raise("Image: out of memory", "RuntimeError");
        return 0;
    }

    replace_pixels(img, data, w, h, c);
    return (ClayPluginValue)self;
}

//------------------------------------------------------------------------
// the list of methods for the Image prototype
//------------------------------------------------------------------------
static ClayPluginMethod methods[] = {
    { "_create", image_create },
    { "_read", image_read },
    { "savePng", image_save_png },
    { "width",  image_width },
    { "height", image_height },
    { "_probe", image_probe },
    { "channels", image_channels },
    { "pixels", image_pixels },
    { "_setPixels", image_set_pixels },
    { "floats", image_floats },
    { "_setFloats", image_set_floats },
    { "_setMapped", image_set_mapped },
    { "pngBytes", image_png_bytes },
    { "_gif", image_gif },
    { "_apng", image_apng },
    { "_setQuantized", image_set_quantized },
    { NULL, NULL }
};

//------------------------------------------------------------------------
// Register our Image type with the Clay plugin API
//------------------------------------------------------------------------
CLAY_PLUGIN_EXPORT void image_plugin_init(const ClayPluginAPI *clay_api) {
    // save the Clay plugin API pointer for later use
    api = clay_api;

    // check if the API version is compatible
    if (api->api_version < MIN_API_VERSION) {
        api->raise("Image: incompatible API version", "RuntimeError");
        return;
    }

    // define the ImageCore type with its methods, free function, and size
    ClayPluginType def = { "ImageCore", methods, image_free, sizeof(void *) };

    // register the ImageCore type with the Clay plugin API
    api->register_type(&def);

    // and CanvasCore, for drawing (canvas.c)
    canvas_register(api);
}
