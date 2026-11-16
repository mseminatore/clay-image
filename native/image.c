/* clay_image.c -- the native part of the clay-image brick.
 *
 * It registers one prototype, ClayImageCore, whose objects each own a C struct.
 * src/main.clay wraps it in a Clay API. The C API is in clay_plugin.h (see
 * "Native plugins" in the Clay guide): values cross as opaque handles,
 * arguments are borrowed, and what a method returns is a new reference (or
 * its own receiver). The entry point must be named <plugin>_plugin_init.
 */
#include "clay_plugin.h"
#include <stdlib.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

static const ClayPluginAPI *api;

/* what each counter owns */
typedef struct {
    intptr_t total;
} Counter;

/* runs when the Clay object is freed */
static void counter_free(void *data) { free(data); }

static Counter *counter_of(ClayPluginObject self) {
    Counter *c = (Counter *)api->get_data(self);
    if (!c) api->raise("ClayImageCore: not a counter (make one with ClayImage counter)", "RuntimeError");
    return c;
}

/* ClayImageCore create(start) -> a new counter; self is the prototype */
static ClayPluginValue core_create(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    Counter *c = (Counter *)calloc(1, sizeof(Counter));
    if (!c) { api->raise("clay-image: out of memory", "RuntimeError"); return 0; }
    if (argc > 0) {
        if (!api->is_int(argv[0])) {
            free(c);
            api->raise("create expects an integer", "ArgumentError");
            return 0;
        }
        c->total = api->to_int(argv[0]);
    }
    ClayPluginObject obj = api->clone(self);
    api->set_data(obj, c);
    return (ClayPluginValue)obj;
}

/* counter add(n) -> the counter, so calls chain */
static ClayPluginValue core_add(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    Counter *c = counter_of(self);
    if (!c) return 0;
    if (argc < 1 || !api->is_int(argv[0])) {
        api->raise("add expects an integer", "ArgumentError");
        return 0;
    }
    c->total += api->to_int(argv[0]);
    return (ClayPluginValue)self;
}

/* counter value -> the total */
static ClayPluginValue core_value(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)argc; (void)argv;
    Counter *c = counter_of(self);
    if (!c) return 0;
    return api->make_int(c->total);
}

static ClayPluginMethod methods[] = {
    { "create", core_create },
    { "add",    core_add },
    { "value",  core_value },
    { NULL, NULL }
};

CLAY_PLUGIN_EXPORT void clay_image_plugin_init(const ClayPluginAPI *clay_api) {
    api = clay_api;
    ClayPluginType def = { "ClayImageCore", methods, counter_free, sizeof(void *) };
    api->register_type(&def);
}
