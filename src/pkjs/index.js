// Phone-side transfer of converted images to the watch.
//
// The config page (config/index.html) does all image processing: it crops,
// resizes, dithers to the 64-colour palette and packs the pixels. This file
// only moves those bytes to the watch's image slots, keeps a queue of
// operations that have not reached the watch yet, and remembers a thumbnail
// per slot for the config page.

var CONFIG_URL = 'https://lanrat.github.io/WristFrame/';

var CMD_HELLO = 1;
var CMD_READY = 2;
var CMD_BEGIN = 3;
var CMD_CHUNK = 4;
var CMD_END = 5;
var CMD_DONE = 6;
var CMD_CLEAR = 7;

var SLOT_COUNT = 4;

var MAX_RETRIES = 3;
// Operations the watch rejects this many times are dropped.
var MAX_ATTEMPTS = 3;

// Screen info for supported platforms, used before the watch has reported in.
var PLATFORMS = {
  emery: { width: 200, height: 228, round: 0 },
  gabbro: { width: 260, height: 260, round: 1 }
};

var watchInfo = null;
// id of the queued operation being sent, or null.
var busy = null;
// Images the watch has finished since the queue was last empty, so it can
// show "2/3" while a batch arrives.
var batchDone = 0;

// Tells the config page this version accepts several changes per save.
var CONFIG_PROTOCOL = 2;

function screenInfo() {
  if (watchInfo) {
    return watchInfo;
  }
  var platform = 'emery';
  try {
    platform = Pebble.getActiveWatchInfo().platform;
  } catch (e) {
    console.log('getActiveWatchInfo unavailable: ' + e);
  }
  return PLATFORMS[platform] || PLATFORMS.emery;
}

function base64Decode(str) {
  var chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  var lookup = {};
  for (var i = 0; i < chars.length; i++) {
    lookup[chars.charAt(i)] = i;
  }
  // The config page sends URL-safe base64.
  lookup['-'] = 62;
  lookup['_'] = 63;
  var out = [];
  var acc = 0;
  var bits = 0;
  for (var j = 0; j < str.length; j++) {
    var v = lookup[str.charAt(j)];
    if (v === undefined) {
      continue;
    }
    acc = ((acc << 6) | v) & 0xffffff;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push((acc >> bits) & 0xff);
    }
  }
  return out;
}

function fnv1a(bytes) {
  var hash = 0x811c9dc5;
  for (var i = 0; i < bytes.length; i++) {
    hash ^= bytes[i];
    hash = Math.imul ? Math.imul(hash, 16777619) : (hash * 16777619) & 0xffffffff;
  }
  // Pebble int32 tuple
  return hash | 0;
}

// Sends dict, retrying on failure, then calls done(error).
function send(dict, retries, done) {
  Pebble.sendAppMessage(dict, function() {
    if (done) {
      done(null);
    }
  }, function(e) {
    if (retries > 0) {
      setTimeout(function() {
        send(dict, retries - 1, done);
      }, 500);
    } else if (done) {
      done(e || 'send failed');
    }
  });
}

// Sends each message in order, stopping at the first failure.
function sendAll(messages, done) {
  var i = 0;
  (function next(err) {
    if (err || i >= messages.length) {
      done(err);
      return;
    }
    send(messages[i++], MAX_RETRIES, next);
  })(null);
}

function load(key, fallback) {
  try {
    var raw = localStorage.getItem(key);
    return raw ? JSON.parse(raw) : fallback;
  } catch (e) {
    return fallback;
  }
}

function save(key, value) {
  localStorage.setItem(key, JSON.stringify(value));
}

// Queue of {id, op: 'set' | 'clear', slot, attempts, ...image} not yet
// confirmed by the watch.
function loadQueue() {
  var queue = load('queue', []);
  // Single-image versions stored one pending image under 'pending'.
  var legacy = load('pending', null);
  if (legacy) {
    localStorage.removeItem('pending');
    legacy.op = 'set';
    legacy.slot = 0;
    legacy.id = 'legacy';
    legacy.attempts = 0;
    queue.push(legacy);
    save('queue', queue);
  }
  return queue;
}

function enqueue(op) {
  op.id = Date.now() + '-' + Math.random();
  op.attempts = 0;
  // A newer operation for a slot replaces any older one that is not already
  // being sent.
  var queue = loadQueue().filter(function(o) {
    return o.slot !== op.slot || o.id === busy;
  });
  queue.push(op);
  save('queue', queue);

  var thumbs = load('thumbs', {});
  if (op.op === 'set' && op.thumb) {
    thumbs[op.slot] = op.thumb;
  } else {
    delete thumbs[op.slot];
  }
  save('thumbs', thumbs);
}

function buildMessages(op, queue) {
  if (op.op === 'clear') {
    return [{ Cmd: CMD_CLEAR, Slot: op.slot }];
  }
  var bytes = base64Decode(op.data);
  var chunkSize = watchInfo.chunkSize;
  var messages = [{
    Cmd: CMD_BEGIN,
    Slot: op.slot,
    Width: op.width,
    Height: op.height,
    Round: op.round,
    Size: bytes.length,
    Checksum: fnv1a(bytes),
    Index: batchDone + 1,
    Count: batchDone + queue.filter(function(o) { return o.op === 'set'; }).length
  }];
  for (var offset = 0; offset < bytes.length; offset += chunkSize) {
    messages.push({
      Cmd: CMD_CHUNK,
      Offset: offset,
      Data: bytes.slice(offset, offset + chunkSize)
    });
  }
  messages.push({ Cmd: CMD_END });
  return messages;
}

// Sends the next queued operation. The watch answers each one with DONE,
// which removes it from the queue and continues with the next.
function processQueue() {
  if (!watchInfo || busy) {
    return;
  }
  var queue = loadQueue();
  var op = queue[0];
  if (!op) {
    return;
  }
  if (op.op === 'set' && (op.width !== watchInfo.width || op.height !== watchInfo.height ||
                          op.round !== watchInfo.round)) {
    console.log('Image for slot ' + op.slot + ' does not match this watch, discarding');
    save('queue', queue.slice(1));
    processQueue();
    return;
  }

  var id = op.id;
  var start = Date.now();
  busy = id;
  console.log('Sending ' + op.op + ' for slot ' + op.slot);
  sendAll(buildMessages(op, queue), function(err) {
    if (err) {
      // Usually a lost connection; retried the next time the app opens.
      console.log('Transfer failed: ' + JSON.stringify(err));
      busy = null;
      batchDone = 0;
      return;
    }
    console.log('Sent in ' + (Date.now() - start) + ' ms');
    setTimeout(function() {
      if (busy === id) {
        busy = null;
      }
    }, 10000);
  });
}

function handleDone(msg) {
  var queue = loadQueue();
  var op = queue[0];
  if (!busy || !op || op.id !== busy || op.slot !== msg.Slot) {
    return;
  }
  busy = null;
  var slots = load('slots', 0);
  if (msg.Status === 1) {
    console.log('Watch finished ' + op.op + ' for slot ' + op.slot);
    queue.shift();
    if (!queue.length) {
      batchDone = 0;
    } else if (op.op === 'set') {
      batchDone++;
    }
    slots = op.op === 'set' ? slots | (1 << op.slot) : slots & ~(1 << op.slot);
    save('slots', slots);
    save('queue', queue);
    processQueue();
  } else {
    // Leave it for the next launch rather than retrying in a loop.
    batchDone = 0;
    op.attempts++;
    console.log('Watch rejected ' + op.op + ' for slot ' + op.slot);
    if (op.attempts >= MAX_ATTEMPTS) {
      queue.shift();
    }
    save('queue', queue);
  }
}

function handleReady(msg) {
  watchInfo = {
    width: msg.Width,
    height: msg.Height,
    round: msg.Round,
    chunkSize: msg.ChunkSize
  };
  save('slots', msg.Slots);
  // Forget thumbnails of slots the watch no longer has, unless an image for
  // that slot is still on its way.
  var queue = loadQueue();
  var thumbs = load('thumbs', {});
  for (var slot = 0; slot < SLOT_COUNT; slot++) {
    var queued = queue.some(function(o) { return o.slot === slot && o.op === 'set'; });
    if (!(msg.Slots & (1 << slot)) && !queued) {
      delete thumbs[slot];
    }
  }
  save('thumbs', thumbs);
  processQueue();
}

// What the config page shows for each slot: queued operations win over the
// last state the watch reported.
function slotSummary() {
  var slots = load('slots', 0);
  var thumbs = load('thumbs', {});
  var queue = loadQueue();
  var summary = [];
  for (var slot = 0; slot < SLOT_COUNT; slot++) {
    var filled = !!(slots & (1 << slot)) || !!thumbs[slot];
    queue.forEach(function(o) {
      if (o.slot === slot) {
        filled = o.op === 'set';
      }
    });
    summary.push({ filled: filled, thumb: filled ? thumbs[slot] || null : null });
  }
  return summary;
}

Pebble.addEventListener('ready', function() {
  send({ Cmd: CMD_HELLO }, MAX_RETRIES);
});

Pebble.addEventListener('appmessage', function(e) {
  var msg = e.payload;
  if (msg.Cmd === CMD_READY) {
    handleReady(msg);
  } else if (msg.Cmd === CMD_DONE) {
    handleDone(msg);
  }
});

Pebble.addEventListener('showConfiguration', function() {
  var info = screenInfo();
  Pebble.openURL(CONFIG_URL + '?width=' + info.width + '&height=' + info.height +
                 '&round=' + info.round + '&v=' + CONFIG_PROTOCOL + '#' +
                 encodeURIComponent(JSON.stringify({ slots: slotSummary() })));
});

Pebble.addEventListener('webviewclosed', function(e) {
  if (!e || !e.response) {
    return;
  }
  var result;
  try {
    result = JSON.parse(decodeURIComponent(e.response));
  } catch (err) {
    console.log('Invalid config response: ' + err);
    return;
  }
  // {ops: [...]} from this version's page; a bare op from older pages.
  var ops = result && Array.isArray(result.ops) ? result.ops : [result];
  var queued = 0;
  ops.forEach(function(op) {
    if (!op || typeof op.slot !== 'number' || op.slot < 0 || op.slot >= SLOT_COUNT) {
      return;
    }
    if (op.op === 'clear') {
      enqueue({ op: 'clear', slot: op.slot });
    } else if (op.op === 'set' && typeof op.data === 'string') {
      enqueue({
        op: 'set',
        slot: op.slot,
        width: op.width,
        height: op.height,
        round: op.round,
        data: op.data,
        thumb: typeof op.thumb === 'string' ? op.thumb : null
      });
    } else {
      return;
    }
    queued++;
  });
  if (!queued) {
    return;
  }
  if (watchInfo) {
    processQueue();
  } else {
    send({ Cmd: CMD_HELLO }, MAX_RETRIES);
  }
});
