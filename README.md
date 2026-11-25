# st

<img align="right" width="192px" src="./st.svg" alt="st Logo">

<a href="./LICENSE"><img src="https://img.shields.io/badge/license-MIT-green" alt="License"></a>
<a href="https://www.buymeacoffee.com/gabrielzschmitz" target="_blank"><img src="https://www.buymeacoffee.com/assets/img/custom_images/orange_img.png" alt="Buy Me A Coffee" style="height: 20px !important;width: 87px;" ></a>
<a href="https://github.com/gabrielzschmitz/st"><img src="https://img.shields.io/github/stars/gabrielzschmitz/st?style=social" alt="Give me a Star"></a>

st (simple terminal) is a lightweight and minimalist terminal emulator
developed by the suckless community. This package provides a custom build
maintained by [gabrielzschmitz](https://github.com/gabrielzschmitz), with
modifications and configuration tailored to personal preferences.

---

## Features

- Minimal and lightweight terminal emulator
- Fast startup and low memory footprint
- Scrollback and mouse support
- True color and 24-bit color support
- Custom cursor behavior and colors
- Clipboard integration and drag-and-drop support
- Xresources support for dynamic configuration
- Nix flake, Nix package, and development shell included

---

## Build & Install

<details open>
<summary><b>With Make</b></summary>

```sh
cd src/
make
sudo make install
```

</details>

<details>
<summary><b>With Nix flakes</b></summary>

Build the package and enter the development shell:

```sh
nix build
nix develop
```

**Install on NixOS**

Add the flake input:

```nix
inputs.st.url = "github:gabrielzschmitz/st";
```

Add the package to your NixOS configuration:

```nix
environment.systemPackages = [
  inputs.st.packages.${pkgs.system}.default
];
```

Rebuild:

```sh
sudo nixos-rebuild switch --flake .#nixos
```

</details>

## Renderer selection

`ST_RENDERER` selects the drawing backend. The default is `xft`. Build with
`ST_WITH_GL=1` to enable the `gl` value. GL mode creates a GLX context, clears
the window, and swaps buffers. Text primitives still draw to an Xft offscreen
buffer; GL does not present them yet. An unknown value prints an error and
exits before the window opens.

```sh
ST_RENDERER=xft st
make -C src ST_WITH_GL=1
ST_RENDERER=gl src/st
```

Set `ST_VSYNC=1` to request vsync from the GL renderer. st uses
`GLX_EXT_swap_control` or `GLX_MESA_swap_control`. If neither is available, st
logs once and continues unsynced.

## Renderer benchmarks

Build `st`, then run repeatable terminal-output benchmarks:

```sh
make -C src benchmark
```

The benchmark uses current `DISPLAY`. Run on an isolated virtual X server with
`make -C src benchmark-xvfb`. Xvfb measures software rendering; use a real
display to compare GPU-backed renderers.

Workloads cover sustained output, repeated full-screen redraws, scrollback, and
Kitty image upload/display. Each trial records output-generation throughput,
child CPU time, terminal-process lifetime, and frame trace percentiles:
`draw_ms` is renderer work from draw start to frame flush, and `submit_ms` is
trigger-to-flush latency. These are client-side measurements; they do not prove
that the X server or compositor presented a frame.

With `--capture`, the benchmark draws a colored test pattern, captures the
window through the small `benchmarks/xcapture` X11 helper, and fails trials whose
image looks blank. Xvfb is useful for repeatability and correctness. Compare GPU
performance on a real display. Results go to `benchmark-results.json`. See
`python3 benchmarks/benchmark.py --help` for options.

### Recording a baseline on the target machine

Use a real display for GPU comparisons. Xvfb only measures software rendering.

1. Pin the display refresh rate. Disable adaptive sync and any automatic
   refresh switching for the duration of the run.
2. Disable compositor effects that add latency, such as animations, blur, and
   unredirect heuristics. Record whether the compositor was active.
3. Keep resolution, font, font size, terminal geometry, and `st` build fixed.
   Rebuild from the commit being measured.
4. Run each workload several times with the repeatability check:

   ```sh
   python3 benchmarks/benchmark.py --repeat-check 5 --tolerance 10 \
     --workloads stream redraw scroll --output baseline.json
   ```

   The check reports relative spread `(max-min)/median` for generation
   throughput and `draw_ms_p95`, and exits non-zero when the spread exceeds the
   tolerance.
5. Record the `system`, `machine`, and `display` fields from the JSON with the
   results. They identify the kernel, platform, and X display used for the run.
6. Repeat the whole procedure in a separate session. The exit criterion is two
   independent runs that agree within the chosen tolerance.

Frame tracing reports `draw_ms`, `submit_ms`, and `present_ms`. `present_ms`
covers a benchmark-only `XSync` round trip, which shows when the server has
processed the flush. It still does not prove that a compositor presented the
frame, so compare medians under identical conditions.

<details>
<summary><b>With Nix (`default.nix`)</b></summary>

Build the package:

```sh
nix-build -A st
```

**Install on NixOS**

```nix
environment.systemPackages = [
  (pkgs.callPackage /path/to/st/default.nix {})
];
```

Replace `/path/to/st` with the path to your local clone, then rebuild:

```sh
sudo nixos-rebuild switch
```

</details>

---

## Patches

This build includes the following patches:

- [anygeometry](https://st.suckless.org/patches/anygeometry/)
- [blinking cursor](https://st.suckless.org/patches/blinking_cursor/)
- [bold is not bright](https://st.suckless.org/patches/bold-is-not-bright/)
- [clipboard](https://st.suckless.org/patches/clipboard/)
- [desktop entry](https://st.suckless.org/patches/desktopentry/)
- [drag and drop](https://st.suckless.org/patches/drag-n-drop/)
- [dynamic cursor color](https://st.suckless.org/patches/dynamic-cursor-color/)
- [scrollback and mouse support for alternate screens](https://st.suckless.org/patches/scrollback/)
- [swap mouse buttons](https://st.suckless.org/patches/swapmouse/)
- [unfocused cursor](https://st.suckless.org/patches/unfocused_cursor/)
- [Xresources signal reloading](https://st.suckless.org/patches/xresources-with-reload-signal/)

All patch files are available in [`src/patches/`](./src/patches/).

---

## Configuration

The terminal can be customized through `src/config.def.h` and `src/config.mk`.

After changing the configuration, rebuild the terminal:

```sh
cd src/
make clean
make
```

### Xresources (`xrdb`)

The Xresources patch allows supported terminal settings to be configured
through Xresources instead of hardcoding them in `config.def.h`.

Add your preferred settings to `~/.Xresources`, for example:

```xresources
! Terminal options
st.font:        SFMono Nerd Font:antialias=true:autohint=true:pixelsize=20
st.borderpx:    0
st.alpha:       1.0

! Terminal colors
*background: #14171c
*foreground: #abb2bf

! Black + DarkGrey
*color0:  #181b21
*color8:  #5c6370

! DarkRed + Red
*color1:  #be5046
*color9:  #e06c75

! DarkGreen + Green
*color2:  #81ae5f
*color10: #98c379

! DarkYellow + Yellow
*color3:  #d19a66
*color11: #e5c07b

! DarkBlue + Blue
*color4:  #4889be
*color12: #61afef

! DarkMagenta + Magenta
*color5:  #7560d3
*color13: #8677cf

! DarkCyan + Cyan
*color6:  #49919a
*color14: #56b6c2

! LightGrey + White
*color7:  #abb2bf
*color15: #b8bfe5
```

Load the settings with `xrdb`:

```sh
xrdb -merge ~/.Xresources
```

---

## License

This project is licensed under the MIT License. See the [LICENSE](./LICENSE)
file for details.
