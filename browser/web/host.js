// host/host.h's outward calls for Emscripten's --js-library. The host passes onLog and transport to the module
// factory (module.js).
addToLibrary({
  tmweb_host_log__deps: ["$UTF8ToString"],
  tmweb_host_log: (text, length) => Module["onLog"](UTF8ToString(text, length)),
  tmweb_host_connect__deps: ["$UTF8ToString"],
  tmweb_host_connect: (id, address, port) => Module["transport"].connect(id, UTF8ToString(address), port),
  tmweb_host_send: (id, bytes, length) => Module["transport"].send(id, HEAPU8.slice(bytes, bytes + length)),
  tmweb_host_close: id => Module["transport"].close(id),
});
