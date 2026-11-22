// The keyboard and mouse on the canvas, for the DirectInput and Win32 stand-ins (host/host.h, InputKind). Taken from
// theater-fit's kestrel/port/run/input.ts (commit 658d8c2): the key map and the pointer's place on the picture. The
// worker turns that place into back-buffer pixels.

/** host::InputKind */
export const InputKind = { Key: 0, Move: 1, Button: 2, Wheel: 3 };

/** KeyboardEvent.code to DirectInput key code: where the key sits, whatever the layout prints on it. */
export const DIK = {
  Escape: 0x01,
  Minus: 0x0c,
  Equal: 0x0d,
  Backspace: 0x0e,
  Tab: 0x0f,
  BracketLeft: 0x1a,
  BracketRight: 0x1b,
  Enter: 0x1c,
  ControlLeft: 0x1d,
  Semicolon: 0x27,
  Quote: 0x28,
  Backquote: 0x29,
  ShiftLeft: 0x2a,
  Backslash: 0x2b,
  Comma: 0x33,
  Period: 0x34,
  Slash: 0x35,
  ShiftRight: 0x36,
  NumpadMultiply: 0x37,
  AltLeft: 0x38,
  Space: 0x39,
  CapsLock: 0x3a,
  F11: 0x57,
  F12: 0x58,
  Numpad7: 0x47,
  Numpad8: 0x48,
  Numpad9: 0x49,
  NumpadSubtract: 0x4a,
  Numpad4: 0x4b,
  Numpad5: 0x4c,
  Numpad6: 0x4d,
  NumpadAdd: 0x4e,
  Numpad1: 0x4f,
  Numpad2: 0x50,
  Numpad3: 0x51,
  Numpad0: 0x52,
  NumpadDecimal: 0x53,
  NumpadEnter: 0x9c,
  ControlRight: 0x9d,
  NumpadDivide: 0xb5,
  AltRight: 0xb8,
  Pause: 0xc5,
  Home: 0xc7,
  ArrowUp: 0xc8,
  PageUp: 0xc9,
  ArrowLeft: 0xcb,
  ArrowRight: 0xcd,
  End: 0xcf,
  ArrowDown: 0xd0,
  PageDown: 0xd1,
  Insert: 0xd2,
  Delete: 0xd3,
};
for (const [row, first] of [["1234567890", 0x02], ["QWERTYUIOP", 0x10], ["ASDFGHJKL", 0x1e], ["ZXCVBNM", 0x2c]]) {
  row.split("").forEach((c, i) => DIK[c <= "9" ? `Digit${c}` : `Key${c}`] = first + i);
}
for (let i = 0; i < 10; i++) DIK[`F${i + 1}`] = 0x3b + i;

/** Where a point of rect falls on a picture of aspect width / height drawn in it with object-fit: contain, 0 to 1. */
export function onPicture(rect, aspect, x, y) {
  const w = Math.min(rect.width, rect.height * aspect);
  const h = w / aspect;
  const clamp = v => Math.min(1, Math.max(0, v));
  return [clamp((x - rect.left - (rect.width - w) / 2) / w), clamp((y - rect.top - (rect.height - h) / 2) / h)];
}

/**
 * Sends the stage's keys, pointer and wheel as { type: "input", kind, a, b }, Move as 0 to 1 across and down the
 * picture. The stage must be focusable; keys go only while it has focus, and none reaches the page. aspect() is the
 * picture's width over its height. Returns the cleanup.
 */
export function forwardInput(stage, send, aspect) {
  const keys = new Set();
  const buttons = new Set();
  const input = (kind, a, b = 0) => send({ type: "input", kind, a, b });
  const button = (b, down) => {
    if (down === buttons.has(b)) return;
    if (down) buttons.add(b);
    else buttons.delete(b);
    input(InputKind.Button, b, down ? 1 : 0);
  };
  const key = down => e => {
    e.preventDefault();
    const k = DIK[e.code];
    if (k === undefined || e.repeat || down === keys.has(k)) return;
    if (down) keys.add(k);
    else keys.delete(k);
    input(InputKind.Key, k, down ? 1 : 0);
  };
  // A key or button let go elsewhere would stay down in the game
  const release = () => {
    for (const k of keys) input(InputKind.Key, k, 0);
    keys.clear();
    for (const b of [...buttons]) button(b, false);
  };
  // DOM buttons: 0 left, 1 middle, 2 right. The game's: 0 left, 1 right, 2 middle.
  const gameButton = b => [0, 2, 1][b];
  const move = e => input(InputKind.Move, ...onPicture(stage.getBoundingClientRect(), aspect(), e.clientX, e.clientY));
  const down = e => {
    stage.focus();
    if (gameButton(e.button) === undefined) return;
    stage.setPointerCapture(e.pointerId);
    // The middle button would start the browser's autoscroll
    e.preventDefault();
    move(e);
    button(gameButton(e.button), true);
  };
  const up = e => gameButton(e.button) !== undefined && button(gameButton(e.button), false);
  const wheel = e => {
    if (e.deltaY === 0) return;
    e.preventDefault();
    input(InputKind.Wheel, -Math.sign(e.deltaY) * 120);
  };
  const listeners = [
    ["keydown", key(true)],
    ["keyup", key(false)],
    ["blur", release],
    ["pointermove", move],
    ["pointerdown", down],
    ["pointerup", up],
    ["pointercancel", release],
    ["wheel", wheel],
    ["contextmenu", e => e.preventDefault()],
  ];
  const off = new AbortController();
  for (const [type, f] of listeners) stage.addEventListener(type, f, { passive: false, signal: off.signal });
  return () => {
    release();
    off.abort();
  };
}
