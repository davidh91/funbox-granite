# Syrius

Syrius is a monophonic guitar synth, inspired by synth pedals such as the Boss SY-200. The pedal tracks the pitch of the note you play and plays two oscillators at that pitch through a resonant filter. The synth note can sustain much longer than the guitar note (Sustain knob), or forever (Hold footswitch).

## Controls

| Control | Function | Effect on the sound |
| --- | --- | --- |
| Ctrl 1 | Mix | Dry/synth. The dry signal stays at full level up to noon, the synth is at full level from noon. Fully clockwise: synth only. |
| Ctrl 2 | Attack (2 ms – 2 s) | Low: the synth starts with the pick attack. High: slow swells, pads. |
| Ctrl 3 | Sustain (50 ms – 10 s, max = infinite) | Release time after the guitar note stops. Fully clockwise: the synth note stays until the next note. |
| Ctrl 4 | Cutoff (80 Hz – 14.5 kHz) | Filter frequency. The cutoff also follows the note pitch a little. |
| Ctrl 5 | Resonance | Filter peak. High: a vocal, "squelchy" synth sound. |
| Ctrl 6 | Envelope | Filter sweep on each note (0 – 5 octaves up, then 400 ms decay). Low: static tone. High: "wow" on each note. |
| 3-Way Switch 1 | Waveform | left=Saw (bright), center=Square (hollow), right=Triangle (soft, flute-like) |
| 3-Way Switch 2 | Voice | left=Unison (two oscillators detuned ±10 cents), center=Octave down, right=Fifth up |
| 3-Way Switch 3 | Filter | left=Low-pass 24 dB, center=Band-pass, right=High-pass |
| Dip Switch 1 | Stereo | The two oscillators are panned left and right, each with its own filter. |
| Dip Switch 2 | Trails | In bypass, the synth note plays out its release (max 2 s). |
| Dip Switch 3 | High sensitivity | Lower note detection threshold, for low-output pickups or soft playing. |
| Dip Switch 4 | Long glide | 150 ms portamento between notes (off: 5 ms). |
| FS 1 | Bypass |  |
| FS 2 | Hold | Holds the current synth note forever. If no note plays, holds the next note. Play on top of the held note; the synth does not follow. Press again to release. |
| LED 1 | Effect on |  |
| LED 2 | Hold / synth level | On in hold. Otherwise shows the synth level. |
| Audio In 1 | Mono | Ignores Right channel input |
| Audio Out 1 | Stereo | |

## Playing tips

- The synth is monophonic. Play single notes. Chords give an unstable or wrong pitch.
- Use the neck pickup and turn the tone knob down a little for the most stable tracking.
- For pads: Attack at 2 o'clock, Sustain at max, Triangle or Unison saw, Cutoff at 11 o'clock.
- For a classic synth lead: Attack low, Saw, Low-pass, Resonance at 2 o'clock, Envelope at 1 o'clock.
- Hold + Mix at noon: a drone note with your guitar on top.

## How it works

1. Detector path: the input is high-passed (60 Hz), low-passed (2 × 1.5 kHz) and decimated to 24 kHz into a ring buffer.
2. The main loop runs a YIN pitch detector (16 ms window, 70 – 1500 Hz) every 5.3 ms. A pitch is used only when two estimates in a row agree within 0.7 semitones. This rejects octave errors.
3. An envelope follower on the detector signal finds pick attacks (fast envelope > 2 × slow envelope). After an attack, the synth waits for the new pitch (15 – 100 ms), then starts the note. Without a new attack (hammer-on, slide, bend), the synth glides to the new pitch.
4. The note stays on while the input is above the gate threshold. Then the Sustain release starts.
5. Two PolyBLEP oscillators go through a 4-pole ladder filter (DaisySP `LadderFilter`). The amp envelope and the filter envelope control the level and the cutoff.

## Hum (block size)

Syrius uses block size 4, the same as Granite. This moves the callback noise of the Funbox v3 + Seed3 to 12 kHz. The detector and the envelope follower use the low-passed signal, so the noise does not affect the note detection.

## Build

Syrius runs in Flash memory on the Daisy Seed. Build with ```make``` after installing the Daisy Toolchain (```make clean && make``` forces a full rebuild). Flash ```build/syrius.bin``` with the Electrosmith Web Programmer.

Build with ```make LOAD_DEBUG=1``` to print the CPU load and the last detected pitch once per second over USB serial. Read it with ```screen /dev/tty.usbmodem* 115200``` (exit: Ctrl+A, K, Y).
