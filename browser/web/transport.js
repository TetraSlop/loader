// The stream sockets' transport (host/host.h, Socket): one binary WebSocket per guest connection, in the page's worker
// and under Node alike, shaped for tmserver's gateway streams (docs/LOADER_AUTH_SPEC.md, "Browser clients"): the
// tm-stream-v1 subprotocol, the play-session token as the first text message, then raw stream bytes both ways.
// route(address, port) gives { url, token, origin } for a guest's address:port, or nothing for none. Until sign-in
// lands no route exists, so the default refuses every connect. origin is for Node only, whose WebSocket sends none
// unless told: the gateway takes a browser stream by its listed Origin. A page's WebSocket sends its own.

export const SocketEvent = { Opened: 0, Received: 1, Closed: 2, Failed: 3 };

// The gateway closes a stream it refuses with 1008 and a reason (invalid_token, invalid_session, channel_busy, ...)
const POLICY = 1008;

/**
 * @param {(address: string, port: number) => { url: string, token?: string } | undefined} route
 * @param {(line: string) => void} log
 */
export function webSocketTransport(route = () => undefined, log = () => {}) {
  /** @type {Map<number, WebSocket>} */
  const open = new Map();
  /** @type {(id: number, kind: number, bytes?: Uint8Array) => void} */
  let event = () => {};
  return {
    /** Where socket events go: the module's tmweb_socket_event (module.js). */
    listen: f => void (event = f),
    connect(id, address, port) {
      const to = route(address, port);
      // Asynchronous, as a WebSocket's own failure is: the guest sees the socket Connecting first
      if (!to) return void queueMicrotask(() => event(id, SocketEvent.Failed));
      const ws = to.origin ? new WebSocket(to.url, { protocols: ["tm-stream-v1"], headers: { Origin: to.origin } }) : new WebSocket(to.url, "tm-stream-v1");
      ws.binaryType = "arraybuffer";
      open.set(id, ws);
      ws.onopen = () => {
        // The server sends no acknowledgement; stream bytes may follow at once
        if (to.token) ws.send(to.token);
        event(id, SocketEvent.Opened);
      };
      ws.onmessage = ({ data }) => typeof data !== "string" && event(id, SocketEvent.Received, new Uint8Array(data));
      ws.onclose = ({ code, reason }) => {
        if (!open.delete(id)) return; // closed by the guest
        if (code === POLICY) log(`tmweb: ${address}:${port} refused: ${reason}`);
        event(id, code === 1000 ? SocketEvent.Closed : SocketEvent.Failed);
      };
    },
    send: (id, bytes) => open.get(id)?.send(bytes),
    close(id) {
      open.get(id)?.close();
      open.delete(id);
    },
  };
}
