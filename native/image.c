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
    unsigned char *data = stbi_load(api->to_string(argv[0]), &w, &h, &c, 0);

    // check if the image was loaded successfully
    // TODO: provide more detailed error information if available
    if (!data) {
        api->raise("Image: Failed to load image", "RuntimeError");
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
// Image info
//
// Prints information about the image
//------------------------------------------------------------------------
static ClayPluginValue image_info(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)argc; (void)argv;

    // retrieve the image object from the plugin object
    Image *img = image_of(self);

    // ensure the image object is valid
    if (!img)
        return 0;

    // ensure the image has data before printing info
    if (!img->data) {
        api->raise("Image: no image loaded", "RuntimeError");
        return 0;
    }

    char line[SMALL_BUF];

    snprintf(line, sizeof line, "Image: %d x %d, %d channels\n", img->width, img->height, img->channels);

    // write_output reaches the Debug Console under --dap, where stdout does not
    if (api->api_version >= 3)
        api->write_output(line);
    else
        fputs(line, stdout);

    // return self for message chaining
    return (ClayPluginValue)self;
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

//------------------------------------------------------------------------
// shared helpers for reading and replacing pixels
//------------------------------------------------------------------------

// the image's loaded pixels, or raise
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

    Image *img = image_of(self);

    if (!img)
        return 0;

    int w, h, c;
    size_t n = read_shape("fromBytes", argc, argv, &w, &h, &c);

    if (!n)
        return 0;

    size_t len = 0;
    const char *bytes = api->to_bytes(argv[3], &len);

    if (!bytes) {
        api->raise("Image: fromBytes expects a String of bytes", "ArgumentError");
        return 0;
    }

    if (len != n) {
        api->raise("Image: fromBytes data must be width * height * channels bytes", "ArgumentError");
        return 0;
    }

    unsigned char *data = (unsigned char *)STBI_MALLOC(n);

    if (!data) {
        api->raise("Image: out of memory", "RuntimeError");
        return 0;
    }

    memcpy(data, bytes, n);
    replace_pixels(img, data, w, h, c);

    return (ClayPluginValue)self;
}

//------------------------------------------------------------------------
// Image floats
//
// Return the pixels as a flat Array of numbers in 0..1 (byte / 255),
// in the same order as `pixels`: ready to feed a neural network
//------------------------------------------------------------------------
static ClayPluginValue image_floats(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)argc; (void)argv;

    Image *img = loaded_image_of(self);

    if (!img)
        return 0;

    size_t n = (size_t)img->width * img->height * img->channels;
    float *f = (float *)malloc(n * sizeof(float));

    if (!f) {
        api->raise("Image: out of memory", "RuntimeError");
        return 0;
    }

    for (size_t i = 0; i < n; i++)
        f[i] = img->data[i] / 255.0f;

    ClayPluginValue arr = api->from_floats(f, n, 0);

    free(f);

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
    Image *img = image_of(self);

    if (!img)
        return 0;

    int w, h, c;
    size_t n = read_shape("fromFloats", argc, argv, &w, &h, &c);

    if (!n)
        return 0;

    if (!api->is_array(argv[3])) {
        api->raise("Image: fromFloats expects an Array of numbers", "ArgumentError");
        return 0;
    }

    if (api->to_floats(argv[3], NULL, 0) != n) {
        api->raise("Image: fromFloats data must be width * height * channels numbers", "ArgumentError");
        return 0;
    }

    float *f = (float *)malloc(n * sizeof(float));
    unsigned char *data = (unsigned char *)STBI_MALLOC(n);

    if (!f || !data) {
        free(f);
        STBI_FREE(data);
        api->raise("Image: out of memory", "RuntimeError");
        return 0;
    }

    api->to_floats(argv[3], f, n);

    for (size_t i = 0; i < n; i++) {
        float v = f[i];

        // !(v > 0) also catches NaN
        v = !(v > 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v);
        data[i] = (unsigned char)(v * 255.0f + 0.5f);
    }

    free(f);
    replace_pixels(img, data, w, h, c);

    // return the updated image object
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
    { "info", image_info },
    { "channels", image_channels },
    { "pixels", image_pixels },
    { "_setPixels", image_set_pixels },
    { "floats", image_floats },
    { "_setFloats", image_set_floats },
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
}
