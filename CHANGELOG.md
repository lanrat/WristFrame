# Changelog

Each release's notes come from its section here: the release workflow copies
the section for the new version into the GitHub release and both app stores.

## 1.2.0

- **Switch photos while a batch is arriving.** Pressing Up or Down while photos are being sent now shows another photo, and the transfer carries on in the background. Previously this cancelled the transfer, and the rest of the batch waited until the app was reopened.
- **Recovers from interrupted transfers.** If the phone disconnects or stops sending for 15 seconds, the watch drops the partial photo and shows a complete one. When the connection comes back, the remaining photos are sent without reopening the app.
- **Lost confirmations no longer stall the queue.** The phone sends a photo again if the watch's confirmation doesn't arrive within 10 seconds.
- **Fixed:** when the watch rejected a photo, it reported the wrong slot back to the phone, which then stopped sending.
- **New menu icon** in the watch's app list.
- **Settings page:** results now go only to the Pebble app or a local emulator, and the HEIF decoder is updated to libheif-js 1.23.5.

## 1.1.0

- **Send several changes at once.** Choose photos for any of the 4 slots, or clear them, then send everything together.
- **Progress on the watch.** While a photo arrives, the watch shows a progress bar (a ring on the Round 2) and a count such as "2/3".
- **iPhone photos.** HEIF photos now work in every browser, not just Safari.
- **Large photos** are shrunk while decoding, so 12 MP camera photos no longer run the settings page out of memory.
- Errors are shown on the settings page instead of failing silently.

## 1.0.0

- First release: shows a photo full screen on the Pebble Time 2 and Pebble Round 2.
- Holds up to 4 photos; press Up and Down to switch between them.
- Photos are converted on the phone, dithered to the watch's 64 colours, and stored on the watch so they work without the phone.
