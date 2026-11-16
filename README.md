# clay-image

TODO: what clay-image does. A native brick: part of it is C, built with your C compiler
the first time it is imported.

```clay
import("clay-image")

c = ClayImage counter(10)
c add(5)
c value println          // 15
```

## Install

```sh
clay bricks add github.com/YOU/clay-image
```

`clay bricks add` tells people that a brick runs C code and asks before going ahead.

## Layout

| File | What |
|---|---|
| `brick.json` | the manifest; `native` lists the C sources, flags and libraries |
| `native/clay_image.c` | the plugin: registers `ClayImageCore` (entry point `clay_image_plugin_init`) |
| `src/main.clay` | the Clay API that wraps it |
| `tests/test_clay_image.clay` | tests |

`.github/workflows/test.yml` runs the tests on Linux, macOS and Windows for every push and
pull request on GitHub, which is where a native brick's platform differences show up. It
builds Clay from source; pin `CLAY_REF` to a release once you depend on one. On another Git
host, delete it.

Per-platform flags go in `cflags-macos`, `libs-linux`, `sources-windows` and so on;
`-I@/dir` names a directory inside the brick. See "Native bricks" in the Clay guide.

## Develop

```sh
clay tests/test_clay_image.clay       # builds the plugin on first use; a brick imports itself by name
```

The plugin is cached in `build/` (not committed); delete it to force a rebuild. To
debug, add `"-g", "-O0"` to `cflags` in `brick.json` and run under `lldb` or `gdb`:
`lldb -- clay tests/test_clay_image.clay`. Use `api->write_output` instead of `printf`
for text people should see.

## Publish

1. Choose a license and add a `LICENSE` file.
2. Fill in `description` in `brick.json`, commit, and push to GitHub (or another Git host).
3. Tag a release: `git tag v0.1.0 && git push --tags`.
4. Check it on Linux, macOS and Windows (C differs between them), then list it in the
   [clay-registry](https://github.com/mseminatore/clay-registry) with a pull request.
