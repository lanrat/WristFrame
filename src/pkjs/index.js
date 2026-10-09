// Phone-side transfer of converted images to the watch.
//
// The config page (config/index.html) does all image processing: it crops,
// resizes, dithers to the 64-colour palette and packs the pixels. This file
// only moves those bytes to the watch and retries interrupted transfers.

var CONFIG_URL = 'https://lanrat.github.io/pebble-image/';

var CMD_HELLO = 1;
var CMD_READY = 2;
var CMD_BEGIN = 3;
var CMD_CHUNK = 4;
var CMD_END = 5;
var CMD_DONE = 6;

var MAX_RETRIES = 3;

// Screen info for supported platforms, used before the watch has reported in.
var PLATFORMS = {
  emery: { width: 200, height: 228, round: 0 },
  gabbro: { width: 260, height: 260, round: 1 }
};

var watchInfo = null;
var sending = false;

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

function loadPending() {
  try {
    var raw = localStorage.getItem('pending');
    return raw ? JSON.parse(raw) : null;
  } catch (e) {
    return null;
  }
}

function sendPending() {
  var image = loadPending();
  if (!image || !watchInfo || sending) {
    return;
  }
  if (image.width !== watchInfo.width || image.height !== watchInfo.height ||
      image.round !== watchInfo.round) {
    console.log('Pending image does not match this watch, discarding');
    localStorage.removeItem('pending');
    return;
  }

  var bytes = base64Decode(image.data);
  var chunkSize = watchInfo.chunkSize;
  var messages = [{
    Cmd: CMD_BEGIN,
    Width: image.width,
    Height: image.height,
    Round: image.round,
    Size: bytes.length,
    Checksum: fnv1a(bytes)
  }];
  for (var offset = 0; offset < bytes.length; offset += chunkSize) {
    messages.push({
      Cmd: CMD_CHUNK,
      Offset: offset,
      Data: bytes.slice(offset, offset + chunkSize)
    });
  }
  messages.push({ Cmd: CMD_END });

  var start = Date.now();
  sending = image.data;
  console.log('Sending ' + bytes.length + ' bytes in ' + chunkSize + ' byte chunks');
  sendAll(messages, function(err) {
    if (err) {
      console.log('Image transfer failed: ' + JSON.stringify(err));
    } else {
      console.log('Image sent in ' + (Date.now() - start) + ' ms');
    }
    // On success the watch answers END with DONE, which clears `sending`.
    if (err) {
      sending = false;
    } else {
      setTimeout(function() {
        if (sending === image.data) {
          sending = false;
        }
      }, 10000);
    }
  });
}

Pebble.addEventListener('ready', function() {
  send({ Cmd: CMD_HELLO }, MAX_RETRIES);
});

Pebble.addEventListener('appmessage', function(e) {
  var msg = e.payload;
  if (msg.Cmd === CMD_READY) {
    watchInfo = {
      width: msg.Width,
      height: msg.Height,
      round: msg.Round,
      chunkSize: msg.ChunkSize
    };
    sendPending();
  } else if (msg.Cmd === CMD_DONE) {
    var pending = loadPending();
    if (msg.Status === 1) {
      console.log('Watch stored image');
      if (pending && pending.data === sending) {
        localStorage.removeItem('pending');
      }
    } else {
      console.log('Watch rejected image');
    }
    // Send an image chosen while this transfer was in progress.
    var retry = sending && pending && pending.data !== sending;
    sending = false;
    if (retry) {
      sendPending();
    }
  }
});

Pebble.addEventListener('showConfiguration', function() {
  var info = screenInfo();
  Pebble.openURL(CONFIG_URL + '?width=' + info.width + '&height=' + info.height +
                 '&round=' + info.round);
});

Pebble.addEventListener('webviewclosed', function(e) {
  if (!e || !e.response) {
    return;
  }
  var image;
  try {
    image = JSON.parse(decodeURIComponent(e.response));
  } catch (err) {
    console.log('Invalid config response: ' + err);
    return;
  }
  if (!image || typeof image.data !== 'string') {
    return;
  }
  localStorage.setItem('pending', JSON.stringify({
    width: image.width,
    height: image.height,
    round: image.round,
    data: image.data
  }));
  if (watchInfo) {
    sendPending();
  } else {
    send({ Cmd: CMD_HELLO }, MAX_RETRIES);
  }
});
