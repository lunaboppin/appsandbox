// Plays the VM's sound. Samples arrive as Int16Array messages. The buffer stays short:
// when it runs past maxBuffered the oldest sound is dropped, so sound never trails
// the picture for long.
class RemoteAudio extends AudioWorkletProcessor {
  constructor(options) {
    super();
    this.channels = options.processorOptions.channels;
    this.size = Math.ceil(sampleRate) * this.channels;
    this.buffer = new Float32Array(this.size);
    this.read = 0;
    this.count = 0;
    this.startAt = Math.ceil(sampleRate * 0.04) * this.channels;
    this.maxBuffered = Math.ceil(sampleRate * 0.25) * this.channels;
    this.playing = false;
    this.port.onmessage = (e) => this.push(e.data);
  }

  push(samples) {
    let write = (this.read + this.count) % this.size;
    for (let i = 0; i < samples.length; i++) {
      this.buffer[write] = samples[i] / 32768;
      write = (write + 1) % this.size;
    }
    this.count += samples.length;
    if (this.count > this.maxBuffered) {
      const drop = this.count - this.startAt;
      this.read = (this.read + drop) % this.size;
      this.count -= drop;
    }
  }

  process(inputs, outputs) {
    const out = outputs[0];
    const frames = out[0].length;
    if (!this.playing && this.count >= this.startAt) this.playing = true;
    if (this.playing && this.count < frames * this.channels) {
      this.playing = false;
    }
    if (!this.playing) return true;
    for (let f = 0; f < frames; f++) {
      for (let c = 0; c < this.channels; c++) {
        const sample = this.buffer[this.read];
        this.read = (this.read + 1) % this.size;
        if (out[c]) out[c][f] = sample;
      }
      if (this.channels === 1) {
        for (let c = 1; c < out.length; c++) out[c][f] = out[0][f];
      }
    }
    this.count -= frames * this.channels;
    return true;
  }
}

registerProcessor('remote-audio', RemoteAudio);
