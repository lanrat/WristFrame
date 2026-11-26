#include <pebble.h>

// Image wire/storage format (produced by config/index.html on the phone):
// 64-colour pixels, 6 bits each (rrggbb), packed MSB-first, row-major.
// On round displays only the pixels inside the visible circle are included
// (see prv_row_span, mirrored in config/index.html).
//
// Up to SLOT_COUNT images are stored; Up/Down switch between them.

#define SLOT_COUNT 4
#define META_KEY(slot) (1 + (slot))
#define CURRENT_KEY 10
#define DATA_KEY(slot, record) (100 + (slot) * 200 + (record))
#define META_VERSION 1
#define RECORD_SIZE PERSIST_DATA_MAX_LENGTH
#define INBOX_SIZE_CAP (2048 + 64)
#define OUTBOX_SIZE 64
// A transfer that receives nothing for this long is abandoned.
#define RX_TIMEOUT_MS 15000

enum {
  CMD_HELLO = 1,  // phone -> watch: request READY
  CMD_READY = 2,  // watch -> phone: screen info, chunk size, filled slots
  CMD_BEGIN = 3,  // phone -> watch: new image header
  CMD_CHUNK = 4,  // phone -> watch: image data
  CMD_END = 5,    // phone -> watch: transfer complete
  CMD_DONE = 6,   // watch -> phone: result of END or CLEAR
  CMD_CLEAR = 7,  // phone -> watch: delete a slot
};

typedef struct __attribute__((packed)) {
  uint32_t version;
  uint16_t width;
  uint16_t height;
  uint8_t round;
  uint32_t size;
  uint32_t checksum;
} ImageMeta;

typedef struct {
  uint8_t *data;
  uint16_t stride;
  int width;
  int height;
  bool round;
  int y;
  int x;
  int x_end;
  uint32_t acc;
  int bits;
} Decoder;

static Window *s_window;
static Layer *s_image_layer;
static TextLayer *s_hint_layer;
static GBitmap *s_bitmap;
static Decoder s_decoder;

// Bit n set when slot n holds a complete image.
static uint8_t s_filled;
// Slot on screen, or -1 when none.
static int s_current = -1;
static int s_saved_current = -1;

static struct {
  bool active;
  bool persist;
  // Whether the image is drawn on screen as it arrives. A button press during
  // a transfer moves away from it, and the rest of that batch then arrives in
  // the background.
  bool shown;
  int slot;
  // Position of this image in the phone's current batch, for the progress label.
  int index;
  int count;
  ImageMeta meta;
  uint32_t offset;
  uint32_t checksum;
} s_rx;
static AppTimer *s_rx_timer;

// --- Image format helpers ----------------------------------------------------

static uint32_t prv_fnv1a(uint32_t hash, const uint8_t *buf, size_t len) {
  for (size_t i = 0; i < len; i++) {
    hash ^= buf[i];
    hash *= 16777619u;
  }
  return hash;
}

#define FNV_OFFSET 2166136261u

// Visible pixel columns [x0, x1) of row y. On round displays this keeps the
// pixels whose centre lies within the screen radius plus a 1px margin.
static void prv_row_span(int w, int h, bool round, int y, int *x0, int *x1) {
  if (!round) {
    *x0 = 0;
    *x1 = w;
    return;
  }
  const int d = (w < h ? w : h) + 2;  // doubled radius
  const int dy = 2 * y + 1 - h;
  const int lim = d * d - dy * dy;
  if (lim < 0) {
    *x0 = *x1 = 0;
    return;
  }
  int x = 0;
  while (x < w / 2 && (2 * x + 1 - w) * (2 * x + 1 - w) > lim) {
    x++;
  }
  *x0 = x;
  *x1 = w - x;
}

static uint32_t prv_packed_size(int w, int h, bool round) {
  uint32_t pixels = 0;
  for (int y = 0; y < h; y++) {
    int x0, x1;
    prv_row_span(w, h, round, y, &x0, &x1);
    pixels += x1 - x0;
  }
  return (pixels * 6 + 7) / 8;
}

// --- Decoder -----------------------------------------------------------------

static void prv_decoder_next_row(Decoder *d) {
  while (d->y < d->height) {
    prv_row_span(d->width, d->height, d->round, d->y, &d->x, &d->x_end);
    if (d->x < d->x_end) {
      return;
    }
    d->y++;
  }
}

static bool prv_bitmap_create(int w, int h, bool round) {
  if (s_bitmap) {
    gbitmap_destroy(s_bitmap);
    s_bitmap = NULL;
  }
  s_bitmap = gbitmap_create_blank(GSize(w, h), GBitmapFormat8Bit);
  if (!s_bitmap) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Out of memory for %dx%d bitmap", w, h);
    return false;
  }
  s_decoder = (Decoder){
    .data = gbitmap_get_data(s_bitmap),
    .stride = gbitmap_get_bytes_per_row(s_bitmap),
    .width = w,
    .height = h,
    .round = round,
  };
  memset(s_decoder.data, GColorBlack.argb, s_decoder.stride * h);
  prv_decoder_next_row(&s_decoder);
  return true;
}

static void prv_decoder_feed(Decoder *d, const uint8_t *buf, size_t len) {
  for (size_t i = 0; i < len; i++) {
    d->acc = (d->acc << 8) | buf[i];
    d->bits += 8;
    while (d->bits >= 6) {
      d->bits -= 6;
      const uint8_t v = (d->acc >> d->bits) & 0x3F;
      if (d->y < d->height) {
        d->data[d->y * d->stride + d->x] = 0xC0 | v;
        if (++d->x >= d->x_end) {
          d->y++;
          prv_decoder_next_row(d);
        }
      }
    }
    d->acc &= (1u << d->bits) - 1;
  }
}

static void prv_show_image(bool show) {
  layer_set_hidden(text_layer_get_layer(s_hint_layer), show);
  layer_mark_dirty(s_image_layer);
}

static void prv_discard_image(void) {
  if (s_bitmap) {
    gbitmap_destroy(s_bitmap);
    s_bitmap = NULL;
  }
  s_current = -1;
  prv_show_image(false);
}

// --- Persistent storage ------------------------------------------------------

static bool prv_valid_slot(int32_t slot) {
  return slot >= 0 && slot < SLOT_COUNT;
}

static bool prv_meta_valid(const ImageMeta *meta) {
  const GRect bounds = layer_get_bounds(window_get_root_layer(s_window));
  return meta->version == META_VERSION && meta->width == bounds.size.w &&
         meta->height == bounds.size.h && meta->round == PBL_IF_ROUND_ELSE(1, 0) &&
         meta->size == prv_packed_size(meta->width, meta->height, meta->round);
}

static bool prv_read_meta(int slot, ImageMeta *meta) {
  return persist_read_data(META_KEY(slot), meta, sizeof(*meta)) == sizeof(*meta) &&
         prv_meta_valid(meta);
}

static bool prv_load_slot(int slot) {
  ImageMeta meta;
  if (!prv_read_meta(slot, &meta) || !prv_bitmap_create(meta.width, meta.height, meta.round)) {
    return false;
  }
  uint8_t buf[RECORD_SIZE];
  uint32_t checksum = FNV_OFFSET;
  for (uint32_t offset = 0; offset < meta.size; offset += RECORD_SIZE) {
    const uint32_t len = meta.size - offset < RECORD_SIZE ? meta.size - offset : RECORD_SIZE;
    if (persist_read_data(DATA_KEY(slot, offset / RECORD_SIZE), buf, len) != (int)len) {
      APP_LOG(APP_LOG_LEVEL_WARNING, "Slot %d is incomplete", slot);
      return false;
    }
    checksum = prv_fnv1a(checksum, buf, len);
    prv_decoder_feed(&s_decoder, buf, len);
  }
  if (checksum != meta.checksum) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "Slot %d checksum mismatch", slot);
    return false;
  }
  return true;
}

// Shows the first loadable filled slot, starting at `slot` and stepping by
// `dir` (wrapping). Slots that fail to load are marked empty.
static void prv_show_from(int slot, int dir) {
  for (int i = 0; i < SLOT_COUNT; i++) {
    const int s = (slot + dir * i + SLOT_COUNT * SLOT_COUNT) % SLOT_COUNT;
    if (!(s_filled & (1 << s))) {
      continue;
    }
    if (prv_load_slot(s)) {
      s_current = s;
      prv_show_image(true);
      return;
    }
    s_filled &= ~(1 << s);
  }
  prv_discard_image();
}

// --- AppMessage --------------------------------------------------------------

static void prv_send(uint8_t cmd, int32_t status, int32_t slot) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
    return;
  }
  dict_write_uint8(iter, MESSAGE_KEY_Cmd, cmd);
  if (cmd == CMD_READY) {
    const GRect bounds = layer_get_bounds(window_get_root_layer(s_window));
    const uint32_t inbox = app_message_inbox_size_maximum();
    const uint32_t capped = inbox < INBOX_SIZE_CAP ? inbox : INBOX_SIZE_CAP;
    dict_write_int32(iter, MESSAGE_KEY_Width, bounds.size.w);
    dict_write_int32(iter, MESSAGE_KEY_Height, bounds.size.h);
    dict_write_int32(iter, MESSAGE_KEY_Round, PBL_IF_ROUND_ELSE(1, 0));
    dict_write_int32(iter, MESSAGE_KEY_ChunkSize, ((capped - 64) / RECORD_SIZE) * RECORD_SIZE);
    dict_write_int32(iter, MESSAGE_KEY_Slots, s_filled);
  } else {
    dict_write_int32(iter, MESSAGE_KEY_Status, status);
    dict_write_int32(iter, MESSAGE_KEY_Slot, slot);
  }
  app_message_outbox_send();
}

static int32_t prv_int(DictionaryIterator *iter, uint32_t key) {
  Tuple *t = dict_find(iter, key);
  return t ? t->value->int32 : -1;
}

static void prv_rx_stop(void) {
  s_rx.active = false;
  if (s_rx_timer) {
    app_timer_cancel(s_rx_timer);
    s_rx_timer = NULL;
  }
  layer_mark_dirty(s_image_layer);
}

// Gives up on a transfer that stopped arriving, replacing its partial image.
// The phone sends it again on a later launch or reconnect.
static void prv_rx_abandon(void) {
  if (!s_rx.active) {
    return;
  }
  APP_LOG(APP_LOG_LEVEL_WARNING, "Image transfer for slot %d stopped", s_rx.slot);
  prv_rx_stop();
  if (s_current == s_rx.slot) {
    prv_show_from(s_rx.slot, 1);
  }
}

static void prv_rx_timeout(void *context) {
  s_rx_timer = NULL;
  prv_rx_abandon();
}

static void prv_rx_touch(void) {
  if (!s_rx_timer || !app_timer_reschedule(s_rx_timer, RX_TIMEOUT_MS)) {
    s_rx_timer = app_timer_register(RX_TIMEOUT_MS, prv_rx_timeout, NULL);
  }
}

static void prv_connection_changed(bool connected) {
  if (connected) {
    // Lets the phone resume changes that a lost connection interrupted.
    prv_send(CMD_READY, 0, 0);
  } else {
    prv_rx_abandon();
  }
}

static void prv_handle_begin(DictionaryIterator *iter) {
  const int32_t slot = prv_int(iter, MESSAGE_KEY_Slot);
  ImageMeta meta = {
    .version = META_VERSION,
    .width = prv_int(iter, MESSAGE_KEY_Width),
    .height = prv_int(iter, MESSAGE_KEY_Height),
    .round = prv_int(iter, MESSAGE_KEY_Round),
    .size = prv_int(iter, MESSAGE_KEY_Size),
    .checksum = prv_int(iter, MESSAGE_KEY_Checksum),
  };
  const int32_t index = prv_int(iter, MESSAGE_KEY_Index);
  prv_rx_stop();
  // Set before any early return: END reports this slot back to the phone.
  s_rx.slot = slot;
  if (!prv_valid_slot(slot) || !prv_meta_valid(&meta)) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Rejected image for slot %d: %dx%d size %d", (int)slot,
            meta.width, meta.height, (int)meta.size);
    return;
  }
  // Show the new image unless the user moved away from an earlier image of
  // this batch (index 1 starts a new batch; older phone versions send none).
  const bool show = index <= 1 || s_rx.shown || !s_bitmap || slot == s_current;
  if (show && !prv_bitmap_create(meta.width, meta.height, meta.round)) {
    prv_discard_image();
    return;
  }
  // Invalidate the slot until the new image is complete.
  if (persist_exists(META_KEY(slot))) {
    persist_delete(META_KEY(slot));
  }
  s_filled &= ~(1 << slot);
  if (show) {
    s_current = slot;
  }
  s_rx.active = true;
  s_rx.shown = show;
  s_rx.index = index;
  s_rx.count = prv_int(iter, MESSAGE_KEY_Count);
  s_rx.persist = persist_get_max_size() >= meta.size + 1024;
  s_rx.meta = meta;
  s_rx.offset = 0;
  s_rx.checksum = FNV_OFFSET;
  if (!s_rx.persist) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "Not enough persistent storage to remember the image");
  }
  prv_rx_touch();
  if (show) {
    prv_show_image(true);
  } else {
    layer_mark_dirty(s_image_layer);  // draws the progress bar
  }
}

static void prv_handle_chunk(DictionaryIterator *iter) {
  Tuple *data = dict_find(iter, MESSAGE_KEY_Data);
  if (!s_rx.active || !data || prv_int(iter, MESSAGE_KEY_Offset) != (int32_t)s_rx.offset ||
      s_rx.offset + data->length > s_rx.meta.size) {
    prv_rx_stop();
    return;
  }
  const uint8_t *buf = data->value->data;
  const uint16_t len = data->length;
  if (s_rx.persist) {
    for (uint16_t i = 0; i < len; i += RECORD_SIZE) {
      const uint16_t n = len - i < RECORD_SIZE ? len - i : RECORD_SIZE;
      const uint32_t key = DATA_KEY(s_rx.slot, (s_rx.offset + i) / RECORD_SIZE);
      if (persist_write_data(key, buf + i, n) != n) {
        APP_LOG(APP_LOG_LEVEL_WARNING, "Persist write failed");
        s_rx.persist = false;
        break;
      }
    }
  }
  s_rx.checksum = prv_fnv1a(s_rx.checksum, buf, len);
  if (s_rx.shown) {
    prv_decoder_feed(&s_decoder, buf, len);
  }
  s_rx.offset += len;
  prv_rx_touch();
  layer_mark_dirty(s_image_layer);
}

static void prv_handle_end(void) {
  const bool ok = s_rx.active && s_rx.offset == s_rx.meta.size &&
                  s_rx.checksum == s_rx.meta.checksum;
  const int slot = s_rx.slot;
  prv_rx_stop();  // also removes the progress bar
  if (!ok) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Image transfer failed");
    prv_send(CMD_DONE, 0, slot);
    // Replace the partial image, unless a button press already moved away.
    if (s_current == slot) {
      prv_show_from(slot, 1);
    }
    return;
  }
  // The image stays on screen either way, but only a stored image counts as
  // done; otherwise the phone retries on a later launch.
  const bool stored = s_rx.persist && persist_write_data(META_KEY(slot), &s_rx.meta,
                                                         sizeof(s_rx.meta)) == (int)sizeof(s_rx.meta);
  if (stored) {
    s_filled |= 1 << slot;
  }
  prv_send(CMD_DONE, stored ? 1 : 0, slot);
}

static void prv_handle_clear(DictionaryIterator *iter) {
  const int32_t slot = prv_int(iter, MESSAGE_KEY_Slot);
  if (!prv_valid_slot(slot)) {
    prv_send(CMD_DONE, 0, slot);
    return;
  }
  if (persist_exists(META_KEY(slot))) {
    persist_delete(META_KEY(slot));
  }
  s_filled &= ~(1 << slot);
  if (s_current == slot) {
    prv_show_from(slot, 1);
  }
  prv_send(CMD_DONE, 1, slot);
}

static void prv_inbox_received(DictionaryIterator *iter, void *context) {
  switch (prv_int(iter, MESSAGE_KEY_Cmd)) {
    case CMD_HELLO:
      prv_send(CMD_READY, 0, 0);
      break;
    case CMD_BEGIN:
      prv_handle_begin(iter);
      break;
    case CMD_CHUNK:
      prv_handle_chunk(iter);
      break;
    case CMD_END:
      prv_handle_end();
      break;
    case CMD_CLEAR:
      prv_handle_clear(iter);
      break;
  }
}

// --- UI ----------------------------------------------------------------------

static void prv_step(int dir) {
  const int from = s_current < 0 ? 0 : s_current;
  for (int i = 1; i <= SLOT_COUNT; i++) {
    const int s = (from + dir * i + SLOT_COUNT * SLOT_COUNT) % SLOT_COUNT;
    if (s_filled & (1 << s)) {
      if (s != s_current || !s_bitmap) {
        // An image still arriving keeps arriving, in the background.
        s_rx.shown = false;
        prv_show_from(s, dir);
      }
      return;
    }
  }
}

static void prv_up_click(ClickRecognizerRef recognizer, void *context) {
  prv_step(-1);
}

static void prv_down_click(ClickRecognizerRef recognizer, void *context) {
  prv_step(1);
}

static void prv_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, prv_up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_down_click);
}

#define PROGRESS_THICKNESS 6

// While an image is arriving: a bar along the bottom edge (a ring around the
// edge on round displays), plus "2/3" when the phone is sending several.
static void prv_draw_progress(GContext *ctx, GRect bounds) {
  if (!s_rx.active || s_rx.meta.size == 0) {
    return;
  }
  const int32_t done = s_rx.offset;
  const int32_t total = s_rx.meta.size;
#if defined(PBL_ROUND)
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_radial(ctx, bounds, GOvalScaleModeFitCircle, PROGRESS_THICKNESS, 0, TRIG_MAX_ANGLE);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_radial(ctx, grect_inset(bounds, GEdgeInsets(1)), GOvalScaleModeFitCircle,
                       PROGRESS_THICKNESS - 2, 0, (int32_t)((int64_t)TRIG_MAX_ANGLE * done / total));
  const int label_bottom = bounds.size.h - PROGRESS_THICKNESS - 14;
#else
  const GRect bar = GRect(0, bounds.size.h - PROGRESS_THICKNESS, bounds.size.w, PROGRESS_THICKNESS);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bar, 0, GCornerNone);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, GRect(0, bar.origin.y + 1, (bar.size.w * done) / total, bar.size.h - 2),
                     0, GCornerNone);
  const int label_bottom = bar.origin.y - 4;
#endif
  if (s_rx.count > 1 && s_rx.index > 0) {
    static char label[24];
    snprintf(label, sizeof(label), "%d/%d", s_rx.index, s_rx.count);
    const GRect box = GRect(bounds.size.w / 2 - 22, label_bottom - 22, 44, 22);
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, box, 4, GCornersAll);
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, label, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                       GRect(box.origin.x, box.origin.y - 3, box.size.w, box.size.h),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  }
}

static void prv_image_update_proc(Layer *layer, GContext *ctx) {
  if (!s_bitmap) {
    return;
  }
  const GRect bounds = layer_get_bounds(layer);
  graphics_context_set_compositing_mode(ctx, GCompOpAssign);
  graphics_draw_bitmap_in_rect(ctx, s_bitmap, bounds);
  graphics_context_set_compositing_mode(ctx, GCompOpSet);
  prv_draw_progress(ctx, bounds);
}

static void prv_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  const GRect bounds = layer_get_bounds(root);

  s_image_layer = layer_create(bounds);
  layer_set_update_proc(s_image_layer, prv_image_update_proc);
  layer_add_child(root, s_image_layer);

  const GRect hint = grect_inset(bounds, GEdgeInsets(bounds.size.h / 2 - 30, 20, 0, 20));
  s_hint_layer = text_layer_create(hint);
  text_layer_set_background_color(s_hint_layer, GColorClear);
  text_layer_set_text_color(s_hint_layer, GColorWhite);
  text_layer_set_font(s_hint_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_hint_layer, GTextAlignmentCenter);
  text_layer_set_text(s_hint_layer, "Choose an image in the app settings");
  layer_add_child(root, text_layer_get_layer(s_hint_layer));
}

static void prv_window_unload(Window *window) {
  text_layer_destroy(s_hint_layer);
  layer_destroy(s_image_layer);
}

static void prv_init(void) {
  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_click_config_provider(s_window, prv_click_config);
  window_set_window_handlers(s_window, (WindowHandlers){
                                           .load = prv_window_load,
                                           .unload = prv_window_unload,
                                       });
  window_stack_push(s_window, false);

  for (int slot = 0; slot < SLOT_COUNT; slot++) {
    ImageMeta meta;
    if (prv_read_meta(slot, &meta)) {
      s_filled |= 1 << slot;
    }
  }
  if (persist_exists(CURRENT_KEY)) {
    s_saved_current = persist_read_int(CURRENT_KEY);
  }
  prv_show_from(prv_valid_slot(s_saved_current) ? s_saved_current : 0, 1);

  app_message_register_inbox_received(prv_inbox_received);
  const uint32_t inbox = app_message_inbox_size_maximum();
  app_message_open(inbox < INBOX_SIZE_CAP ? inbox : INBOX_SIZE_CAP, OUTBOX_SIZE);
  connection_service_subscribe((ConnectionHandlers){
      .pebble_app_connection_handler = prv_connection_changed,
  });
}

static void prv_deinit(void) {
  connection_service_unsubscribe();
  // Only write when changed: every persist write adds a record that slows
  // later persist calls until the firmware compacts the file.
  if (s_current >= 0 && s_current != s_saved_current) {
    persist_write_int(CURRENT_KEY, s_current);
  }
  window_destroy(s_window);
  if (s_bitmap) {
    gbitmap_destroy(s_bitmap);
  }
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
