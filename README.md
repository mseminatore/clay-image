# clay-image

Image reading and writing for [Clay](https://github.com/mseminatore/clay), built on
[stb_image](https://github.com/nothings/stb). It loads PNG, JPEG, BMP, GIF, TGA, PSD, HDR, PIC
and PNM files, saves PNG and animated GIF and PNG, exposes pixels as bytes or as numbers in
0..1, and draws on a Canvas (anti-aliased shapes, lines and text). That makes it easy
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
| `quantize(colors, opts)` | A copy in at most `colors` colours (2 to 256), chosen as GIFs choose them (below). `quality`: 1 (best) to 30 (fastest), 10. |
| `gifBytes`, `saveGif(path)` | The image as a one-frame GIF. |
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

### Animation

| | |
|---|---|
| `Image saveGif(path, frames, opts)` | Saves an animated GIF of `frames`: Images (or Canvases) all the same size. Returns `path`. |
| `Image gifBytes(frames, opts)` | The GIF file as a String of bytes. |
| `Image saveApng(path, frames, opts)` | Saves an animated PNG: full colour, shown animated by browsers (other viewers show the first frame). |
| `Image apngBytes(frames, opts)` | The APNG file's bytes. |

Options: `delay` (milliseconds a frame, 100, or an Array with one per frame), `loop` (0, the
default, plays for ever; 1 once; n n times), and for GIFs `colors` (up to 256) and
`quality` (1 to 30, 10).

```clay
frames = (0..19) map { |i|
    c = Canvas new(200, 120)
    c fillCircle(10 + (i * 9), 60, 10, "#d62728")
    c toImage
}
Image saveGif("ball.gif", frames, #{delay: 50})
Image saveApng("ball.png", frames, #{delay: 50})
```

A GIF has at most 256 colours, shared here by all the frames so nothing flickers. When the
frames use 256 colours or fewer (most charts), they are kept exactly. Otherwise the palette
is learnt by [NeuQuant](https://scientificgems.wordpress.com/stuff/neuquant-fast-high-quality-image-quantization/)
(Anthony Dekker, 1994), a small self-organising map trained on a sample of the pixels, and
then the most used colours (backgrounds, text, lines) are set exactly, so flat areas stay
pure. There is no dithering, which keeps text and lines crisp. Each frame after the first
stores only the rectangle that changed, with unchanged pixels transparent, so an animation
whose frames differ a little stays small. Pixels with alpha below 128 are transparent.

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
  `_setPixels`, `_setFloats`, `_setMapped`, `_probe`). They are not part of the API. In the
  same way `Canvas` is a clone of `CanvasCore`.

## Drawing: Canvas

A `Canvas` is pixels you draw on, anti-aliased: shapes, lines and text. It is what
[clay-plot](https://github.com/mseminatore/clay-plot) draws PNG charts with.

```clay
c = Canvas new(400, 300)                       // white; Canvas new(w, h, color) for another
c fillRect(10, 10, 100, 50, "#1f77b4")
c strokePolyline([[10, 200], [100, 120], [200, 180]], [214, 39, 40], 2, #{join: "round"})
c fillCircle(300, 80, 30, [44, 160, 44, 0.5])  // half transparent
c text("Hello, Clay", 200, 280, 24, "#000", #{anchor: "middle"})
c savePng("drawing.png")
```

Coordinates are pixels from the top left and can be fractions. A colour is `"#rrggbb"`,
`"#rgb"`, `[r, g, b]` or `[r, g, b, alpha]` (r, g, b in 0..255, alpha in 0..1). Points are
`[[x, y], ...]` or flat `[x0, y0, x1, y1, ...]`. Drawing methods return the canvas.

| | |
|---|---|
| `Canvas new(w, h, background)` | A new canvas (up to 16384 x 16384), filled with `background` (white). `[0, 0, 0, 0]` makes it transparent. |
| `fillRect(x, y, w, h, color)`, `strokeRect(x, y, w, h, color, width)` | Rectangles |
| `fillPolygon(points, color, opts)` | A filled polygon. `opts`: `evenOdd: true` leaves holes where it overlaps itself. |
| `fillPaths(paths, color, opts)` | Several polygons in one fill: overlaps are drawn once, and with `evenOdd: true` inner ones cut holes. |
| `fillCircle(cx, cy, r, color)`, `strokeCircle(cx, cy, r, color, width)` | Circles |
| `fillCircles(centres, radius, color)` | Many circles at once (one radius, or one per centre): fast for scatter plots |
| `line(x1, y1, x2, y2, color, width, opts)` | A line |
| `strokePolyline(points, color, width, opts)` | A line through points. `opts`: `cap` (`"butt"`, `"round"`, `"square"`), `join` (`"round"`, `"miter"`, `"bevel"`), `dash` (an Array of on and off lengths), `closed`. A semi-transparent line is drawn once where it crosses itself or turns. |
| `strokePaths(paths, color, width, opts)` | Several lines in one stroke, with the same options |
| `text(s, x, y, size, color, opts)` | Text with its baseline at `y`, `size` pixels to the em. `opts`: `anchor` (`"start"`, `"middle"`, `"end"`: where `x` is along the text), `angle` (degrees, counter-clockwise). |
| `textWidth(s, size)` | The text's width in pixels (also on `Canvas` itself, with no canvas) |
| `drawImage(img, x, y, w, h, opts)` | An `Image` stretched over the rectangle (its own size if `w`, `h` are left out), nearest pixel. `opts`: `alpha`, `flipY`. |
| `clip(x, y, w, h)`, `unclip` | Draw only inside the rectangle; draw anywhere again |
| `width`, `height`, `rgba` | The size, and the pixels as a String of `width * height * 4` bytes (RGBA) |
| `toImage`, `savePng(path)`, `pngBytes` | The drawing as an RGBA `Image`, a PNG file or a PNG's bytes |

Text is drawn with DejaVu Sans (the font matplotlib uses), built into the plugin, so compiled
programs need no font file. It covers Latin-1, Greek, dashes, curly quotes, the minus sign
and a few symbols (≤ ≥ × ∞); other characters show as a box.

Each fill or stroke is one anti-aliased pass (8 samples down each pixel, exact coverage across),
so the pieces of a shape never blend twice where they overlap. Shapes from separate calls do,
as on paper.

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

`native/stb/` holds `stb_image.h`, `stb_image_write.h` and `stb_truetype.h` from
[nothings/stb](https://github.com/nothings/stb), public domain or MIT (see
`native/stb/LICENSE`). `native/stb/VERSION` records the commit. To upgrade, download the
three headers and `LICENSE` from a newer commit and update `VERSION`.

`native/anim.c` (GIF, APNG and palettes) is this brick's own code. Its colour quantizer
implements the NeuQuant algorithm described in A. Dekker, "Kohonen neural networks for
optimal colour quantization", *Network: Computation in Neural Systems* 5 (1994).

`native/font/` holds the Canvas font, a subset of DejaVu Sans 2.37 under its own licence
(`native/font/LICENSE`); `native/font/README.md` says how it was made and how to remake
`dejavu_sans.h`, the C array the plugin compiles in.

## Testing

```sh
clay tests/test_image.clay       # builds the plugin on first use; a brick imports itself by name
clay tests/test_canvas.clay
clay tests/test_anim.clay
```

The plugin is cached in `build/` (not committed); delete it to force a rebuild. To
debug, add `"-g", "-O0"` to `cflags` in `brick.json` and run under `lldb` or `gdb`:
`lldb -- clay tests/test_image.clay`. Use `api->write_output` instead of `printf`
for text people should see.
