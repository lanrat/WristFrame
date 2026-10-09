#include <pebble.h>

// Image wire/storage format (produced by config/index.html on the phone):
// 64-colour pixels, 6 bits each (rrggbb), packed MSB-first, row-major.
// On round displays only the pixels inside the visible circle are included
// (see prv_row_span, mirrored in config/index.html).

#define META_KEY 1
#define DATA_KEY_BASE 100
#define META_VERSION 1
#define RECORD_SIZE PERSIST_DATA_MAX_LENGTH
#define INBOX_SIZE_CAP (2048 + 64)
#define OUTBOX_SIZE 64

enum {
  CMD_HELLO = 1,  // phone -> watch: request READY
  CMD_READY = 2,  // watch -> phone: screen info + chunk size
  CMD_BEGIN = 3,  // phone -> watch: new image header
  CMD_CHUNK = 4,  // phone -> watch: image data
  CMD_END = 5,    // phone -> watch: transfer complete
  CMD_DONE = 6,   // watch -> phone: result of transfer, sent once per END
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

static struct {
  bool active;
  bool persist;
  ImageMeta meta;
  uint32_t offset;
  uint32_t checksum;
} s_rx;

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
  prv_show_image(false);
}

// --- Persistent storage ------------------------------------------------------

static bool prv_meta_valid(const ImageMeta *meta) {
  const GRect bounds = layer_get_bounds(window_get_root_layer(s_window));
  return meta->version == META_VERSION && meta->width == bounds.size.w &&
         meta->height == bounds.size.h && meta->round == PBL_IF_ROUND_ELSE(1, 0) &&
         meta->size == prv_packed_size(meta->width, meta->height, meta->round);
}

static void prv_load_image(void) {
  ImageMeta meta;
  if (persist_read_data(META_KEY, &meta, sizeof(meta)) != sizeof(meta) || !prv_meta_valid(&meta)) {
    return;
  }
  if (!prv_bitmap_create(meta.width, meta.height, meta.round)) {
    return;
  }
  uint8_t buf[RECORD_SIZE];
  uint32_t checksum = FNV_OFFSET;
  for (uint32_t offset = 0; offset < meta.size; offset += RECORD_SIZE) {
    const uint32_t len = meta.size - offset < RECORD_SIZE ? meta.size - offset : RECORD_SIZE;
    if (persist_read_data(DATA_KEY_BASE + offset / RECORD_SIZE, buf, len) != (int)len) {
      APP_LOG(APP_LOG_LEVEL_WARNING, "Stored image is incomplete");
      prv_discard_image();
      return;
    }
    checksum = prv_fnv1a(checksum, buf, len);
    prv_decoder_feed(&s_decoder, buf, len);
  }
  if (checksum != meta.checksum) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "Stored image checksum mismatch");
    prv_discard_image();
    return;
  }
  prv_show_image(true);
}

// --- AppMessage --------------------------------------------------------------

static void prv_send(uint8_t cmd, int32_t status) {
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
  } else {
    dict_write_int32(iter, MESSAGE_KEY_Status, status);
  }
  app_message_outbox_send();
}

static int32_t prv_int(DictionaryIterator *iter, uint32_t key) {
  Tuple *t = dict_find(iter, key);
  return t ? t->value->int32 : -1;
}

static void prv_handle_begin(DictionaryIterator *iter) {
  ImageMeta meta = {
    .version = META_VERSION,
    .width = prv_int(iter, MESSAGE_KEY_Width),
    .height = prv_int(iter, MESSAGE_KEY_Height),
    .round = prv_int(iter, MESSAGE_KEY_Round),
    .size = prv_int(iter, MESSAGE_KEY_Size),
    .checksum = prv_int(iter, MESSAGE_KEY_Checksum),
  };
  s_rx.active = false;
  if (!prv_meta_valid(&meta)) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Rejected image %dx%d size %d", meta.width, meta.height,
            (int)meta.size);
    return;
  }
  if (!prv_bitmap_create(meta.width, meta.height, meta.round)) {
    prv_show_image(false);
    return;
  }
  // Invalidate the stored image until the new one is complete.
  if (persist_exists(META_KEY)) {
    persist_delete(META_KEY);
  }
  s_rx.active = true;
  s_rx.persist = persist_get_max_size() >= meta.size + 1024;
  s_rx.meta = meta;
  s_rx.offset = 0;
  s_rx.checksum = FNV_OFFSET;
  if (!s_rx.persist) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "Not enough persistent storage to remember the image");
  }
  prv_show_image(true);
}

static void prv_handle_chunk(DictionaryIterator *iter) {
  Tuple *data = dict_find(iter, MESSAGE_KEY_Data);
  if (!s_rx.active || !data || prv_int(iter, MESSAGE_KEY_Offset) != (int32_t)s_rx.offset ||
      s_rx.offset + data->length > s_rx.meta.size) {
    s_rx.active = false;
    return;
  }
  const uint8_t *buf = data->value->data;
  const uint16_t len = data->length;
  if (s_rx.persist) {
    for (uint16_t i = 0; i < len; i += RECORD_SIZE) {
      const uint16_t n = len - i < RECORD_SIZE ? len - i : RECORD_SIZE;
      const uint32_t key = DATA_KEY_BASE + (s_rx.offset + i) / RECORD_SIZE;
      if (persist_write_data(key, buf + i, n) != n) {
        APP_LOG(APP_LOG_LEVEL_WARNING, "Persist write failed");
        s_rx.persist = false;
        break;
      }
    }
  }
  s_rx.checksum = prv_fnv1a(s_rx.checksum, buf, len);
  prv_decoder_feed(&s_decoder, buf, len);
  s_rx.offset += len;
  layer_mark_dirty(s_image_layer);
}

static void prv_handle_end(void) {
  const bool ok = s_rx.active && s_rx.offset == s_rx.meta.size &&
                  s_rx.checksum == s_rx.meta.checksum;
  s_rx.active = false;
  if (!ok) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Image transfer failed");
    prv_send(CMD_DONE, 0);
    return;
  }
  if (s_rx.persist) {
    persist_write_data(META_KEY, &s_rx.meta, sizeof(s_rx.meta));
  }
  prv_send(CMD_DONE, 1);
}

static void prv_inbox_received(DictionaryIterator *iter, void *context) {
  switch (prv_int(iter, MESSAGE_KEY_Cmd)) {
    case CMD_HELLO:
      prv_send(CMD_READY, 0);
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
  }
}

// --- UI ----------------------------------------------------------------------

static void prv_image_update_proc(Layer *layer, GContext *ctx) {
  if (!s_bitmap) {
    return;
  }
  graphics_context_set_compositing_mode(ctx, GCompOpAssign);
  graphics_draw_bitmap_in_rect(ctx, s_bitmap, layer_get_bounds(layer));
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
  window_set_window_handlers(s_window, (WindowHandlers){
                                           .load = prv_window_load,
                                           .unload = prv_window_unload,
                                       });
  window_stack_push(s_window, false);

  prv_load_image();
  prv_show_image(s_bitmap != NULL);

  app_message_register_inbox_received(prv_inbox_received);
  const uint32_t inbox = app_message_inbox_size_maximum();
  app_message_open(inbox < INBOX_SIZE_CAP ? inbox : INBOX_SIZE_CAP, OUTBOX_SIZE);
}

static void prv_deinit(void) {
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
