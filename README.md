# sbar

sbar(simple bar) is a 100% user-configurable status panel for wayland compositors.
You can make it look exactly as you want, only have features that you want
or very easily add support for your compositor / desktop environment.

# Dependencies

* [wayland-client]
* [pixman]
* [fcft]
* [json-c]
* [libpng] (optional)
* [resvg] (optional)

# Installation

```
git clone https://github.com/pd2s/sbar
cd sbar
meson setup build
meson compile -C build
ninja -C build install
```

# Configuration

All configuration is done by writing newline-separated JSON objects to sbar's stdin and reading it's state from stdout.
You can use any programming or scripting language that is able to work with JSON.
See examples/python for more details.
examples/swaybar is almost a drop-in replacement for swaybar. The only features it lacks are Pango-related font description and markup.

[wayland-client]: https://gitlab.freedesktop.org/wayland/wayland
[pixman]: https://gitlab.freedesktop.org/pixman/pixman
[fcft]: https://codeberg.org/dnkl/fcft
[json-c]: https://github.com/json-c/json-c
[libpng]: https://github.com/pnggroup/libpng
[resvg]: https://github.com/linebender/resvg
