# Devinera

Devinera is an 8 voice polyphonic synth with chorus, delay and reverb, played from a MIDI keyboard through the MIDI In jack. It is designed for the Arturia MiniLab 3. It combines the polyphony and chorus of the Juno with the waveforms, filter and envelopes of the Minimoog, and the built-in effects of the Novation Peak. The pedal has the live and effect controls. The MiniLab has the sound design controls. The audio input is not used.

## Voice

Each voice: OSC 1 + OSC 2 (the six Minimoog waveforms, anti-aliased, DaisySP `VariableShapeOscillator`) + sub oscillator (square, one octave down) + noise → 4-pole ladder filter → amp envelope. The filter has its own envelope. One global LFO can modulate the pitch, the filter and the pulse width.

The filter and the envelopes are set up like a Moog:
- The filter is a zero-delay-feedback Moog ladder (`tptladder.h`, after Zavalishin, "The Art of VA Filter Design"). It sounds like the DaisySP `LadderFilter` but uses about 4× less CPU (measured on a computer).
- The oscillators drive the ladder filter a little (input drive 1.0) for a warmer, thicker sound.
- More resonance gives some bass loss (passband gain 0.25), like a real Moog ladder.
- Key tracking is 2/3, like the Minimoog.
- The envelopes are snappy: the attack rises almost linearly (analog RC curve), and the fader times are the real segment times.

## Pedal controls

| Control | Function |
| --- | --- |
| Ctrl 1 | Volume |
| Ctrl 2 | Filter cutoff (20 Hz – 18 kHz) |
| Ctrl 3 | Resonance. Near the top the filter self-oscillates (a sine tone at the cutoff frequency): watch the volume. |
| Ctrl 4 | Delay time (50 ms – 1.5 s) |
| Ctrl 5 | Delay mix. The feedback goes up with the mix (max 75%). |
| Ctrl 6 | Reverb mix |
| 3-Way Switch 1 | Filter: left=High-pass, center=Low-pass 24 dB, right=Band-pass |
| 3-Way Switch 2 | Chorus: left=Off, center=I, right=II (Juno style) |
| 3-Way Switch 3 | Voice mode: left=Mono (legato), center=Poly (8 voices), right=Unison (2 voices on one note, detuned ±7 cents, one fully left and one fully right) |
| Dip Switch 1 | Long reverb |
| Dip Switch 2 | Ping-pong delay |
| Dip Switch 3 | Velocity off (all notes at full level) |
| Dip Switch 4 | Glide (100 ms) |
| FS 1 | Hold: notes stay on after you release the keys, like a sustain pedal. Release Hold to stop them. |
| FS 2 | Effects on/off (chorus, delay, reverb) |
| LED 1 | On in Hold. Otherwise flashes on each note. |
| LED 2 | Effects on |
| Audio Out 1 | Stereo |

## MiniLab 3 controls

| Control | CC | Function |
| --- | --- | --- |
| Knob 1 | 20 | OSC 1 wave (Minimoog, six zones): triangle / shark-tooth / saw / square / wide pulse / narrow pulse |
| Knob 2 | 21 | OSC 2 wave (same six waves) |
| Knob 3 | 22 | OSC 2 pitch (−24 to +24 semitones) |
| Knob 4 | 23 | OSC 2 detune (−50 to +50 cents, center = 0) |
| Knob 5 | 24 | OSC 1 ↔ OSC 2 balance |
| Knob 6 | 25 | Sub oscillator level |
| Knob 7 | 26 | Filter envelope amount (0 – 5 octaves) |
| Knob 8 | 27 | LFO rate (0.05 – 20 Hz) |
| Faders 1–4 | 28–31 | Attack, Decay, Sustain, Release of the Amp envelope or the Filter envelope (see Pad 7) |
| Pad 1 | 102 (toggle) | LFO → pitch (±1 semitone) |
| Pad 2 | 103 (toggle) | LFO → filter (±2 octaves) |
| Pad 3 | 104 (toggle) | LFO → pulse width (square and pulse waves only, width × 0.2 – 1.8) |
| Pad 4 | 105 (toggle) | LFO wave: off = triangle, on = random |
| Pad 5 | 106 (toggle) | Noise on |
| Pad 6 | 107 (toggle) | Velocity → filter |
| Pad 7 | 108 (toggle) | Fader page: off = Amp envelope, on = Filter envelope |
| Pad 8 | 109 (gate) | Panic: releases all notes and Hold |
| Mod strip | 1 | LFO depth. At 0 the LFO has no effect. |
| Pitch strip | Pitch bend | ±2 semitones |
| Sustain pedal | 64 | Same as Hold |

Envelope times (Moog style): Attack 1 ms – 10 s, Decay and Release 2 ms – 10 s, with an exponential fader taper (fader at the center = 100 ms). The times are the real segment times. Sustain is 0 – 100%.

**Fader pickup:** after you change the fader page with Pad 7, a fader has no effect until it reaches the stored value of the new page. Then it follows the fader. This prevents jumps in the sound.

**At power-on**, the MiniLab parameters have default values (two saws, slightly detuned, medium filter envelope) until you move a control. Devinera has no presets.

### MiniLab 3 setup (Arturia MIDI Control Center)

The template file `Devinera.minilab3` has all the settings above (pad bank A).

1. Connect the MiniLab 3 to the computer and open MIDI Control Center.
2. Click *Import* and select `Devinera.minilab3`.
3. Drag the template onto a User program slot (or select it and click *Store To*).
4. Select that User program on the MiniLab.

Manual setup, without the file: Knobs 1–8 *Control* CC 20–27 (absolute), Faders 1–4 CC 28–31, Pads 1–7 *Control* CC 102–108 mode *Toggle*, Pad 8 CC 109 mode *Gate*. Keep the mod strip on CC 1 and the pitch strip on pitch bend.

Devinera listens on all MIDI channels.

## Notes

- The MIDI events are read in the main loop and handled at the start of each audio block (1 ms at block size 48), so all voice changes happen in the audio callback.
- Voice allocation (Poly): the same note uses the same voice. Otherwise a free voice, then the oldest released voice, then the oldest voice.
- Mono and Unison use last-note priority. When you release a key and another key is still down, the pitch goes back to that key without a new attack.
- The CPU runs at 480 MHz (boost).
- Block size 48: the synth does not use the audio input, so the input hum of the Funbox v3 + Seed3 does not matter.
- The reverb (ReverbSc, DaisySP-LGPL) uses most of the internal SRAM. The delay buffers are in SDRAM.

## Build

Devinera runs in Flash memory on the Daisy Seed (88% used). Build with ```make``` after installing the Daisy Toolchain (```make clean && make``` forces a full rebuild). Flash ```build/devinera.bin``` with the Electrosmith Web Programmer.

Build with ```make clean && make LOAD_DEBUG=1``` to print debug values once per second over USB serial (98% of Flash used). Read them with ```screen /dev/tty.usbmodem* 115200``` (exit: Ctrl+A, K, Y).

```
CPU avg 17.353 max 17.743 | voices 0 | peak 0.000 clipped 0 nan 0 | dip 0000 | knobs 63 99 49 47 13 5
```

| Value | Meaning |
| --- | --- |
| CPU avg / max | Audio callback load in %. Max must stay well below 100, or the audio breaks up and the controls stop responding. |
| voices | Active voices |
| peak / clipped | Highest output level before the soft clipper, and the number of samples above 1.0 (distortion) |
| nan | Invalid samples (a calculation fault) |
| dip | DIP switch states, DIP 1 first |
| knobs | Pedal knobs, 0 – 100 |

## Development log

| Step | Before | After | Reason |
| --- | --- | --- | --- |
| 1. Plan | – | 8 voices, 2 oscillators + sub + noise, ladder filter, 2 ADSR, chorus / delay / reverb, MiniLab 3 control | Keep it simple. Pedal = live and effect controls, MiniLab = sound design |
| 2. Fader pages | 4 faders for one envelope | Pad 7 switches the faders between the Amp and Filter envelope, with pickup | Two envelopes with only four faders |
| 3. Changes to the plan | Guitar input through the effects, reverb freeze | Removed. FS 2 = effects on/off, DIP 3 = velocity off | User choice |
| 4. MiniLab template | Manual setup in MIDI Control Center | `Devinera.minilab3`, made from an exported template | Fast setup. Format found by comparing two exports |
| 5. Moog envelopes and filter | Soft attack, times were time constants (tails about 4.6× longer) | Analog RC attack, real segment times 1 ms – 10 s, filter drive, bass loss, 2/3 key tracking | More Moog-like |
| 6. Moog waveforms | Saw, square, triangle (DaisySP `Oscillator`) | Six Minimoog waves (`VariableShapeOscillator`), DC removed from the pulse waves, level matched | More Moog-like. Without DC removal, pulse notes started and stopped with a thump |
| 7. CPU overload | DaisySP `LadderFilter` (4× oversampled), 400 MHz: about 14% CPU per voice, 8 voices about 130% | `tptladder.h` (3.7× faster on a computer, same response), 480 MHz boost | Unison (8 voices) and 8 notes in Poly gave distortion and lost control |
| 8. Unison | 8 voices, ±14 cents | 2 voices, ±7 cents, hard left / right | Closer to a Moog, less CPU |
