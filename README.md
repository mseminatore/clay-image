# clay-image

Image for [Clay](https://github.com/mseminatore/clay.git). This library provides basic
image read/write capabilities.

This is a **native brick**: its C sources are compiled the first time you import it, 
then loaded into Clay, or linked into executables build with `clay --compile`.

```clay
import("image")

image = Image clone
image load(path)        // Loads the image at the given path
image savePng(path)     // Saves the given image to the specified path
image info(path)        // Prints information about the image at the given path
image width             // Returns the width of the image
image height            // Returns the height of the image
```

## Install

```sh
clay bricks add github.com/mseminatore/clay-image image
```

`clay bricks add` tells people that a brick runs C code and asks before going ahead.

## Develop

`.github/workflows/test.yml` runs the tests on Linux, macOS and Windows for every push and
pull request on GitHub, which is where a native brick's platform differences show up. It
builds Clay from source; pin `CLAY_REF` to a release once you depend on one. On another Git
host, delete it.

Per-platform flags go in `cflags-macos`, `libs-linux`, `sources-windows` and so on;
`-I@/dir` names a directory inside the brick. See "Native bricks" in the Clay guide.

## Testing

```sh
clay tests/test_image.clay       # builds the plugin on first use; a brick imports itself by name
```

The plugin is cached in `build/` (not committed); delete it to force a rebuild. To
debug, add `"-g", "-O0"` to `cflags` in `brick.json` and run under `lldb` or `gdb`:
`lldb -- clay tests/test_image.clay`. Use `api->write_output` instead of `printf`
for text people should see.
