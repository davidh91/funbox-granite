#include "daisy_petal.h"
#include "daisysp.h"
#include "daisysp-lgpl.h"
#include "funbox.h"
#include "tptladder.h"
#include <cmath>

//
// Devinera: an 8 voice polyphonic synth for the GuitarML Funbox_v3/Daisy Seed platform,
// played from a MIDI keyboard (Arturia MiniLab 3) through the MIDI In jack.
// Each voice: 2 oscillators + sub oscillator + noise -> ladder filter -> amp envelope.
// Effects: chorus -> delay -> reverb.
// The pedal has the live and effect controls, the MiniLab has the sound design controls.
//

using namespace daisy;
using namespace daisysp;
using namespace funbox;  // This is important for mapping the correct controls to the Daisy Seed on Funbox PCB

// Declare a local daisy_petal for hardware access
DaisyPetal hw;
Parameter param1, param2, param3, param4, param5, param6;

#ifdef DEVINERA_LOAD_DEBUG
CpuLoadMeter cpu_load;
float        debug_peak    = 0.f; // Highest output level before the soft clipper
uint32_t     debug_nan     = 0;   // Number of invalid output samples
uint32_t     debug_clipped = 0;   // Number of samples above 1.0 before the soft clipper
float        debug_knobs[6];
#endif

bool            pswitch1[2], pswitch2[2], pswitch3[2], pdip[4];
int             switch1[2], switch2[2], switch3[2], dip[4];

Led led1, led2;

// The synth does not use the audio input, so the input hum does not matter: use a normal block size
constexpr size_t kBlockSize = 48;
constexpr size_t kSubBlock  = 8; // Modulation (pitch, filter cutoff, LFO) is updated every 8 samples

float samplerate;

// MIDI CC numbers. Set the MiniLab 3 User program to these values (see README).
constexpr uint8_t kCcModWheel     = 1;
constexpr uint8_t kCcSustain      = 64;
constexpr uint8_t kCcAllNotesOff  = 123;
constexpr uint8_t kCcKnobFirst    = 20; // Knobs 1-8: CC 20-27
constexpr uint8_t kCcFaderFirst   = 28; // Faders 1-4: CC 28-31
constexpr uint8_t kCcPadFirst     = 102; // Pads 1-8: CC 102-109

enum Knob
{
    KNOB_OSC1_WAVE,
    KNOB_OSC2_WAVE,
    KNOB_OSC2_PITCH,
    KNOB_OSC2_DETUNE,
    KNOB_OSC_BALANCE,
    KNOB_SUB_LEVEL,
    KNOB_FENV_AMOUNT,
    KNOB_LFO_RATE,
};

enum Pad
{
    PAD_LFO_PITCH,
    PAD_LFO_FILTER,
    PAD_LFO_PW,
    PAD_LFO_RANDOM,
    PAD_NOISE,
    PAD_VEL_FILTER,
    PAD_FADER_PAGE,
    PAD_PANIC,
};

// Modes, set by the 3-way switches
enum VoiceMode { MODE_MONO, MODE_POLY, MODE_UNISON };
enum ChorusMode { CHORUS_OFF, CHORUS_1, CHORUS_2 };

VoiceMode  voice_mode  = MODE_POLY;
ChorusMode chorus_mode = CHORUS_1;
TptLadder::Mode filter_mode = TptLadder::Mode::LP24;

// Sound parameters from the MiniLab
// Minimoog oscillator waveforms (the order of the Model D waveform switch).
// VariableShapeOscillator: shape 0 = triangle, 0.25 = half triangle + half saw (shark-tooth),
// 0.5 = saw, 1 = pulse with pulse width pw.
// The gain brings the waves to about the same loudness. Narrow pulses stay a bit quieter, like on a Moog.
struct MoogWave
{
    float shape;
    float pw;
    float gain;
};
constexpr int  kNumWaves = 6;
const MoogWave kMoogWaves[kNumWaves] = {
    {0.f, 0.5f, 1.f},     // Triangle
    {0.25f, 0.5f, 1.3f},  // Shark-tooth (triangle-saw)
    {0.5f, 0.5f, 1.f},    // Saw
    {1.f, 0.5f, 0.75f},   // Square
    {1.f, 0.3f, 0.75f},   // Wide pulse
    {1.f, 0.12f, 0.75f},  // Narrow pulse
};

inline bool IsPulse(int wave) { return kMoogWaves[wave].shape == 1.f; }
constexpr int kWaveSaw = 2;

int     osc1_wave    = kWaveSaw;
int     osc2_wave    = kWaveSaw;
float   osc2_semis   = 0.f;
float   osc2_cents   = 7.f;
float   osc_balance  = 0.5f;
float   sub_level    = 0.f;
float   fenv_amount  = 2.f;   // Octaves
float   lfo_rate     = 5.f;   // Hz
float   mod_depth    = 0.f;   // Mod strip, 0..1
float   bend_semis   = 0.f;
bool    pads[8]      = {false, false, false, false, false, false, false, false};

// ADSR settings: [0] = amp, [1] = filter. Values are the raw CC values 0..127.
uint8_t adsr_cc[2][4] = {{20, 70, 110, 80}, {20, 85, 40, 80}};

// Fader pickup: after a page change, a fader has no effect until it reaches the stored value
bool    fader_engaged[4] = {true, true, true, true};
int     fader_last[4]    = {-1, -1, -1, -1};

// Values from the pedal knobs
float volume       = 0.5f;
float cutoff_base  = 2000.f;
float resonance    = 0.f;
float delay_time_target = 0.3f;
float delay_mix    = 0.f;
float reverb_mix   = 0.f;

// Footswitches
bool hold   = false;
bool fx_on  = true;
bool cc_sustain = false;

float led1_flash = 0.f;

// Fast random number generator
uint32_t rng_state = 22222;
inline float RandomFloat() // 0..1
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return (rng_state >> 8) * (1.0f / 16777216.0f);
}

inline float MidiToFreq(float note)
{
    return 440.f * powf(2.f, (note - 69.f) / 12.f);
}


// ---------------------------------------------------------------- Voices

constexpr int kNumVoices = 8;

// Unison: detune (cents) and pan (-1..1) of each voice
constexpr int kUnisonVoices = 2; // One voice left, one right: wide and close to a Moog
const float kUnisonCents[kUnisonVoices] = {-7.f, 7.f};
const float kUnisonPan[kUnisonVoices]   = {-1.f, 1.f};
// Poly: stereo voice spread
const float kPolyPan[kNumVoices]     = {-0.3f, 0.3f, -0.2f, 0.2f, -0.1f, 0.1f, -0.25f, 0.25f};

struct Voice
{
    VariableShapeOscillator osc1, osc2;
    Oscillator   sub;
    TptLadder    filter;
    Adsr         amp_env, filt_env;

    int      note     = -1;
    bool     gate     = false;
    bool     key_down = false; // The key is pressed (gate can stay on with Hold / sustain)
    float    velocity = 1.f;
    uint32_t age      = 0;     // Note-on counter value, for voice stealing
    float    pitch    = 60.f;  // Current pitch with glide (MIDI note number)
    float    target   = 60.f;
    float    cents    = 0.f;   // Unison detune
    float    gain_l   = 1.f, gain_r = 1.f;
    float    fenv_out = 0.f;

    void Init(float sr)
    {
        osc1.Init(sr);
        osc2.Init(sr);
        osc1.SetSync(false);
        osc2.SetSync(false);
        sub.Init(sr);
        sub.SetWaveform(Oscillator::WAVE_POLYBLEP_SQUARE);
        filter.Init(sr);
        filter.SetInputDrive(1.f);      // Moog style: the mixer drives the filter a little
        filter.SetPassbandGain(0.25f);  // Some bass loss at high resonance, like a Moog ladder
        amp_env.Init(sr);
        filt_env.Init(sr);
    }

    bool Active() const { return gate || amp_env.IsRunning(); }

    void SetPan(float pan) // -1..1, equal power
    {
        float angle = (pan + 1.f) * 0.25f * PI_F;
        gain_l      = cosf(angle) * 1.41421356f;
        gain_r      = sinf(angle) * 1.41421356f;
    }
};

Voice    voices[kNumVoices];
uint32_t note_counter = 0;

// Held keys for Mono and Unison (last note priority)
constexpr int kMaxHeld = 16;
int held[kMaxHeld];
int num_held = 0;

void HeldAdd(int note)
{
    for(int i = 0; i < num_held; i++)
        if(held[i] == note)
            return;
    if(num_held < kMaxHeld)
        held[num_held++] = note;
}

void HeldRemove(int note)
{
    for(int i = 0; i < num_held; i++)
    {
        if(held[i] == note)
        {
            for(int j = i; j < num_held - 1; j++)
                held[j] = held[j + 1];
            num_held--;
            return;
        }
    }
}

bool Sustained() { return hold || cc_sustain; }

// Moog style envelopes: the fader times are the real segment times (1 ms .. 10 s, exponential taper,
// noon = 100 ms). The attack charges toward 1.5 like an analog RC envelope, so it rises almost
// linearly and does not slow down near the top. Decay and release are exponential.
constexpr float kAttackShape  = 0.7f;  // DaisySP Adsr attack target 1.47
constexpr float kSegmentToTau = 4.6f;  // Release reaches 0 after 4.6 time constants

inline float FaderTime(uint8_t value, float min_time)
{
    return fmaxf(min_time, 0.001f * powf(10000.f, value / 127.f)); // 1 ms .. 10 s
}

void ApplyAdsr()
{
    for(int e = 0; e < 2; e++)
    {
        float a = FaderTime(adsr_cc[e][0], 0.001f);
        float d = FaderTime(adsr_cc[e][1], 0.002f) / kSegmentToTau;
        float s = adsr_cc[e][2] / 127.f;
        float r = FaderTime(adsr_cc[e][3], 0.002f) / kSegmentToTau; // 2 ms minimum: no clicks
        for(int v = 0; v < kNumVoices; v++)
        {
            Adsr& env = e == 0 ? voices[v].amp_env : voices[v].filt_env;
            env.SetAttackTime(a, kAttackShape);
            env.SetDecayTime(d);
            env.SetReleaseTime(r);
            env.SetSustainLevel(s);
        }
    }
}

void ApplyWaveforms()
{
    for(int v = 0; v < kNumVoices; v++)
    {
        voices[v].osc1.SetWaveshape(kMoogWaves[osc1_wave].shape);
        voices[v].osc2.SetWaveshape(kMoogWaves[osc2_wave].shape);
    }
}

void ApplyFilterMode()
{
    for(int v = 0; v < kNumVoices; v++)
        voices[v].filter.SetMode(filter_mode);
}

void StartVoice(Voice& v, int note, float velocity, bool legato)
{
    if(!v.Active())
        v.pitch = note; // From silence: no glide
    v.note     = note;
    v.target   = note;
    v.velocity = velocity;
    v.key_down = true;
    if(!legato)
    {
        v.gate = true;
        v.age  = ++note_counter;
        v.amp_env.Retrigger(false); // Soft retrigger: no click
        v.filt_env.Retrigger(false);
    }
}

void ReleaseAll()
{
    for(int i = 0; i < kNumVoices; i++)
    {
        voices[i].gate     = false;
        voices[i].key_down = false;
    }
    num_held = 0;
}

void SetupVoiceMode()
{
    ReleaseAll();
    for(int i = 0; i < kNumVoices; i++)
    {
        if(voice_mode == MODE_UNISON && i < kUnisonVoices)
        {
            voices[i].cents = kUnisonCents[i];
            voices[i].SetPan(kUnisonPan[i]);
        }
        else
        {
            voices[i].cents = 0.f;
            voices[i].SetPan(voice_mode == MODE_POLY ? kPolyPan[i] : 0.f);
        }
    }
}

Voice* FindPolyVoice(int note)
{
    // Same note again: use the same voice
    for(int i = 0; i < kNumVoices; i++)
        if(voices[i].note == note && voices[i].Active())
            return &voices[i];
    // A free voice
    for(int i = 0; i < kNumVoices; i++)
        if(!voices[i].Active())
            return &voices[i];
    // The oldest released voice
    Voice* best = nullptr;
    for(int i = 0; i < kNumVoices; i++)
        if(!voices[i].gate && (best == nullptr || voices[i].age < best->age))
            best = &voices[i];
    if(best != nullptr)
        return best;
    // The oldest voice
    best = &voices[0];
    for(int i = 1; i < kNumVoices; i++)
        if(voices[i].age < best->age)
            best = &voices[i];
    return best;
}

void OnNoteOn(int note, int vel)
{
    float velocity = pdip[2] ? 1.f : vel / 127.f; // DIP 3: velocity off
    led1_flash     = 1.f;

    if(voice_mode == MODE_POLY)
    {
        StartVoice(*FindPolyVoice(note), note, velocity, false);
        return;
    }

    // Mono / Unison: legato when another key is still down
    bool legato = num_held > 0 && voices[0].gate;
    HeldAdd(note);
    int count = voice_mode == MODE_UNISON ? kUnisonVoices : 1;
    for(int i = 0; i < count; i++)
        StartVoice(voices[i], note, velocity, legato);
}

void OnNoteOff(int note)
{
    if(voice_mode == MODE_POLY)
    {
        for(int i = 0; i < kNumVoices; i++)
        {
            Voice& v = voices[i];
            if(v.note == note && v.key_down)
            {
                v.key_down = false;
                if(!Sustained())
                    v.gate = false;
            }
        }
        return;
    }

    HeldRemove(note);
    int count = voice_mode == MODE_UNISON ? kUnisonVoices : 1;
    for(int i = 0; i < count; i++)
    {
        Voice& v = voices[i];
        if(num_held > 0)
        {
            // Back to the last key that is still down
            v.note   = held[num_held - 1];
            v.target = v.note;
        }
        else if(v.note == note)
        {
            v.key_down = false;
            if(!Sustained())
                v.gate = false;
        }
    }
}

void SustainOff()
{
    for(int i = 0; i < kNumVoices; i++)
        if(!voices[i].key_down)
            voices[i].gate = false;
}


// ---------------------------------------------------------------- MIDI

// MIDI events go from the main loop to the audio callback through this queue,
// so all voice changes happen in the audio callback.
constexpr uint32_t kMidiQueueSize = 64;
MidiEvent          midi_queue[kMidiQueueSize];
volatile uint32_t  midi_head = 0;
volatile uint32_t  midi_tail = 0;

void PushMidiEvent(const MidiEvent& e)
{
    if(midi_head - midi_tail >= kMidiQueueSize)
        return; // Queue full
    midi_queue[midi_head % kMidiQueueSize] = e;
    asm volatile("" ::: "memory");
    midi_head = midi_head + 1;
}

void HandleKnob(int knob, uint8_t value)
{
    float x = value / 127.f;
    switch(knob)
    {
        case KNOB_OSC1_WAVE:
        case KNOB_OSC2_WAVE:
        {
            // Six zones: the Minimoog waveforms
            int wf = value * kNumWaves / 128;
            if(knob == KNOB_OSC1_WAVE)
                osc1_wave = wf;
            else
                osc2_wave = wf;
            ApplyWaveforms();
            break;
        }
        case KNOB_OSC2_PITCH:  osc2_semis  = roundf(x * 48.f - 24.f); break;            // -24 .. +24 semitones
        case KNOB_OSC2_DETUNE: osc2_cents  = (value - 64) / 64.f * 50.f; break;         // -50 .. +50 cents
        case KNOB_OSC_BALANCE: osc_balance = x; break;
        case KNOB_SUB_LEVEL:   sub_level   = x; break;
        case KNOB_FENV_AMOUNT: fenv_amount = x * 5.f; break;                            // 0 .. 5 octaves
        case KNOB_LFO_RATE:    lfo_rate    = 0.05f * powf(400.f, x); break;             // 0.05 .. 20 Hz
    }
}

void HandleFader(int fader, uint8_t value)
{
    int      page   = pads[PAD_FADER_PAGE] ? 1 : 0;
    uint8_t& stored = adsr_cc[page][fader];

    if(!fader_engaged[fader])
    {
        // Pickup: engage when the fader reaches or crosses the stored value
        int last = fader_last[fader];
        if(abs(value - stored) <= 2
           || (last >= 0 && (last - stored) * (value - stored) <= 0))
            fader_engaged[fader] = true;
    }
    fader_last[fader] = value;

    if(fader_engaged[fader])
    {
        stored = value;
        ApplyAdsr();
    }
}

void HandlePad(int pad, uint8_t value)
{
    bool on = value >= 64;
    if(pad == PAD_PANIC)
    {
        if(on)
        {
            hold       = false;
            cc_sustain = false;
            ReleaseAll();
        }
        return;
    }
    if(pad == PAD_FADER_PAGE && on != pads[PAD_FADER_PAGE])
    {
        for(int f = 0; f < 4; f++)
        {
            fader_engaged[f] = false;
            fader_last[f]    = -1;
        }
    }
    pads[pad] = on;
}

void HandleMidiMessage(MidiEvent m)
{
    switch(m.type)
    {
        case NoteOn:
        {
            NoteOnEvent p = m.AsNoteOn();
            if(p.velocity == 0) // Note Off can come in as Note On with velocity 0
                OnNoteOff(p.note);
            else
                OnNoteOn(p.note, p.velocity);
            break;
        }
        case NoteOff:
        {
            NoteOffEvent p = m.AsNoteOff();
            OnNoteOff(p.note);
            break;
        }
        case PitchBend:
        {
            PitchBendEvent p = m.AsPitchBend();
            bend_semis       = p.value / 8192.f * 2.f; // +-2 semitones
            break;
        }
        case ControlChange:
        {
            ControlChangeEvent p = m.AsControlChange();
            uint8_t cc = p.control_number;
            if(cc >= kCcKnobFirst && cc < kCcKnobFirst + 8)
                HandleKnob(cc - kCcKnobFirst, p.value);
            else if(cc >= kCcFaderFirst && cc < kCcFaderFirst + 4)
                HandleFader(cc - kCcFaderFirst, p.value);
            else if(cc >= kCcPadFirst && cc < kCcPadFirst + 8)
                HandlePad(cc - kCcPadFirst, p.value);
            else if(cc == kCcModWheel)
                mod_depth = p.value / 127.f;
            else if(cc == kCcSustain)
            {
                cc_sustain = p.value >= 64;
                if(!Sustained())
                    SustainOff();
            }
            else if(cc == kCcAllNotesOff)
                ReleaseAll();
            break;
        }
        default: break;
    }
}


// ---------------------------------------------------------------- Effects

// Juno style chorus: one modulated delay per side, the LFO is inverted on the right side
DelayLine<float, 1024> chorus_l, chorus_r;
float chorus_phase = 0.f;

// Stereo delay, 2 seconds, in SDRAM
constexpr size_t kMaxDelay = 96000;
DelayLine<float, kMaxDelay> DSY_SDRAM_BSS delay_l;
DelayLine<float, kMaxDelay> DSY_SDRAM_BSS delay_r;
float delay_samples = 14400.f;
float delay_tone_l = 0.f, delay_tone_r = 0.f;
float delay_tone_coeff = 0.f;

ReverbSc reverb;

// Global LFO
float lfo_phase  = 0.f;
float lfo_random = 0.f;


// ---------------------------------------------------------------- Controls

void updateSwitch1() // left=high-pass, center=low-pass 24 dB, right=band-pass
{
    if (pswitch1[0] == true) {  // left
        filter_mode = TptLadder::Mode::HP12;
    } else if (pswitch1[1] == true) {  // right
        filter_mode = TptLadder::Mode::BP12;
    } else {   // center
        filter_mode = TptLadder::Mode::LP24;
    }
    ApplyFilterMode();
}

void updateSwitch2() // left=chorus off, center=chorus I, right=chorus II
{
    if (pswitch2[0] == true) {  // left
        chorus_mode = CHORUS_OFF;
    } else if (pswitch2[1] == true) {  // right
        chorus_mode = CHORUS_2;
    } else {   // center
        chorus_mode = CHORUS_1;
    }
}

void updateSwitch3() // left=mono, center=poly, right=unison
{
    VoiceMode mode;
    if (pswitch3[0] == true) {  // left
        mode = MODE_MONO;
    } else if (pswitch3[1] == true) {  // right
        mode = MODE_UNISON;
    } else {   // center
        mode = MODE_POLY;
    }
    if(mode != voice_mode)
    {
        voice_mode = mode;
        SetupVoiceMode();
    }
}


void UpdateButtons()
{
    // Toggle hold when the left footswitch is pressed
    if(hw.switches[Funbox::FOOTSWITCH_1].RisingEdge())
    {
        hold = !hold;
        if(!Sustained())
            SustainOff();
    }

    // Toggle the effects when the right footswitch is pressed
    if(hw.switches[Funbox::FOOTSWITCH_2].RisingEdge())
    {
        fx_on = !fx_on;
    }

    led1.Set(hold ? 1.0f : led1_flash);
    led2.Set(fx_on ? 1.0f : 0.0f);

    led1.Update();
    led2.Update();
}


void UpdateSwitches()
{
    // Detect any changes in switch positions (3 On-Off-On switches and Dip switches)

    // 3-way Switch 1
    bool changed1 = false;
    for(int i=0; i<2; i++) {
        if (hw.switches[switch1[i]].Pressed() != pswitch1[i]) {
            pswitch1[i] = hw.switches[switch1[i]].Pressed();
            changed1 = true;
        }
    }
    if (changed1)
        updateSwitch1();

    // 3-way Switch 2
    bool changed2 = false;
    for(int i=0; i<2; i++) {
        if (hw.switches[switch2[i]].Pressed() != pswitch2[i]) {
            pswitch2[i] = hw.switches[switch2[i]].Pressed();
            changed2 = true;
        }
    }
    if (changed2)
        updateSwitch2();

    // 3-way Switch 3
    bool changed3 = false;
    for(int i=0; i<2; i++) {
        if (hw.switches[switch3[i]].Pressed() != pswitch3[i]) {
            pswitch3[i] = hw.switches[switch3[i]].Pressed();
            changed3 = true;
        }
    }
    if (changed3)
        updateSwitch3();

    // Dip switches
    for(int i=0; i<4; i++) {
        if (hw.switches[dip[i]].Pressed() != pdip[i]) {
            pdip[i] = hw.switches[dip[i]].Pressed();
            // Action for dipswitches handled in audio callback
        }
    }
}


// ---------------------------------------------------------------- Audio

// This runs at a fixed rate, to prepare audio samples
static void AudioCallback(AudioHandle::InputBuffer  in,
                          AudioHandle::OutputBuffer out,
                          size_t                    size)
{
#ifdef DEVINERA_LOAD_DEBUG
    cpu_load.OnBlockStart();
#endif

    hw.ProcessAnalogControls();
    hw.ProcessDigitalControls();

    UpdateButtons();
    UpdateSwitches();

    // MIDI events from the main loop
    while(midi_tail != midi_head)
    {
        HandleMidiMessage(midi_queue[midi_tail % kMidiQueueSize]);
        asm volatile("" ::: "memory");
        midi_tail = midi_tail + 1;
    }

    float vparam1 = param1.Process();
    float vparam2 = param2.Process();
    float vparam3 = param3.Process();
    float vparam4 = param4.Process();
    float vparam5 = param5.Process();
    float vparam6 = param6.Process();
#ifdef DEVINERA_LOAD_DEBUG
    debug_knobs[0] = vparam1;
    debug_knobs[1] = vparam2;
    debug_knobs[2] = vparam3;
    debug_knobs[3] = vparam4;
    debug_knobs[4] = vparam5;
    debug_knobs[5] = vparam6;
#endif

    // Handle Knob Changes Here
    volume            = 2.f * vparam1 * vparam1;                 // 0 .. 2 (noon = 0.5)
    cutoff_base       = 20.f * powf(2.f, vparam2 * 9.8f);        // 20 Hz .. 18 kHz
    resonance         = 1.6f * vparam3 * vparam3;                // 0 .. 1.6, self-oscillation near the top
    delay_time_target = 0.05f * powf(30.f, vparam4) * samplerate; // 50 ms .. 1.5 s
    delay_mix         = vparam5;
    reverb_mix        = vparam6;

    bool  long_reverb = pdip[0];
    bool  ping_pong   = pdip[1];
    bool  glide_on    = pdip[3];

    reverb.SetFeedback(long_reverb ? 0.93f : 0.82f);

    led1_flash *= 0.97f; // Flash decays over about 100 ms at block size 48

    // Values for this block
    float bal1       = sqrtf(1.f - osc_balance);
    float bal2       = sqrtf(osc_balance);
    float osc2_ratio = powf(2.f, (osc2_semis + osc2_cents * 0.01f) / 12.f);
    float noise_lvl  = pads[PAD_NOISE] ? 0.5f : 0.f;
    float glide      = glide_on ? 1.f - expf(-(float)kSubBlock / (0.1f * samplerate)) : 1.f;
    // Filter drive 1.0 doubles the level. Unison voices are panned hard left/right (+3 dB per side).
    float voice_gain = voice_mode == MODE_UNISON ? 0.18f : 0.25f;
    float fb         = delay_mix * 0.75f;

    for(size_t start = 0; start < size; start += kSubBlock)
    {
        // Global LFO
        lfo_phase += lfo_rate * kSubBlock / samplerate;
        if(lfo_phase >= 1.f)
        {
            lfo_phase -= 1.f;
            lfo_random = RandomFloat() * 2.f - 1.f;
        }
        float lfo = pads[PAD_LFO_RANDOM] ? lfo_random : 4.f * fabsf(lfo_phase - 0.5f) - 1.f;
        lfo *= mod_depth;
        float lfo_pitch  = pads[PAD_LFO_PITCH] ? lfo : 0.f;             // +-1 semitone
        float lfo_filter = pads[PAD_LFO_FILTER] ? lfo * 2.f : 0.f;      // +-2 octaves
        float pw_mod     = pads[PAD_LFO_PW] ? 1.f + 0.8f * lfo : 1.f;      // Pulse width x 0.2 .. 1.8
        float pw1        = kMoogWaves[osc1_wave].pw * (IsPulse(osc1_wave) ? pw_mod : 1.f);
        float pw2        = kMoogWaves[osc2_wave].pw * (IsPulse(osc2_wave) ? pw_mod : 1.f);
        // A pulse with width pw has a DC offset of 1 - 2 * pw: remove it, or notes start and stop with a thump
        float dc1        = IsPulse(osc1_wave) ? 1.f - 2.f * pw1 : 0.f;
        float dc2        = IsPulse(osc2_wave) ? 1.f - 2.f * pw2 : 0.f;
        float g1         = bal1 * kMoogWaves[osc1_wave].gain;
        float g2         = bal2 * kMoogWaves[osc2_wave].gain;

        // Voice settings for this sub block
        for(int vi = 0; vi < kNumVoices; vi++)
        {
            Voice& v = voices[vi];
            if(!v.Active())
                continue;
            v.pitch += glide * (v.target - v.pitch);
            float note = v.pitch + bend_semis + lfo_pitch + v.cents * 0.01f;
            float f1   = MidiToFreq(note);
            // With sync off, SetSyncFreq sets the pitch. Set it before the pulse width (SetPW limits use it).
            v.osc1.SetSyncFreq(f1);
            v.osc2.SetSyncFreq(f1 * osc2_ratio);
            v.sub.SetFreq(f1 * 0.5f);
            v.osc1.SetPW(pw1);
            v.osc2.SetPW(pw2);

            float oct = fenv_amount * v.fenv_out
                        + 0.67f * (v.pitch - 60.f) / 12.f // Key tracking 2/3, like the Minimoog
                        + lfo_filter
                        + (pads[PAD_VEL_FILTER] ? 2.f * v.velocity - 1.f : 0.f);
            float cutoff = fclamp(cutoff_base * powf(2.f, oct), 20.f, 20000.f);
            v.filter.SetFreq(cutoff);
            v.filter.SetRes(resonance);
        }

        size_t end = start + kSubBlock < size ? start + kSubBlock : size;
        for(size_t i = start; i < end; i++)
        {
            // Voices
            float sum_l = 0.f, sum_r = 0.f;
            for(int vi = 0; vi < kNumVoices; vi++)
            {
                Voice& v = voices[vi];
                if(!v.Active())
                    continue;
                float s = (v.osc1.Process() - dc1) * g1 + (v.osc2.Process() - dc2) * g2
                          + v.sub.Process() * sub_level;
                if(noise_lvl > 0.f)
                    s += (RandomFloat() * 2.f - 1.f) * noise_lvl;
                float y   = v.filter.Process(s * 0.5f);
                float amp = v.amp_env.Process(v.gate);
                v.fenv_out = v.filt_env.Process(v.gate);
                y *= amp * (0.3f + 0.7f * v.velocity) * voice_gain;
                sum_l += y * v.gain_l;
                sum_r += y * v.gain_r;
            }

            float l = sum_l, r = sum_r;

            if(fx_on)
            {
                // Chorus
                if(chorus_mode != CHORUS_OFF)
                {
                    float rate  = chorus_mode == CHORUS_1 ? 0.5f : 0.83f;
                    float depth = chorus_mode == CHORUS_1 ? 1.5f : 2.2f; // ms
                    chorus_phase += rate / samplerate;
                    if(chorus_phase >= 1.f)
                        chorus_phase -= 1.f;
                    float tri  = 4.f * fabsf(chorus_phase - 0.5f) - 1.f;
                    float d_l  = (3.5f + depth * tri) * 0.001f * samplerate;
                    float d_r  = (3.5f - depth * tri) * 0.001f * samplerate;
                    chorus_l.Write(l);
                    chorus_r.Write(r);
                    l = 0.7f * (l + chorus_l.Read(d_l));
                    r = 0.7f * (r + chorus_r.Read(d_r));
                }

                // Delay
                fonepole(delay_samples, delay_time_target, 0.0002f);
                float wet_l = delay_l.Read(delay_samples);
                float wet_r = delay_r.Read(delay_samples);
                delay_tone_l += delay_tone_coeff * (wet_l - delay_tone_l); // Darker repeats
                delay_tone_r += delay_tone_coeff * (wet_r - delay_tone_r);
                if(ping_pong)
                {
                    delay_l.Write((l + r) * 0.5f + delay_tone_r * fb);
                    delay_r.Write(delay_tone_l * fb);
                }
                else
                {
                    delay_l.Write(l + delay_tone_l * fb);
                    delay_r.Write(r + delay_tone_r * fb);
                }
                l += wet_l * delay_mix;
                r += wet_r * delay_mix;

                // Reverb
                float rev_l, rev_r;
                reverb.Process(l, r, &rev_l, &rev_r);
                l += rev_l * reverb_mix;
                r += rev_r * reverb_mix;
            }

#ifdef DEVINERA_LOAD_DEBUG
            float peak = fmaxf(fabsf(l * volume), fabsf(r * volume));
            if(!(peak == peak) || peak > 1e6f)
                debug_nan++;
            else
            {
                debug_peak = fmaxf(debug_peak, peak);
                if(peak > 1.f)
                    debug_clipped++;
            }
#endif
            out[0][i] = SoftClip(l * volume);
            out[1][i] = SoftClip(r * volume);
        }
    }

#ifdef DEVINERA_LOAD_DEBUG
    cpu_load.OnBlockEnd();
#endif
}


int main(void)
{
    hw.Init(true); // Boost: CPU at 480 MHz instead of 400 MHz
    samplerate = hw.AudioSampleRate();

    hw.SetAudioBlockSize(kBlockSize);

    switch1[0]= Funbox::SWITCH_1_LEFT;
    switch1[1]= Funbox::SWITCH_1_RIGHT;
    switch2[0]= Funbox::SWITCH_2_LEFT;
    switch2[1]= Funbox::SWITCH_2_RIGHT;
    switch3[0]= Funbox::SWITCH_3_LEFT;
    switch3[1]= Funbox::SWITCH_3_RIGHT;
    dip[0]= Funbox::SWITCH_DIP_1;
    dip[1]= Funbox::SWITCH_DIP_2;
    dip[2]= Funbox::SWITCH_DIP_3;
    dip[3]= Funbox::SWITCH_DIP_4;

    pswitch1[0]= false;
    pswitch1[1]= false;
    pswitch2[0]= false;
    pswitch2[1]= false;
    pswitch3[0]= false;
    pswitch3[1]= false;
    pdip[0]= false;
    pdip[1]= false;
    pdip[2]= false;
    pdip[3]= false;

    for(int i = 0; i < kNumVoices; i++)
        voices[i].Init(samplerate);
    ApplyWaveforms();
    ApplyAdsr();
    SetupVoiceMode();

    // Default modes match the center positions of the switches
    updateSwitch1();
    updateSwitch2();
    updateSwitch3();

    param1.Init(hw.knob[Funbox::KNOB_1], 0.0f, 1.0f, Parameter::LINEAR);
    param2.Init(hw.knob[Funbox::KNOB_2], 0.0f, 1.0f, Parameter::LINEAR);
    param3.Init(hw.knob[Funbox::KNOB_3], 0.0f, 1.0f, Parameter::LINEAR);
    param4.Init(hw.knob[Funbox::KNOB_4], 0.0f, 1.0f, Parameter::LINEAR);
    param5.Init(hw.knob[Funbox::KNOB_5], 0.0f, 1.0f, Parameter::LINEAR);
    param6.Init(hw.knob[Funbox::KNOB_6], 0.0f, 1.0f, Parameter::LINEAR);

    chorus_l.Init();
    chorus_r.Init();
    delay_l.Init(); // Clears the buffer (SDRAM is not cleared at startup)
    delay_r.Init();
    delay_tone_coeff = 1.f - expf(-TWOPI_F * 4000.f / samplerate); // 4 kHz one-pole low-pass
    reverb.Init(samplerate);
    reverb.SetLpFreq(9000.f);

    // Init the LEDs
    led1.Init(hw.seed.GetPin(Funbox::LED_1),false);
    led1.Update();
    led2.Init(hw.seed.GetPin(Funbox::LED_2),false);
    led2.Update();

    hw.InitMidi();
    hw.midi.StartReceive();

#ifdef DEVINERA_LOAD_DEBUG
    hw.seed.StartLog(false);
    cpu_load.Init(samplerate, kBlockSize);
    uint32_t last_print = System::GetNow();
#endif

    hw.StartAdc();
    hw.StartAudio(AudioCallback);
    while(1)
    {
        hw.midi.Listen();
        while(hw.midi.HasEvents())
            PushMidiEvent(hw.midi.PopEvent());

#ifdef DEVINERA_LOAD_DEBUG
        if(System::GetNow() - last_print >= 1000)
        {
            last_print = System::GetNow();
            int active = 0;
            for(int i = 0; i < kNumVoices; i++)
                if(voices[i].Active())
                    active++;
            hw.seed.PrintLine("CPU avg " FLT_FMT3 " max " FLT_FMT3 " | voices %d | peak " FLT_FMT3
                              " clipped %lu nan %lu | dip %d%d%d%d | knobs %d %d %d %d %d %d",
                              FLT_VAR3(cpu_load.GetAvgCpuLoad() * 100.f),
                              FLT_VAR3(cpu_load.GetMaxCpuLoad() * 100.f),
                              active,
                              FLT_VAR3(debug_peak),
                              (unsigned long)debug_clipped,
                              (unsigned long)debug_nan,
                              pdip[0], pdip[1], pdip[2], pdip[3],
                              (int)(debug_knobs[0] * 100.f), (int)(debug_knobs[1] * 100.f),
                              (int)(debug_knobs[2] * 100.f), (int)(debug_knobs[3] * 100.f),
                              (int)(debug_knobs[4] * 100.f), (int)(debug_knobs[5] * 100.f));
            cpu_load.Reset();
            debug_peak    = 0.f;
            debug_clipped = 0;
            debug_nan     = 0;
        }
#endif
    }
}
