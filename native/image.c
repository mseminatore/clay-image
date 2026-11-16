/* clay_image.c -- the Clay plugin for stb
 *
 * It registers one prototype, ImageCore, whose objects each own a C struct.
 * src/main.clay wraps it in a Clay API. The C API is in clay_plugin.h (see
 * "Native plugins" in the Clay guide): values cross as opaque handles,
 * arguments are borrowed, and what a method returns is a new reference (or
 * its own receiver). The entry point must be named <plugin>_plugin_init.
 */
#include "clay_plugin.h"
#include <stdio.h>
#include <stdlib.h>

#define STB_IMAGE_IMPLEMENTATION
#   include "stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#   include "stb/stb_image_write.h"

// the Clay plugin API instance
static const ClayPluginAPI *api;

// minimum API version required by this plugin
#define MIN_API_VERSION 2

// what each image owns
typedef struct {
    unsigned char *data;    // pixel data
    int width;
    int height;
    int channels;
} Image;

//------------------------------------------------------------------------
// Image free callback
// runs when the Clay object is freed
// and releases the image's data
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
// Image of callback
// retrieves the C struct from the Clay object
//------------------------------------------------------------------------
static Image *image_of(ClayPluginObject self) {
    Image *img = (Image *)api->get_data(self);

    if (!img) 
        api->raise("Image: not an image", "RuntimeError");

    return img;
}

//------------------------------------------------------------------------
// ImageCore create -> a new, empty image; self is the prototype
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
// Image load(path) -> the image, so calls chain
//------------------------------------------------------------------------
static ClayPluginValue image_load(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    Image *img = image_of(self);
    
    if (!img) 
        return 0;
    
    if (argc < 1 || !api->is_string(argv[0])) {
        api->raise("Image: load expects a string", "ArgumentError");
        return 0;
    }

    // load the image and set its data, width, height and channels
    unsigned char *data = stbi_load(api->to_string(argv[0]), &img->width, &img->height, &img->channels, 0);
    if (!data) {
        api->raise("Image: Failed to load image", "RuntimeError");
        return 0;
    }
    
    // release any image loaded before
    if (img->data)
        stbi_image_free(img->data);

    img->data = data;

    return (ClayPluginValue)self;
}

//------------------------------------------------------------------------
// Image savePng -> saves the image as a PNG file
//------------------------------------------------------------------------
static ClayPluginValue image_save_png(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    Image *img = image_of(self);

    if (!img) 
        return 0;

    if (argc < 1 || !api->is_string(argv[0])) {
        api->raise("Image: savePng expects a string", "ArgumentError");
        return 0;
    }

    if (!img->data) {
        api->raise("Image: no image loaded", "RuntimeError");
        return 0;
    }

    if (!stbi_write_png(api->to_string(argv[0]), img->width, img->height, img->channels, img->data, img->width * img->channels)) {
        api->raise("Image: Failed to save image", "RuntimeError");
        return 0;
    }

    return (ClayPluginValue)self;
}

//------------------------------------------------------------------------
// Image info -> prints information about the image
//------------------------------------------------------------------------
static ClayPluginValue image_info(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)argc; (void)argv;

    Image *img = image_of(self);

    if (!img)
        return 0;

    if (!img->data) {
        api->raise("Image: no image loaded", "RuntimeError");
        return 0;
    }

    char line[96];

    snprintf(line, sizeof line, "Image: %d x %d, %d channels\n", img->width, img->height, img->channels);

    // write_output reaches the Debug Console under --dap, where stdout does not
    if (api->api_version >= 3)
        api->write_output(line);
    else
        fputs(line, stdout);

    return (ClayPluginValue)self;
}

//------------------------------------------------------------------------
// Image width -> the image's width
//------------------------------------------------------------------------
static ClayPluginValue image_width(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)argc; (void)argv;

    Image *img = image_of(self);

    if (!img) 
        return 0;

    return api->make_int(img->width);
}

//------------------------------------------------------------------------
// Image height -> the image's height
//------------------------------------------------------------------------
static ClayPluginValue image_height(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)argc; (void)argv;

    Image *img = image_of(self);

    if (!img) 
        return 0;

    return api->make_int(img->height);
}

//------------------------------------------------------------------------
// the list of methods for the Image prototype
//------------------------------------------------------------------------
static ClayPluginMethod methods[] = {
    { "create", image_create },
    { "load", image_load },
    { "savePng", image_save_png },
    { "width",  image_width },
    { "height", image_height },
    { "info", image_info },
    { NULL, NULL }
};

//------------------------------------------------------------------------
// Register our Image type with the Clay plugin API
//------------------------------------------------------------------------
CLAY_PLUGIN_EXPORT void image_plugin_init(const ClayPluginAPI *clay_api) {
    api = clay_api;

    if (api->api_version < 2) {
        api->raise("Image: incompatible API version", "RuntimeError");
        return;
    }

    ClayPluginType def = { "ImageCore", methods, image_free, sizeof(void *) };

    api->register_type(&def);
}
