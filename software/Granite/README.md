# Granite

Granite is a granular delay, inspired by granular / glitch delay pedals such as the Red Panda Particle. The input is recorded continuously into a 4 second buffer. Short pieces of the recording (grains) are played back from behind the record position, with pitch, direction and position randomness. The grain output feeds back into the buffer, so each repeat becomes more granular.

## Controls

| Control | Function | Effect on the sound |
| --- | --- | --- |
| Ctrl 1 | Mix | Dry/wet. The dry signal stays at full level up to noon, the wet signal is at full level from noon. |
| Ctrl 2 | Time (20 ms – 2 s) | How far back the grains read. Low: a texture on top of your notes. High: clear repeats of earlier phrases. |
| Ctrl 3 | Size (10 – 500 ms) | Grain length. Small: clicks and stutter. Large: recognizable fragments and smooth clouds. |
| Ctrl 4 | Density (1 – 40 grains/s) | How often a new grain starts. Low: sparse glitches. High: a dense cloud. |
| Ctrl 5 | Pitch (−12 to +12 semitones) | Pitch of the grains. Noon is no pitch shift. |
| Ctrl 6 | Feedback (0 – 95%) | Grain output goes back into the recording. Low: one pass. High: long, building repeats; pitch shift and reverse stack up with each repeat. |
| 3-Way Switch 1 | Pitch mode | left=Free (continuous), center=Semitones, right=Octaves + random octave up on 30% of the grains (shimmer) |
| 3-Way Switch 2 | Direction | left=Forward, center=Random (50/50), right=Reverse |
| 3-Way Switch 3 | Spray | left=Off, center=Small, right=Large. Adds a random position offset (up to 50 / 500 ms) and random timing (±25 / ±75%). More chaotic and less "buzzy". |
| Dip Switch 1 | Stereo spread | Each grain gets a random pan position. |
| Dip Switch 2 | Trails | In bypass, the grains and feedback continue to play out. |
| Dip Switch 3 | Notch filters | Removes the audio callback noise from the input (see Hum below). |
| Dip Switch 4 | Dark | 2.5 kHz low-pass on the wet signal. |
| FS 1 | Bypass |  |
| FS 2 | Freeze | Stops the recording. The grains continue to play the last 4 seconds; play on top of it. Feedback has no effect in freeze. |
| LED 1 | Effect on |  |
| LED 2 | Freeze / grain | On in freeze. Otherwise flashes for each new grain (looks constantly on at high density). |
| Audio In 1 | Mono | Ignores Right channel input |
| Audio Out 1 | Stereo | |

Grains are started by a clock (Density + Spray), not by the input level. Grains also play when you do not play.

## How it works

1. A 4 second circular buffer in SDRAM records input + feedback (soft clipped, DC blocked).
2. A scheduler starts a grain every 1/Density seconds (randomized by Spray).
3. Each grain reads from Time (+ random Spray offset) behind the record position, at the Pitch ratio, forward or reverse, with a Hann window. Forward grains with pitch up start far enough back so they never cross the record position.
4. Up to 24 grains play at the same time. If all grains are busy, a new grain is skipped.
5. The wet signal gets +4 dB makeup gain (after the feedback point) and a soft clipper.

## Development log

| Step | Before | After | Reason |
| --- | --- | --- | --- |
| 1. Plan | – | 6 knobs, 3 switches, 4 DIPs, 2 footswitches, no MIDI/sync | Keep it simple |
| 2. Spray control | Spray on a knob (proposal) | Spray on Switch 3 | User choice; knobs stay for the main parameters |
| 3. Buffer | 3 s (plan) | 4 s | Room for reverse grains with high pitch at long Time |
| 4. CPU load test (16 grains) | – | Normal 14% max, worst case 39% max, freeze worst case 41% max | Enough headroom at block size 4 |
| 5. Wet level | No makeup gain: full wet quieter than full dry | +4 dB makeup gain + soft clip (+6 dB was also tried) | Grain windows and gaps lower the average level |
| 6. Max grains | 16 | 24 | Max density × max size needs 20 grains; with 16, grains were skipped and the top of K4 did nothing. Expected worst case load about 55%. |
| 7. Density range | 1 – 40 grains/s | Unchanged | Higher only helps very small grains; 40/s is fine |

## Hum (block size)

The Funbox v3 with a Daisy Seed picks up digital noise at the audio callback rate (sample rate / block size: 1 kHz at block size 48). Granite uses block size 4, which moves the noise to 12 kHz, where it is least audible. DIP 3 adds a notch filter at that frequency. The grain code processes one sample at a time, so the block size does not change the sound.

## Build

Granite runs in Flash memory on the Daisy Seed. Build with ```make``` after installing the Daisy Toolchain (```make clean && make``` forces a full rebuild). Flash ```build/granite.bin``` with the Electrosmith Web Programmer.

Build with ```make LOAD_DEBUG=1``` to print the CPU load once per second over USB serial. Read it with ```screen /dev/tty.usbmodem* 115200``` (exit: Ctrl+A, K, Y).
