# clay-image

Image reading and writing for [Clay](https://github.com/mseminatore/clay), built on
[stb_image](https://github.com/nothings/stb). It loads PNG, JPEG, BMP, GIF, TGA, PSD, HDR, PIC
and PNM files, saves PNG, and exposes pixels as bytes or as numbers in 0..1. That makes it easy
to feed images to a neural network, or to turn data such as a mel spectrogram into an image.

This is a **native brick**: its C sources are compiled the first time you import it,
then loaded into Clay, or linked into executables built with `clay --compile`.

```clay
import("image")

img = Image load("photo.png")
img println                       // Image(640 x 480, 3 channels)
img savePng("copy.png")

Image info("photo.png")           // {width: 640, height: 480, channels: 3}, without decoding
```

## API

### Making images

| | |
|---|---|
| `Image load(path)` | A new image loaded from a file. Raises `RuntimeError` if it can't be read. |
| `Image fromBytes(w, h, c, bytes)` | A new image from a String of `w * h * c` pixel bytes. |
| `Image fromFloats(w, h, c, numbers)` | A new image from an Array of `w * h * c` numbers in 0..1, flat or as rows. Values outside 0..1 are clamped. |
| `Image colormap(w, h, numbers, palette, lo, hi)` | A new RGB image from `w * h` numbers (flat or as rows), each coloured through `palette`: a String of r, g, b bytes for 2 or more colours, `lo` taking the first and `hi` the last, with neighbouring colours blended in between. Values outside `lo..hi` take the end colours. `lo` and `hi` default to the smallest and largest number. A NaN leaves its pixel transparent (and the image is then RGBA). This is how a heatmap, a weight matrix or a spectrogram becomes an image. |

### Reading a file's size

| | |
|---|---|
| `Image info(path)` | A Dict `#{width:, height:, channels:}` read from the file's header, without decoding the pixels, so it's cheap enough to scan a whole dataset. `nil` if the file is missing or isn't an image stb can read. |

### Image methods

| | |
|---|---|
| `savePng(path)` | Saves the image as a PNG. |
| `pngBytes` | The PNG file `savePng` would write, as a String of bytes: to embed in an SVG or HTML page, or send over HTTP. |
| `width`, `height` | The size in pixels. |
| `channels` | 1 gray, 2 gray + alpha, 3 RGB, 4 RGBA. |
| `pixels` | The pixels as a String of `width * height * channels` bytes. |
| `floats` | The pixels as a flat Array of numbers in 0..1 (each byte / 255). |
| `toString` | `"Image(640 x 480, 3 channels)"`, so `println` and `+` show an image's size. |

Images don't change once made: to work on another file, make another image
(`img = Image load("b.png")`). The old one's pixels are freed when nothing refers to it.
Constructors are sent to `Image` itself; sending one to an image (`img load(path)`) raises,
since it would leave `img` as it was.

Pixel data is laid out row by row, top to bottom, with channels interleaved (`RGBARGBA...`),
in both bytes and floats. The brick needs Clay 0.3.4 or later.

Every image is an `Image`, so methods you add to `Image` reach all of them:

```clay
Image setMethod("area", { self width * self height })
(Image load("photo.png")) area println     // 307200
```

### Examples

Neural network input, one number per channel value:

```clay
x = (Image load("digit.png")) floats     // e.g. 28 * 28 = 784 numbers for a gray image
```

A grayscale image from data, such as a mel spectrogram scaled to 0..1 (one row per mel band):

```clay
Image fromFloats(frames, bands, 1, rows) savePng("spectrogram.png")
```

The same in colour, from the raw values (no scaling needed), through a palette that runs from
dark blue to yellow:

```clay
palette = String fromBytes([68, 1, 84, 33, 145, 140, 253, 231, 37])
Image colormap(frames, bands, rows, palette) savePng("spectrogram.png")
```

### Notes

- PNG is the only output format. stb's encoder favours simplicity over compression, so a
  re-saved PNG is often larger than the original, with identical pixels. Color-space and DPI
  chunks (`sRGB`, `gAMA`, `pHYs`) are not kept.
- `floats` makes one Clay number per channel value (a 128 x 128 RGBA image is 65,536), so
  mind memory with large images.
- Don't `clone` an image: the clone has no pixel storage of its own, and its methods raise
  "not an image". Make images with the constructors above, and copy one with
  `Image fromBytes(img width, img height, img channels, img pixels)`.
- `Image` is a clone of `ImageCore`, the prototype the C plugin registers, so `methods` also
  lists the C layer's internal methods, named with a leading `_` (`_create`, `_read`,
  `_setPixels`, `_setFloats`, `_probe`). They are not part of the API.

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

## Third-party code

`native/stb/` holds `stb_image.h` and `stb_image_write.h` from
[nothings/stb](https://github.com/nothings/stb), public domain or MIT (see
`native/stb/LICENSE`). `native/stb/VERSION` records the commit. To upgrade, download the
two headers and `LICENSE` from a newer commit and update `VERSION`.

## Testing

```sh
clay tests/test_image.clay       # builds the plugin on first use; a brick imports itself by name
```

The plugin is cached in `build/` (not committed); delete it to force a rebuild. To
debug, add `"-g", "-O0"` to `cflags` in `brick.json` and run under `lldb` or `gdb`:
`lldb -- clay tests/test_image.clay`. Use `api->write_output` instead of `printf`
for text people should see.
