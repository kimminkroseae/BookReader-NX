# BookReader-NX

A manga, comic and e-book reader for the Nintendo Switch homebrew menu.

BookReader is a rework of [WookReader](https://github.com/exorevan/WookReader) by
exorevan (itself a fork of eBookReaderSwitch) with a new interface, auto-rotation,
a manga-aware dark mode, chapter continuation and an in-RAM page cache.

## Installation

1. Copy `BookReader.nro` to **`/switch/BookReader.nro`** on the SD card.
2. Put your books, comics and folders in **`/switch/.BookReader/books/`**.
   The data folder starts with a dot so the homebrew menu doesn't list it.
   (Windows shows dot-folders normally; on macOS press Cmd+Shift+. to see it.)
3. Optional: copy `BookReader.cfg` to `/config/nx-hbmenu/fileassoc/` to open
   files straight from the homebrew menu.

Upgrading from WookReader moves `/switch/.WookReader/`, `/switch/WookReader/`
or `/switch/eBookReader/` into the new hidden folder automatically on first start
(books, reading progress, notes and settings are kept). Delete the old `.nro`.

## Formats

| Kind | Extensions |
|---|---|
| Comics / manga | CBZ, CBR, CB7, CBT, ZIP, RAR, 7Z, TAR, a folder of images |
| Images inside | JPEG, PNG, WebP, GIF, BMP |
| Documents | PDF, XPS/OXPS |
| Reflowable books | EPUB, FB2, XHTML/HTML, TXT (re-flowed to the screen; text size in Settings) |

A folder containing loose images shows a **"Read the images in this folder"** card.

## Dark mode, manga and books

Dark mode makes all backgrounds black. Pages are treated by type:

- **Manga / comic:** artwork is left untouched (colour pages stay colour), the
  area around the page is black.
- **Book:** brightness is inverted (white paper turns black, black text turns
  white) while colours keep their hue.

Press **X** in the library to mark a **folder** (applies to everything inside,
including sub-folders) or a single **file**. A file with no type asks you the
first time you open it (can be turned off in Settings).

## Speed and memory

When a book opens, BookReader keeps working ahead in the background:

- Comic archives are copied into RAM (when they fit), so pages never wait on the SD card.
- Pages are decoded / rendered ahead of you into RAM, nearest pages first, up to a
  memory budget based on the free memory. Most chapters fit entirely, so every
  page turn (and slider jump) becomes instant after a few seconds.
- Everything is freed as soon as the book is closed.

**Launch through title-override** (hold R while starting any game). In applet
mode (from the Album) the app only gets a few hundred MB, so the cache is kept
small; the library shows an "APPLET MODE" tag and a one-time notice.

## Chapters

Turning past the last page shows the next book in the folder; turn again to open
it. Turning back from the first page offers the previous one, opened at its last
page. The **Continue reading** card at the top of the library reopens your last
book where you left off, or shows **Up next** once it is finished.

## Rotation

Hold the Switch upright like a book and the page rotates with it (motion sensor,
handheld mode). You can lock the rotation in Settings or the reader menu. If
pages come out upside down, set *Settings → Rotation → Auto-rotate direction* to
*Reversed*. Touch, D-pad and sticks all follow the rotation.

## Reader controls

| Input | Action |
|---|---|
| ZR / R / tap right side | Next page (scrolls first on tall pages) |
| ZL / L / tap left side | Previous page |
| D-pad | Turn pages / scroll (follows rotation and manga direction) |
| Swipe sideways | Turn page (when the page fits the width) |
| Left stick | Flick to turn pages; moves around a zoomed page |
| Drag | Move around a zoomed page |
| Right stick up/down / pinch | Zoom |
| Stick click | Reset zoom |
| Y | Fit page → Fit width → Two pages (comics) |
| X / tap middle | Show/hide the top bar (with page slider) |
| A | Go to page |
| − | Dark mode |
| + | Reader menu (rotation, fit, manga/book, direction, notes, …) |
| B | Close book |

## Library controls

| Input | Action |
|---|---|
| D-pad / stick / touch | Move, scroll (with momentum) |
| A / tap | Open |
| B | Back / exit |
| X | Mark folder or file as manga / book |
| Y | Notes for a book |
| ZL / ZR | More / fewer covers per row |
| − | Light / dark theme |
| + | Settings |

## Building

Requires devkitPro (devkitA64, libnx, Switch portlibs). On Arch Linux:

```
bash tools/install_devkitpro.sh   # once
bash tools/build.sh              # produces BookReader.nro in the project folder
```

`tools/build.sh` builds in `~/.cache/wookreader-build` because devkitPro's
makefiles break on paths containing characters like `&`. The first build also
compiles MuPDF (a few minutes); later builds are incremental.

## Credits

- [WookReader](https://github.com/exorevan/WookReader) by exorevan — the base this project is built on
- eBookReaderSwitch and moronigranja — original reader and expanded format support
- NX-Shell Team — original application code base
- Papirus Icon Theme — folder/file icon design inspiration
- [MuPDF](https://mupdf.com/) (Artifex) — PDF/EPUB/XPS rendering

## License

GPL-2.0 (see `LICENSE`), as inherited from WookReader. The bundled MuPDF source
in `mupdf/` is licensed under the AGPL-3.0 (see `mupdf/COPYING`).
