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
