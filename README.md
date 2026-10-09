# WristFrame

A Pebble watchapp that shows a photo full screen, and nothing else. It holds up
to 4 photos; press Up and Down on the watch to switch between them.

Supported watches: **Pebble Time 2** (`emery`, 200×228) and **Pebble Round 2**
(`gabbro`, 260×260 round).

## Usage

1. Install the `.pbw` (from the latest [GitHub Actions run](../../actions) or
   [release](../../releases)) with the Pebble app.
2. Open the app's settings in the Pebble app, tap one of the 4 slots, choose a
   photo and tap **Send to slot N**. Repeat for other slots, or use
   **Clear slot N** to empty a slot.
3. Photos are stored on the watch, so they are available even when the phone
   is not connected. Press **Up** and **Down** to switch between them. The app
   reopens on the last photo shown.

## How it works

All image processing happens on the phone, in the settings page
([`config/index.html`](config/index.html)):

- **Crop and resize:** the photo is scaled to cover the whole screen and the
  overflow is cropped evenly from both sides, so the screen is always filled.
- **Dither:** Floyd–Steinberg dithering to the watch's 64-colour palette.
- **Pack:** 6 bits per pixel. On the round watch, pixels outside the visible
  circle are left out (about 21% less data).

The packed image is about 34 KB on the Time 2 and 40 KB on the Round 2. The
settings page returns it to the app as URL-safe base64, together with a small
thumbnail for the slot tiles. The phone-side JavaScript
([`src/pkjs/index.js`](src/pkjs/index.js)) queues each change and sends it to
the watch in 2 KB AppMessage chunks. If a transfer is interrupted, it is
retried the next time the app opens.

The watch ([`src/c/main.c`](src/c/main.c)) decodes chunks as they arrive. It
saves them to persistent storage in 256-byte records, plus a header with a
checksum that is written only once the whole image has arrived. Each slot has
its own range of storage keys. On launch it reads back the last photo shown.

The slot count is capped at 4 because persistent storage slows down as it
fills with records, including the dead records left behind when a photo is
replaced ([PebbleOS issue #2106](https://github.com/coredevices/PebbleOS/issues/2106)).
If the app becomes slow to open after many photo changes, reinstalling it
clears the stored data. This needs firmware with the 1 MB per-app
persistent storage limit (PebbleOS v4.9.171 or later). On older firmware the
image is shown but not remembered.

## Settings page hosting

The settings page is served from GitHub Pages at
`https://lanrat.github.io/WristFrame/`. The workflow deploys `config/` on
every push to the default branch. Enable it once under **Settings → Pages →
Source: GitHub Actions**. If you fork the repo, update `CONFIG_URL` in
`src/pkjs/index.js`.

## Building

```sh
uv tool install pebble-tool
pebble sdk install latest
pebble build
```

The `.pbw` is written to `build/`. To try it in the emulator:

```sh
pebble install --emulator emery
pebble emu-app-config --emulator emery --file config/index.html
```

When you use `--file`, the page gets no screen size from the app and defaults
to the Time 2's 200×228. For the Round 2, add
`&width=260&height=260&round=1` to the URL the browser opens.
