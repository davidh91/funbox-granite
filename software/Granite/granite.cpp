#include "daisy_petal.h"
#include "daisysp.h"
#include "funbox.h"
#include <cmath>

//
// Granite: a granular delay for the GuitarML Funbox_v3/Daisy Seed platform.
// The input is recorded continuously into a circular buffer. Short grains are
// read back from the buffer behind the record position, with pitch, direction
// and position randomness ("spray"). Grain output feeds back into the buffer.
//

using namespace daisy;
using namespace daisysp;
using namespace funbox;  // This is important for mapping the correct controls to the Daisy Seed on Funbox PCB

// Declare a local daisy_petal for hardware access
DaisyPetal hw;
Parameter param1, param2, param3, param4, param5, param6;

#ifdef GRANITE_LOAD_DEBUG
CpuLoadMeter cpu_load;
#endif

bool            bypass;
bool            freeze;

bool            pswitch1[2], pswitch2[2], pswitch3[2], pdip[4];
int             switch1[2], switch2[2], switch3[2], dip[4];

Led led1, led2;

// Block size 4 moves the callback noise to 12 kHz (least audible on the Funbox v3 + Seed3)
constexpr size_t kBlockSize = 4;

float samplerate;

// Circular record buffer in SDRAM
constexpr size_t kBufferSize = 48000 * 4; // 4 seconds at 48 kHz
float DSY_SDRAM_BSS buffer[kBufferSize];
size_t write_pos = 0;

// Grains
constexpr int kMaxGrains = 24;

constexpr float kWetMakeup = 1.585f; // +4 dB

struct Grain
{
    bool   active;
    double pos;       // read position in the buffer (samples)
    float  inc;       // read increment per sample (pitch ratio, negative = reverse)
    float  phase;     // window phase 0..1
    float  phase_inc; // window phase increment per sample
    float  gain_l;
    float  gain_r;
};

Grain grains[kMaxGrains];

// Hann window lookup table
constexpr int kWindowSize = 1024;
float window_table[kWindowSize + 1];

// Grain scheduler
float sched_counter = 0.f;

// Modes, set by the 3-way switches
enum PitchMode { PITCH_FREE, PITCH_SEMI, PITCH_OCTAVE };
enum DirMode   { DIR_FORWARD, DIR_RANDOM, DIR_REVERSE };

PitchMode pitch_mode  = PITCH_SEMI;
DirMode   dir_mode    = DIR_RANDOM;
float     spray_time  = 0.f; // maximum random position offset (seconds)
float     spray_jitter = 0.f; // random timing offset of the scheduler (0..1)

// Values from the knobs
float mix_target      = 0.5f;
float delay_samples   = 0.f;
float size_samples    = 0.f;
float density         = 1.f;
float semitones       = 0.f;
float feedback_target = 0.f;

// Smoothed values
float mix_smooth      = 0.5f;
float feedback_smooth = 0.f;
float wet_gain_smooth = 1.f;

// Feedback and tone
float    last_wet = 0.f;
DcBlock  fb_dcblock;
float    dark_l = 0.f, dark_r = 0.f;
float    dark_coeff = 0.f;

// LED 2 flash on each new grain
float led2_flash = 0.f;

// Fast random number generator
uint32_t rng_state = 22222;
inline float RandomFloat() // 0..1
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return (rng_state >> 8) * (1.0f / 16777216.0f);
}

// Notch filters against the callback noise (DIP 3), same as the hum test firmware
struct Notch
{
    double b0, b1, b2, a1, a2, x1, x2, y1, y2;

    void Init(float freq, float sample_rate, float q, float gain_db)
    {
        double a     = pow(10.0, gain_db / 40.0);
        double w0    = 2.0 * M_PI * freq / sample_rate;
        double alpha = sin(w0) / (2.0 * q);
        double a0    = 1.0 + alpha / a;
        b0 = (1.0 + alpha * a) / a0;
        b1 = (-2.0 * cos(w0)) / a0;
        b2 = (1.0 - alpha * a) / a0;
        a1 = (-2.0 * cos(w0)) / a0;
        a2 = (1.0 - alpha / a) / a0;
        x1 = x2 = y1 = y2 = 0.0;
    }

    float Process(float in)
    {
        double y = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = in;
        y2 = y1;
        y1 = y;
        return (float)y;
    }
};

constexpr int kMaxNotches = 16;
Notch         notches[kMaxNotches];
int           num_notches = 0;

void InitNotches(float sample_rate, size_t block_size)
{
    float base  = sample_rate / block_size;
    num_notches = 0;
    for(int h = 1; h <= kMaxNotches; h++)
    {
        float freq = base * h;
        if(freq >= 20000.f || freq >= 0.45f * sample_rate)
            break;
        notches[num_notches++].Init(freq, sample_rate, 100.f, -40.f);
    }
}


inline float ReadBuffer(double pos)
{
    int32_t i0 = (int32_t)pos;
    float   frac = (float)(pos - i0);
    int32_t i1 = i0 + 1;
    if(i1 >= (int32_t)kBufferSize)
        i1 -= kBufferSize;
    return buffer[i0] + frac * (buffer[i1] - buffer[i0]);
}

inline float ReadWindow(float phase)
{
    float idx  = phase * kWindowSize;
    int   i0   = (int)idx;
    float frac = idx - i0;
    return window_table[i0] + frac * (window_table[i0 + 1] - window_table[i0]);
}

void StartGrain()
{
    Grain* g = nullptr;
    for(int i = 0; i < kMaxGrains; i++)
    {
        if(!grains[i].active)
        {
            g = &grains[i];
            break;
        }
    }
    if(g == nullptr)
        return; // All grains are busy, skip this one

    // Pitch
    float semis = semitones;
    if(pitch_mode == PITCH_SEMI)
    {
        semis = roundf(semis);
    }
    else if(pitch_mode == PITCH_OCTAVE)
    {
        semis = semis < -6.f ? -12.f : (semis > 6.f ? 12.f : 0.f);
        if(RandomFloat() < 0.3f) // Shimmer: random octave up on some grains
            semis += 12.f;
    }
    float ratio = powf(2.f, semis / 12.f);

    // Direction
    bool reverse = (dir_mode == DIR_REVERSE)
                   || (dir_mode == DIR_RANDOM && RandomFloat() < 0.5f);

    // Position behind the record position
    float delay = delay_samples + RandomFloat() * spray_time * samplerate;
    // A forward grain with pitch up reads faster than the record position moves,
    // so it must start far enough back to not cross the record position.
    if(!reverse && ratio > 1.f)
        delay = fmaxf(delay, (ratio - 1.f) * size_samples + 64.f);
    // A reverse grain moves away from the record position, it must not wrap past the oldest sample
    float max_delay = kBufferSize - 64.f - (reverse ? ratio * size_samples : 0.f);
    delay = fminf(fmaxf(delay, 64.f), max_delay);

    double pos = (double)write_pos - delay;
    if(pos < 0.0)
        pos += kBufferSize;

    g->pos       = pos;
    g->inc       = reverse ? -ratio : ratio;
    g->phase     = 0.f;
    g->phase_inc = 1.f / size_samples;

    // Stereo spread (DIP 1)
    if(pdip[0])
    {
        float pan = RandomFloat() * HALFPI_F;
        g->gain_l = cosf(pan) * 1.41421356f;
        g->gain_r = sinf(pan) * 1.41421356f;
    }
    else
    {
        g->gain_l = 1.f;
        g->gain_r = 1.f;
    }

    g->active  = true;
    led2_flash = 1.f;
}

void ClearGrains()
{
    for(int i = 0; i < kMaxGrains; i++)
        grains[i].active = false;
}


void updateSwitch1() // left=free pitch, center=semitones, right=octaves + shimmer
{
    if (pswitch1[0] == true) {  // left
        pitch_mode = PITCH_FREE;
    } else if (pswitch1[1] == true) {  // right
        pitch_mode = PITCH_OCTAVE;
    } else {   // center
        pitch_mode = PITCH_SEMI;
    }
}

void updateSwitch2() // left=forward, center=random, right=reverse
{
    if (pswitch2[0] == true) {  // left
        dir_mode = DIR_FORWARD;
    } else if (pswitch2[1] == true) {  // right
        dir_mode = DIR_REVERSE;
    } else {   // center
        dir_mode = DIR_RANDOM;
    }
}

void updateSwitch3() // left=spray off, center=small, right=large
{
    if (pswitch3[0] == true) {  // left
        spray_time   = 0.f;
        spray_jitter = 0.f;
    } else if (pswitch3[1] == true) {  // right
        spray_time   = 0.5f;
        spray_jitter = 0.75f;
    } else {   // center
        spray_time   = 0.05f;
        spray_jitter = 0.25f;
    }
}


void UpdateButtons()
{
    // (De-)Activate bypass and toggle LED when left footswitch is let go
    if(hw.switches[Funbox::FOOTSWITCH_1].FallingEdge())
    {
        bypass = !bypass;
        if(bypass && !pdip[1]) // No trails: stop the grains
            ClearGrains();
    }

    // Toggle freeze when the right footswitch is pressed
    if(hw.switches[Funbox::FOOTSWITCH_2].RisingEdge())
    {
        freeze = !freeze;
    }

    led1.Set(bypass ? 0.0f : 1.0f);
    if(freeze)
        led2.Set(1.0f);
    else
        led2.Set(bypass ? 0.0f : led2_flash);

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



// This runs at a fixed rate, to prepare audio samples
static void AudioCallback(AudioHandle::InputBuffer  in,
                          AudioHandle::OutputBuffer out,
                          size_t                    size)
{
#ifdef GRANITE_LOAD_DEBUG
    cpu_load.OnBlockStart();
#endif

    hw.ProcessAnalogControls();
    hw.ProcessDigitalControls();

    UpdateButtons();
    UpdateSwitches();

    float vparam1 = param1.Process();
    float vparam2 = param2.Process();
    float vparam3 = param3.Process();
    float vparam4 = param4.Process();
    float vparam5 = param5.Process();
    float vparam6 = param6.Process();

    // Handle Knob Changes Here
    mix_target      = vparam1;                                            // 0..1
    delay_samples   = 0.02f * powf(100.f, vparam2) * samplerate;          // 20 ms .. 2 s
    size_samples    = 0.01f * powf(50.f, vparam3) * samplerate;           // 10 ms .. 500 ms
    density         = powf(40.f, vparam4);                                // 1 .. 40 grains per second
    semitones       = vparam5 * 24.f - 12.f;                              // -12 .. +12
    if(fabsf(semitones) < 0.5f)                                           // Small dead zone at noon
        semitones = 0.f;
    feedback_target = vparam6 * 0.95f;                                    // 0 .. 95%

    // Keep the wet level about constant when many grains overlap
    float overlap         = density * size_samples / samplerate;
    float wet_gain_target = 1.f / sqrtf(fmaxf(overlap, 1.f));

    float interval = samplerate / density;

    bool trails   = pdip[1];
    bool notch_on = pdip[2];
    bool dark     = pdip[3];

    led2_flash *= 0.999f; // Flash decays over about 80 ms at block size 4

    for(size_t i = 0; i < size; i++)
    {
        float input = in[0][i];
        if(notch_on)
        {
            for(int n = 0; n < num_notches; n++)
                input = notches[n].Process(input);
        }

        if(bypass && !trails)
        {
            out[0][i] = in[0][i];
            out[1][i] = in[0][i];

            // Keep recording, so the buffer has audio when the effect turns on
            if(!freeze)
            {
                buffer[write_pos] = input;
                write_pos         = (write_pos + 1) % kBufferSize;
            }
            continue;
        }

        fonepole(mix_smooth, mix_target, 0.0005f);
        fonepole(feedback_smooth, feedback_target, 0.0005f);
        fonepole(wet_gain_smooth, wet_gain_target, 0.0005f);

        // Record the input plus feedback. In bypass with trails, only the feedback is recorded.
        if(!freeze)
        {
            float rec_in = bypass ? 0.f : input;
            float fb     = fb_dcblock.Process(last_wet) * feedback_smooth;
            buffer[write_pos] = SoftClip(rec_in + fb);
            write_pos         = (write_pos + 1) % kBufferSize;
        }

        // Start new grains
        sched_counter -= 1.f;
        if(sched_counter <= 0.f)
        {
            StartGrain();
            float jitter = (RandomFloat() * 2.f - 1.f) * spray_jitter;
            sched_counter += interval * (1.f + jitter);
            if(sched_counter < 1.f)
                sched_counter = 1.f;
        }

        // Play the grains
        float wet_l = 0.f, wet_r = 0.f;
        for(int g = 0; g < kMaxGrains; g++)
        {
            Grain& gr = grains[g];
            if(!gr.active)
                continue;

            float s = ReadBuffer(gr.pos) * ReadWindow(gr.phase);
            wet_l += s * gr.gain_l;
            wet_r += s * gr.gain_r;

            gr.pos += gr.inc;
            if(gr.pos >= kBufferSize)
                gr.pos -= kBufferSize;
            else if(gr.pos < 0.0)
                gr.pos += kBufferSize;

            gr.phase += gr.phase_inc;
            if(gr.phase >= 1.f)
                gr.active = false;
        }
        wet_l *= wet_gain_smooth;
        wet_r *= wet_gain_smooth;

        last_wet = (wet_l + wet_r) * 0.5f;

        // Makeup gain: the grain windows make the wet signal quieter than the dry signal
        wet_l = SoftClip(wet_l * kWetMakeup);
        wet_r = SoftClip(wet_r * kWetMakeup);

        // Dark wet signal (DIP 4)
        if(dark)
        {
            dark_l += dark_coeff * (wet_l - dark_l);
            dark_r += dark_coeff * (wet_r - dark_r);
            wet_l = dark_l;
            wet_r = dark_r;
        }

        // Final mix: full dry level up to the center, full wet level from the center
        float dry_level = bypass ? 1.f : fminf(1.f, 2.f * (1.f - mix_smooth));
        float wet_level = fminf(1.f, 2.f * mix_smooth);

        out[0][i] = in[0][i] * dry_level + wet_l * wet_level;
        out[1][i] = in[0][i] * dry_level + wet_r * wet_level;
    }

#ifdef GRANITE_LOAD_DEBUG
    cpu_load.OnBlockEnd();
#endif
}


int main(void)
{
    hw.Init();
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

    // Clear the record buffer (SDRAM is not cleared at startup)
    for(size_t i = 0; i < kBufferSize; i++)
        buffer[i] = 0.f;

    for(int i = 0; i <= kWindowSize; i++)
        window_table[i] = 0.5f - 0.5f * cosf(TWOPI_F * i / kWindowSize);

    ClearGrains();
    fb_dcblock.Init(samplerate);
    dark_coeff = 1.f - expf(-TWOPI_F * 2500.f / samplerate); // 2.5 kHz one-pole low-pass
    InitNotches(samplerate, kBlockSize);

    // Init the LEDs and set activate bypass
    led1.Init(hw.seed.GetPin(Funbox::LED_1),false);
    led1.Update();
    bypass = true;
    freeze = false;

    led2.Init(hw.seed.GetPin(Funbox::LED_2),false);
    led2.Update();

#ifdef GRANITE_LOAD_DEBUG
    hw.seed.StartLog(false);
    cpu_load.Init(samplerate, kBlockSize);
#endif

    hw.StartAdc();
    hw.StartAudio(AudioCallback);
    while(1)
    {
#ifdef GRANITE_LOAD_DEBUG
        System::Delay(1000);
        hw.seed.PrintLine("CPU avg " FLT_FMT3 "  max " FLT_FMT3,
                          FLT_VAR3(cpu_load.GetAvgCpuLoad() * 100.f),
                          FLT_VAR3(cpu_load.GetMaxCpuLoad() * 100.f));
        cpu_load.Reset();
#else
        System::Delay(10);
#endif
    }
}
