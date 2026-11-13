<p align="center"><img src="data/nordend.svg" width="96" height="96" alt=""></p>

<h1 align="center">Nordend</h1>

<p align="center">
  <b>A native file manager for Wayland: fast, light and safe.</b><br>
  No GTK, no Qt, no GLib: C++20 straight on Wayland and OpenGL ES.<br>
  At home on any Wayland desktop, and right at home on <a href="https://github.com/noctalia-dev">Noctalia</a>.
</p>

<p align="center">
  <a href="https://github.com/pankaspe/nordend-fm/releases/tag/v0.6.1"><img src="https://img.shields.io/badge/release-v0.6.1-2a6a9e" alt="release v0.6.1"></a>
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white" alt="C++20">
  <img src="https://img.shields.io/badge/Wayland-native-FFBC00?logo=wayland&logoColor=black" alt="Wayland native">
  <img src="https://img.shields.io/badge/tests-450%20%C2%B7%20ASan%20%C2%B7%20TSan-4B32C3" alt="450 tests, ASan, TSan">
  <a href="https://github.com/pankaspe/nordend-fm/actions/workflows/ci.yml"><img src="https://github.com/pankaspe/nordend-fm/actions/workflows/ci.yml/badge.svg?branch=main" alt="CI"></a>
  <img src="https://img.shields.io/badge/license-MIT-3DA639" alt="MIT license">
</p>

> [!NOTE]
> **Young software, used every day by its author.** Since 0.5.0 Nordend opens **text files** in
> Quick Look with **syntax colours** for over twenty languages (code, configuration, Markdown,
> diffs), to select and copy; read, like pictures since 0.4.0, by Nordend's own code in a
> **caged helper process**. 0.6 **follows your theme's colours by itself**, as GTK apps do,
> and draws **one header**: the path, back and forward and the window's buttons in one row;
> 0.6.1 gives it **rounded corners and a shadow** (or square ones), and a see-through
> background if you want one.
> On Void Linux, [install it as a package](#on-void-linux); on NixOS or with Nix, from
> [its flake](#on-nixos-and-with-nix); anywhere else, [build it from source](#from-source) in
> about a minute.

<p align="center">
  <img src="docs/images/readme-screenshot.webp" alt="Nordend in a dark theme taken from Noctalia: one header with back and forward, the path and the window's buttons, the sidebar running to the top, three tabs, five columns down a project folder, and the info pane with a JSON file's coloured preview" width="100%">
  <br>
  <sub>One header, the sidebar to the top, three tabs, the columns down a project, and a JSON file's preview in the info pane, all in the colours of the desktop's theme.</sub>
</p>

## Why Nordend

Nordend talks to the compositor directly, with **no toolkit at all**: text, icons, scrolling and
animations come from its own renderer. It follows your theme by itself, as GTK apps do: the
colours in your GTK settings (what Noctalia, Stylix or matugen write there), else the desktop's
light or dark style and accent, live ([Colours](#colours)).

- **Fast.** The first frame in about 65 ms, a 10 000-file folder in 0.7 ms, a raw photo in
  Quick Look in under 100 ms. Every figure is measured, with the tool to measure it again.
- **Light.** A 3 MB binary, 17 libraries, about 25 MB of memory, and no CPU at all while
  nothing moves.
- **Safe with what it reads.** Files that come from outside are read by `nordend-preview`, a
  small helper process in a cage of Nordend's own (seccomp and landlock: no files, no network,
  no programs). A broken or hostile file costs at most its preview; Nordend carries on.
- **Its own readers.** TIFF (LZW, Deflate, PackBits, JPEG, tiles, BigTIFF), PGM and PPM are
  read by code written for Nordend, checked pixel for pixel against libtiff and fuzzed; PNG, JPEG
  and GIF by [wuffs](https://github.com/google/wuffs), memory-safe by construction. Raw files
  show the JPEG their camera stored, so the camera's own rendering; a DNG whose stored previews
  are too small is developed by Nordend's own reader, with the file's own colour.
- **Photos and their editors.** Thumbnails, the info pane and Quick Look with the shot's EXIF
  and rating; darktable sidecars travel with their photo through every rename, copy, move and
  trip to the trash.
- **Three views of the same folder.** Columns, a thumbnail grid and a details table
  (<kbd>Ctrl</kbd>+<kbd>1</kbd> <kbd>2</kbd> <kbd>3</kbd>); switching keeps your place and your
  selection. Sort by name, size, date or type from the context menu, in every view.
- **Tabs.** <kbd>Ctrl</kbd>+<kbd>T</kbd>, or a middle click on a folder to open it behind the
  one you are in; each tab keeps its folder, view and history, and they all come back at the next
  start.
- **Careful with your files.** Nothing overwritten without asking, nothing half-copied under a
  real name, a real freedesktop trash, and <kbd>Ctrl</kbd>+<kbd>Z</kbd>.
- **Keyboard first, mouse always.** Every action has a key (<kbd>F1</kbd> lists them) and a click.

Everything it does is in [docs/features.md](docs/features.md); inside Nordend, <kbd>F1</kbd> lists
every shortcut.

## Features

- **Views:** columns, thumbnail grid, details table; tabs; zoom; sort by name, size, date or type
- **Quick Look** (<kbd>Space</kbd>): photos fill the window, raw files included; text files on a
  scrollable page; the arrows walk the folder
- **Raw files:** the previews cameras embed in ORF, CR2, NEF, ARW, RAF, RW2, SRW, DNG, 3FR
  (found by Nordend's own reader), upright and cropped to the shot; DNGs with only a small
  preview (Leica, drones) developed by Nordend itself; EXIF in the info pane
- **Images:** PNG, JPEG, GIF (wuffs), WebP, JPEG XL, and TIFF, PGM and PPM with Nordend's own
  readers; thumbnails in the shared freedesktop cache
- **Isolated reading:** every picture is made in a caged helper process, never in Nordend
- **darktable:** sidecars follow their photo everywhere; ratings shown
- **Command palette** (<kbd>Ctrl</kbd>+<kbd>K</kbd>): every command, place and tab, fuzzy-matched
  (fzy), the recently used first
- **Find files** (<kbd>Ctrl</kbd>+<kbd>F</kbd>): fuzzy search of every file under the folder,
  first results in milliseconds even among 270 000; Enter shows it, Space opens Quick Look
- **Files:** copy and move with progress and conflict questions, rename, new folder or file
  from templates, undo and redo, freedesktop trash with restore, delete permanently
- **Drag and drop** within Nordend and with other apps; rubber-band selection; the system
  clipboard
- **Sidebar:** places, GTK bookmarks, disks through udisks2 (mount, unmount, eject)
- **Opening:** default applications and "Open With", resolved exactly as GLib does; a terminal
  in any folder (<kbd>F4</kbd>)
- **Always current:** folders follow changes on disk (inotify); history with
  <kbd>Alt</kbd>+<kbd>←</kbd> <kbd>→</kbd> and the mouse's side buttons
- **Looks:** your theme's colours live, read like a GTK app reads them (Noctalia, Stylix,
  matugen, a hand-made `gtk.css`), or the desktop's light/dark style and accent (GNOME, Plasma);
  its icon follows the accent; fractional scaling; one header, as in a modern file manager:
  the sidebar to the top, the path and the window's buttons in one row
- **Remembers** views, order, tabs and window size; a short `config.toml`, reloaded live

## At a glance

| | |
|---|---|
| Binary | **4.09 MB** (3.42 MB stripped), x86-64; the decoders and text readers in `nordend-preview`, 1.15 MB (1.03 stripped), a separate caged helper |
| Libraries | 17 direct (the helper: 8); no GTK, no Qt, no GLib |
| Code | about 52 800 lines of C++20, 18 800 of tests (450 test cases, under ASan and TSan too) |
| Memory | **25 MB** of its own at start; pictures are decoded in the helpers, which give their memory back after a quiet second |
| Idle | **0 CPU**: nothing is drawn while nothing moves |

## Benchmarks

The design's four targets, measured on 2026-11-01 (Ryzen 7 5700G, RX 6650 XT, SATA SSD):

| Target | Measured |
|--------|---------:|
| Cold start to the first frame on screen: < 150 ms | **128 ms** (warm: **64 ms**) |
| A 10 000-file folder: first frame < 50 ms | first entries in **0.7 ms**, all in **17 ms** |
| Quick Look of an ORF raw photo: < 100 ms | **82-96 ms** |
| Scrolling with no dropped frame | worst frame **3.7 ms** of 13.3 (75 Hz) |

Every measurement and its method is in [docs/benchmarks.md](docs/benchmarks.md).

## Install

Nordend needs Linux with a Wayland compositor that offers xdg-shell (developed on Umbriel, also
tried on GNOME, Plasma and niri) and a GPU driver with OpenGL ES 3.0 (Mesa).

### On Void Linux

Nordend is developed on Void, and comes with an xbps-src template in
[`packaging/void`](packaging/void). It builds the latest release into a package that puts
`nordend`, its desktop entry and its icon under `/usr`:

```sh
# 1. void-packages, once (skip if you already have a checkout)
git clone --depth 1 https://github.com/void-linux/void-packages
cd void-packages
./xbps-src binary-bootstrap

# 2. Nordend's template, then the package
git clone --depth 1 https://github.com/pankaspe/nordend-fm /tmp/nordend-fm
cp -r /tmp/nordend-fm/packaging/void/nordend srcpkgs/
./xbps-src pkg nordend
sudo xbps-install -R hostdir/binpkgs nordend
```

The package is the latest release, **0.6.1**. What `main` gains after it comes with the next
release; to have it at once, build from source.

### On NixOS and with Nix

Nordend comes with a flake: [`flake.nix`](flake.nix), and the package in
[`packaging/nix`](packaging/nix). It is not in nixpkgs, and there is no binary cache: Nix builds
it on your machine, in a few minutes, its tests included. Since 0.5.1, for x86_64 Linux, tried
on NixOS 26.05 with GNOME.

```sh
nix run github:pankaspe/nordend-fm           # try it
nix profile add github:pankaspe/nordend-fm   # keep it (nix profile install in older Nix)
```

In a NixOS configuration made with flakes:

```nix
# flake.nix: inputs
nordend.url = "github:pankaspe/nordend-fm";

# a module (with inputs passed in specialArgs)
nixpkgs.overlays = [ inputs.nordend.overlays.default ];
environment.systemPackages = [ pkgs.nordend ];
```

### From source

On Void Linux the packages are these; elsewhere, the development packages of the same libraries
(their names differ and have not been tried yet):

```sh
sudo xbps-install -S cmake ninja pkgconf wayland-devel wayland-protocols \
    libxkbcommon-devel libglvnd-devel MesaLib-devel \
    harfbuzz-devel freetype-devel fontconfig-devel tomlplusplus-devel \
    libwebp-devel libjxl-devel zlib-devel sdbus-c++-devel
git clone https://github.com/pankaspe/nordend-fm
cd nordend-fm
cmake -B build -G Ninja
cmake --build build
./build/nordend            # your home folder, or ./build/nordend ~/Pictures
cmake --install build --prefix ~/.local --strip   # nordend, its desktop entry and icon
```

### Then

Make it the default for folders (a browser's "Show in folder", `xdg-open`) and give it a key:

```sh
xdg-mime default nordend.desktop inode/directory
```

```toml
# Umbriel's config.toml; other compositors have their own way
"Mod+E" = { action = "spawn nordend", repeat = false }
```

### Colours

Nothing to set up: Nordend follows your theme by itself, as GTK apps such as Thunar do. It
reads the colours of your GTK settings (`~/.config/gtk-4.0/gtk.css`, else `gtk-3.0`, and what
they import) and changes with them, live. Whatever themes GTK writes them there: on Noctalia,
its GTK theming (the built-in `gtk4` and `gtk3` templates); Stylix, matugen, or your own
`gtk.css`. Without them, Nordend takes the desktop's light or dark style and accent, where its
portal offers them (GNOME, Plasma), else its own default palette.

Configuration: every key, with its default, in [docs/config.example.toml](docs/config.example.toml)
(copy it to `~/.config/nordend/config.toml`; `nordend --check-config` reports any problem). The
command line: `nordend [DIR]`, `--config FILE`, `--version`, and `--about` (what Nordend runs on,
for a bug report).

## Versions

Nordend stays at 0.x while it is young. A new **minor** version (0.2.0, 0.3.0…) brings features
you can see; a **patch** (0.2.1) only fixes things. 1.0.0 will come once Nordend has been its
author's everyday file manager for a while and its files (`config.toml`, `state.toml`) have
settled.

| Version | What it brought |
|---|---|
| [0.6.1](https://github.com/pankaspe/nordend-fm/releases/tag/v0.6.1), 2026-11-13 | Rounded corners and a soft shadow around Nordend's own header, as GTK draws them (square with `[window] corner_radius = 0`); the shadow's edge resizes the window; a see-through background with solid text, icons and pictures (`[window] background_opacity`, by barrulus); a clean exit at logout, with the session's tabs and views saved, when the session bus goes first (GNOME) |
| [0.6.0](https://github.com/pankaspe/nordend-fm/releases/tag/v0.6.0), 2026-11-13 | Colours from your theme with nothing to set up, read as GTK apps read them (Noctalia, Stylix, matugen, your own `gtk.css`), live; one header, as in a modern file manager: the sidebar to the top, back and forward, the path and the window's buttons in one row (`[window] title_bar = "compositor"` for the compositor's bar), tried on Umbriel, GNOME and Plasma |
| [0.5.1](https://github.com/pankaspe/nordend-fm/releases/tag/v0.5.1), 2026-11-13 | Grid thumbnails up to 512 px, sharp from the larger caches (icons stay at 256); builds hardened on every system (stack protection, full RELRO, `_FORTIFY_SOURCE=3`, the standard library's index checks); a Nix flake, built and tested in Nix's sandbox on every push; sorting by type puts a file whose name has several types (`.md` with shared-mime-info 2.5) among its likely type |
| [0.5.0](https://github.com/pankaspe/nordend-fm/releases/tag/v0.5.0), 2026-11-09 | Text previews: text files in Quick Look and the info pane, read in the caged helper (UTF-8, UTF-16, Latin-1) with syntax colours from your theme for C, C++, Rust, Python, JavaScript, TypeScript, Lua, Luau, shell (heredocs included), fish, CMake, Makefiles, JSON, TOML, YAML, INI files and desktop entries, `.gitignore` and `.gitattributes`, Markdown (code blocks in their own language) and diffs; select with the pointer, Ctrl+A, Ctrl+C; long lines scroll sideways; Home, End, Page Up and Page Down; a caption with the language, encoding and line ends; minified files shown plain; logs opened at their newest lines. The helper became the first engine, described by a manifest |
| [0.4.0](https://github.com/pankaspe/nordend-fm/releases/tag/v0.4.0), 2026-11-08 | Image previews read by Nordend's own code: DNG developed when its previews are too small (Leica M240 and M8, DJI drones, Adobe-converted files) with the file's colour, tone curve, vignetting and lens corrections, read identically to LibRaw and 2-4 times faster; TIFF with its own reader (BigTIFF, LZW, Deflate, PackBits, JPEG, 8/16-bit and float, CMYK, tiles, a scanner's scan in a SubIFD); raw files' previews and the info pane's EXIF without exiv2 (the same results, 3 to 100 times quicker); pictures made in small `nordend-preview` processes, each in a cage of Nordend's own (seccomp and landlock), so a file that crashes or hangs one costs only its preview; one path for every picture, told by the file's bytes; a plain sentence when a file cannot be shown. Canon CRW and Minolta MRW get that sentence; darktable sidecars' ratings are no longer read (Nordend's own ratings come next) |
| [0.3.1](https://github.com/pankaspe/nordend-fm/releases/tag/v0.3.1), 2026-11-07 | Fixes: DNG previews stored as a JPEG inside a TIFF (Leica M240, Samsung GX20 at full size), broken previews passed over for the next one (Samsung SRW), PGM and PPM images, a plain message for raw formats with no readable preview (Sigma X3F, old Panasonic RAW) |
| [0.3.0](https://github.com/pankaspe/nordend-fm/releases/tag/v0.3.0), 2026-11-06 | The command palette (<kbd>Ctrl</kbd>+<kbd>K</kbd>, fuzzy matching with fzy); finding files under a folder (<kbd>Ctrl</kbd>+<kbd>F</kbd>); Nordend's icon follows the desktop's accent colour (`icon.follow_accent`) |
| [0.2.0](https://github.com/pankaspe/nordend-fm/releases/tag/v0.2.0), 2026-11-06 | Sort By in the context menu; tabs (a folder, a view, a history and a zoom each, remembered); zoom in the columns and the details |
| [0.1.0](https://github.com/pankaspe/nordend-fm/releases/tag/v0.1.0), 2026-11-06 | The first release: three views, raw previews and Quick Look, file operations with undo, trash, drag and drop, disks, the desktop's look |

## What's next

Nordend stays what it is: a native C++ file manager, at home on any Wayland desktop and right at
home on Noctalia, with no GTK or Qt; very light, safe and fast. Everything else is a feature on
top, added only when it keeps those three.

0.1.0 came out after a week of daily use by the author; 0.2.0 added Sort By, tabs and zoom in the
lists; 0.3.0 the command palette (<kbd>Ctrl</kbd>+<kbd>K</kbd>) and finding files
(<kbd>Ctrl</kbd>+<kbd>F</kbd>); 0.3.1 more raw previews; 0.4.0 image previews read by Nordend's own
code in caged helpers; 0.5.0 text previews with syntax colours, selecting and copying; 0.5.1
grid thumbnails up to 512 px, hardened builds and a Nix flake; 0.6.0 the colours of your theme
with nothing to set up and one header for the whole window; 0.6.1 rounded corners and a shadow
around it, square corners or a see-through background on request. Coming, roughly in this order:
**a file chooser for other applications** (the desktop portal's, with the grid and thumbnails);
then **previews, one family at a time**: other common formats one by one (SVG, AVIF, fonts,
archive contents, audio tags).
After that: colour labels and star ratings of your own, not tied to any raw editor; a Quick Look
for photographers (100 % zoom, EXIF, histogram, comparison); a dual pane, a queue of operations
and batch renaming; translations, plugins in Luau (one system, each plugin caged, for things
outside Nordend such as local AI search).

## Documentation

[Features](docs/features.md) ·
[Devlog](docs/devlog.md) ·
[Benchmarks](docs/benchmarks.md) ·
[Compatibility](docs/compatibility.md) ·
[Upstream bug log](docs/buglog.md)

## Development

Contributions are welcome: bug reports, fixes, and features after a short discussion in an issue.
See [CONTRIBUTING.md](CONTRIBUTING.md).

Nordend is copiloted by **Claude Opus** (Anthropic), which assists with code writing, code analysis,
bug hunting, testing and documentation. Design decisions, testing on real hardware and every release
are the author's. Step numbers in code comments ("ROADMAP 6.3") refer to the internal roadmap.

## License

[MIT](LICENSE). Third-party code and data (the wuffs decoder, the Tabler icons, Noctalia's default
palette) are listed in [THIRD_PARTY.md](THIRD_PARTY.md).
