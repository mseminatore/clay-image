# Canvas font

`Canvas` draws text with DejaVu Sans 2.37 (the font matplotlib uses by default), from
<https://github.com/dejavu-fonts/dejavu-fonts/releases/tag/version_2_37>, under its licence in
`LICENSE` (Bitstream Vera licence; DejaVu's changes are public domain).

It is subset to what charts need, so the plugin stays small (26 KB instead of 757 KB):
Basic Latin, Latin-1, Greek letters, en and em dashes, curly quotes, bullet, ellipsis, the minus
sign, ≤ ≥ × ∞, with kerning and without hinting:

```sh
pyftsubset DejaVuSans.ttf \
  --unicodes="U+0020-007E,U+00A0-00FF,U+0391-03A9,U+03B1-03C9,U+2013,U+2014,U+2018-201D,U+2022,U+2026,U+2212,U+2264,U+2265,U+00D7,U+221E" \
  --layout-features=kern --no-hinting --desubroutinize --output-file=DejaVuSans-subset.ttf
```

(`pyftsubset` is part of [fontTools](https://github.com/fonttools/fonttools).) The plugin
compiles the font in, from `dejavu_sans.h`, so compiled programs need no font file. After
changing the font, regenerate the header:

```sh
{ echo "// DejaVu Sans 2.37, subset for clay-image's Canvas (see font/README.md)."
  echo "// Generated with: xxd -i DejaVuSans-subset.ttf; do not edit."
  xxd -i -n dejavu_sans_ttf DejaVuSans-subset.ttf |
    sed 's/^unsigned char/static const unsigned char/; s/^unsigned int/static const unsigned int/'
} > dejavu_sans.h
```

Characters the font lacks are drawn as its empty box.
