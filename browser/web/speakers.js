// The game's sound on the page: the worker takes what the DirectSound stand-in mixed (tmweb_sound_take) and posts it
// in chunks, and an AudioWorklet plays them from a short queue. No SharedArrayBuffer, so the page needs no cross-origin
// isolation. Taken from theater-fit's kestrel/port/run/speakers.ts (commit 658d8c2).
import { SOUND_RATE } from "./module.js";

// Frames queued before playing starts, which the queue keeps near: enough to ride out a slow slice of the interpreter.
// The most it holds is a second.
const TARGET = SOUND_RATE * 0.1;
const SIZE = SOUND_RATE;

/**
 * The frames the worklet plays from, stereo frames in a ring of size, apart from it so a test can run it. It starts
 * once it holds target frames. Off target, it plays up to 1% fast or slow until it is back near it: a slow slice of
 * the interpreter, or the two clocks drifting apart, then neither cut the sound nor skip it. Run dry, it fades out rather than cut, and fades back in once it holds target
 * frames again.
 * The worklet's source holds it as text (toString), so it reads nothing outside itself.
 */
export class SoundQueue {
  read = 0; // frames played since the start, and the fraction of the next: a rate off 1 plays between frames
  written = 0;
  playing = false;
  level = 0; // queued, smoothed over the chunks' coming and going
  gain = 0;
  left = 0;
  right = 0;

  constructor(target, size) {
    this.target = target;
    this.size = size;
    this.ring = new Float32Array(2 * size);
  }

  get queued() {
    return this.written - this.read;
  }

  push(samples) {
    const frames = samples.length / 2;
    for (let i = Math.max(0, frames - this.size); i < frames; i++) {
      const at = (this.written + i) % this.size * 2;
      this.ring[at] = samples[2 * i];
      this.ring[at + 1] = samples[2 * i + 1];
    }
    this.written += frames;
    // A burst past the ring loses its oldest frames
    this.read = Math.max(this.read, this.written - this.size + 1);
  }

  pull(left, right) {
    if (!this.playing && this.queued >= this.target) {
      this.playing = true;
      this.level = this.queued;
    }
    this.level += (this.queued - this.level) / 50;
    // A tenth of the target either side plays at 1; three tenths off plays 1% off.
    const off = Math.abs(this.level - this.target) - this.target / 10;
    const rate = 1 + Math.sign(this.level - this.target) * Math.min(0.01, Math.max(0, off) / (20 * this.target));
    // 5 ms of fade at 48 kHz
    const fade = 1 / 240;
    for (let i = 0; i < left.length; i++) {
      // Between two frames, so the next must be there too
      if (this.queued < 2) this.playing = false;
      if (this.playing) {
        const at = Math.floor(this.read), x = this.read - at;
        const a = at % this.size * 2, b = (at + 1) % this.size * 2;
        this.left = this.ring[a] + (this.ring[b] - this.ring[a]) * x;
        this.right = this.ring[a + 1] + (this.ring[b + 1] - this.ring[a + 1]) * x;
        this.read += rate;
        this.gain = Math.min(1, this.gain + fade);
      } else {
        this.gain = Math.max(0, this.gain - fade);
      }
      left[i] = this.left * this.gain;
      right[i] = this.right * this.gain;
    }
  }
}

const PROCESSOR = `
const Queue = ${SoundQueue.toString()};
class Speakers extends AudioWorkletProcessor {
  queue = new Queue(${TARGET}, ${SIZE});
  constructor() {
    super();
    this.port.onmessage = ({ data }) => this.queue.push(data);
  }
  process(_, [[left, right]]) {
    this.queue.pull(left, right);
    return true;
  }
}
registerProcessor("tmweb-speakers", Speakers);
`;

/** Plays the worker's sound chunks. Make it in a click: browsers start audio only after one. */
export function createSpeakers() {
  const context = new AudioContext({ sampleRate: SOUND_RATE, latencyHint: "interactive" });
  const url = URL.createObjectURL(new Blob([PROCESSOR], { type: "text/javascript" }));
  const node = context.audioWorklet.addModule(url).then(() => {
    URL.revokeObjectURL(url);
    const n = new AudioWorkletNode(context, "tmweb-speakers", { numberOfInputs: 0, outputChannelCount: [2] });
    n.connect(context.destination);
    return n;
  });
  let ready;
  void node.then(n => (ready = n));
  return {
    // Chunks that come before the worklet is up are dropped: they would only add latency
    play: samples => ready?.port.postMessage(samples, [samples.buffer]),
    resume: () => context.resume(),
    close: () => context.close(),
  };
}
