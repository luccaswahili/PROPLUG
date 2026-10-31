// Live sliders: a drag sends many values, and each write to Premiere is a transaction (and a rebuild of
// the match for the grade sliders), so moves are coalesced into at most one apply per `interval` ms. The
// newest value of every control always lands, also when the drag ends between two applies.

class Live {
  constructor(apply, { interval = 300, onError, timers } = {}) {
    this.apply = apply;          // async (params) => any
    this.interval = interval;
    this.onError = onError || (() => {});
    this.timers = timers || { setTimeout: (f, ms) => setTimeout(f, ms), now: () => Date.now() };
    this.pending = {};
    this.timer = null;
    this.running = null;
    this.last = -Infinity;
  }

  set(name, value) {
    this.pending[name] = value;
    this.schedule();
  }

  schedule() {
    if (this.timer || this.running || !Object.keys(this.pending).length) return;
    const wait = Math.max(0, this.last + this.interval - this.timers.now());
    this.timer = this.timers.setTimeout(() => { this.timer = null; this.run(); }, wait);
  }

  run() {
    const batch = this.pending;
    if (!Object.keys(batch).length) return Promise.resolve();
    this.pending = {};
    this.last = this.timers.now();
    this.running = Promise.resolve()
      .then(() => this.apply(batch))
      .catch((e) => this.onError(e))
      .then(() => { this.running = null; this.schedule(); });
    return this.running;
  }

  // Drops values not yet sent (a new match resets every control).
  cancel() { this.pending = {}; }

  // Resolves once everything set so far has been applied.
  async idle() {
    while (this.timer || this.running || Object.keys(this.pending).length) {
      if (this.running) await this.running;
      else await new Promise((r) => this.timers.setTimeout(r, 5));
    }
  }
}

module.exports = { Live };
