#include "daisy_petal.h"
#include "daisysp.h"
#include "funbox.h"
#include <cmath>

//
// Syrius: a monophonic guitar synth for the GuitarML Funbox_v3/Daisy Seed platform.
// The main loop tracks the pitch of the input (YIN). The audio callback plays two
// oscillators at that pitch through a resonant ladder filter with an envelope.
// The synth note can sustain much longer than the guitar note (Sustain knob),
// or forever (Hold footswitch).
//

using namespace daisy;
using namespace daisysp;
using namespace funbox;  // This is important for mapping the correct controls to the Daisy Seed on Funbox PCB

// Declare a local daisy_petal for hardware access
DaisyPetal hw;
Parameter param1, param2, param3, param4, param5, param6;

#ifdef SYRIUS_LOAD_DEBUG
CpuLoadMeter cpu_load;
#endif

bool            bypass;
bool            hold;

bool            pswitch1[2], pswitch2[2], pswitch3[2], pdip[4];
int             switch1[2], switch2[2], switch3[2], dip[4];

Led led1, led2;

// Block size 4 moves the callback noise to 12 kHz (least audible on the Funbox v3 + Seed3)
constexpr size_t kBlockSize = 4;

float samplerate;

// Pitch detection runs at half the sample rate on a low-passed copy of the input
constexpr int      kDecimation   = 2;
constexpr int      kYinWindow    = 384;                          // 16 ms at 24 kHz
constexpr int      kMaxLag       = 343;                          // 70 Hz at 24 kHz
constexpr int      kMinLag       = 16;                           // 1500 Hz at 24 kHz
constexpr int      kAnalysisSize = kYinWindow + kMaxLag + 2;
constexpr int      kHop          = 128;                          // New estimate every 5.3 ms
constexpr float    kYinThreshold = 0.15f;
constexpr uint32_t kRingSize     = 2048;                         // Power of 2
constexpr uint32_t kRingMask     = kRingSize - 1;

float             det_ring[kRingSize];
volatile uint32_t det_count = 0; // Total number of samples written to det_ring

float yin_buf[kAnalysisSize];
float yin_diff[kMaxLag + 2];

// Pitch estimates from the main loop to the audio callback
volatile float    pitch_hz    = 220.f;
volatile uint32_t pitch_count = 0;
uint32_t          last_pitch_count = 0;

// Detector filters
float det_hp = 0.f, det_hp_prev = 0.f;
float det_lp1 = 0.f, det_lp2 = 0.f;
float det_hp_coeff = 0.f, det_lp_coeff = 0.f;
int   decim_counter = 0;

// Input envelope followers
float env_fast = 0.f, env_slow = 0.f;
float env_fast_att = 0.f, env_fast_rel = 0.f, env_slow_coeff = 0.f;

// Gate thresholds (DIP 3 = high sensitivity)
constexpr float kGateOn       = 0.008f;
constexpr float kGateOff      = 0.003f;
constexpr float kHighSensGain = 0.3f;

// Note state
bool     note_on       = false;
bool     pending       = false;  // An onset was detected, waiting for the new pitch
uint32_t pending_age   = 0;
uint32_t onset_holdoff = 0;
uint32_t note_age      = 0;
bool     hold_latched  = false;  // Hold has a note

float freq_target_log = 7.78f; // log2(220)
float freq_log        = 7.78f;
float freq            = 220.f;

// Envelopes
float amp        = 0.f;
float fenv       = 0.f;
bool  fenv_rise  = false;
float fenv_att   = 0.f, fenv_dec = 0.f;

// Modes, set by the 3-way switches
enum Wave  { WAVE_SAW_MODE, WAVE_SQUARE_MODE, WAVE_TRI_MODE };
enum Voice { VOICE_UNISON, VOICE_OCT_DOWN, VOICE_FIFTH };
enum FMode { FILTER_LP, FILTER_BP, FILTER_HP };

Voice voice = VOICE_OCT_DOWN;

// Synth voice
Oscillator   osc1, osc2;
LadderFilter filter_l, filter_r;

constexpr float kSynthLevel = 0.6f;

// Values from the knobs
float mix_target   = 0.5f;
float attack_coeff = 0.f;
float release_coeff = 0.f;
float trail_coeff  = 0.f;
float cutoff_base  = 1000.f;
float env_depth    = 0.f;

// Smoothed values
float mix_smooth = 0.5f;

inline float TimeToCoeff(float seconds)
{
    return 1.f - expf(-1.f / (seconds * samplerate));
}


// YIN pitch detection on yin_buf. Returns false if there is no clear pitch.
bool DetectPitch(float det_rate, float& hz)
{
    float energy = 0.f;
    for(int j = 0; j < kYinWindow; j++)
        energy += yin_buf[j] * yin_buf[j];
    if(energy < kYinWindow * 1e-8f)
        return false;

    // Cumulative mean normalized difference
    yin_diff[0]   = 1.f;
    float running = 0.f;
    for(int tau = 1; tau <= kMaxLag + 1; tau++)
    {
        float d = 0.f;
        for(int j = 0; j < kYinWindow; j++)
        {
            float diff = yin_buf[j] - yin_buf[j + tau];
            d += diff * diff;
        }
        running += d;
        yin_diff[tau] = running > 0.f ? d * tau / running : 1.f;
    }

    // First dip below the threshold, then walk down to its minimum
    int tau = -1;
    for(int t = kMinLag; t <= kMaxLag; t++)
    {
        if(yin_diff[t] < kYinThreshold)
        {
            while(t < kMaxLag && yin_diff[t + 1] < yin_diff[t])
                t++;
            tau = t;
            break;
        }
    }
    if(tau < 0)
        return false;

    // Parabolic interpolation
    float s0    = yin_diff[tau - 1];
    float s1    = yin_diff[tau];
    float s2    = yin_diff[tau + 1];
    float denom = s0 - 2.f * s1 + s2;
    float shift = denom != 0.f ? 0.5f * (s0 - s2) / denom : 0.f;
    shift       = fclamp(shift, -1.f, 1.f);

    hz = det_rate / (tau + shift);
    return true;
}


void StartNote(float hz)
{
    freq_target_log = log2f(hz);
    if(amp < 0.02f) // From silence: no glide from the previous note
        freq_log = freq_target_log;
    note_on   = true;
    note_age  = 0;
    fenv_rise = true;
}


void updateSwitch1() // left=saw, center=square, right=triangle
{
    uint8_t wf;
    if (pswitch1[0] == true) {  // left
        wf = Oscillator::WAVE_POLYBLEP_SAW;
    } else if (pswitch1[1] == true) {  // right
        wf = Oscillator::WAVE_POLYBLEP_TRI;
    } else {   // center
        wf = Oscillator::WAVE_POLYBLEP_SQUARE;
    }
    osc1.SetWaveform(wf);
    osc2.SetWaveform(wf);
}

void updateSwitch2() // left=unison, center=octave down, right=fifth up
{
    if (pswitch2[0] == true) {  // left
        voice = VOICE_UNISON;
    } else if (pswitch2[1] == true) {  // right
        voice = VOICE_FIFTH;
    } else {   // center
        voice = VOICE_OCT_DOWN;
    }
}

void updateSwitch3() // left=low-pass, center=band-pass, right=high-pass
{
    LadderFilter::FilterMode mode;
    if (pswitch3[0] == true) {  // left
        mode = LadderFilter::FilterMode::LP24;
    } else if (pswitch3[1] == true) {  // right
        mode = LadderFilter::FilterMode::HP12;
    } else {   // center
        mode = LadderFilter::FilterMode::BP12;
    }
    filter_l.SetFilterMode(mode);
    filter_r.SetFilterMode(mode);
}


void UpdateButtons()
{
    // (De-)Activate bypass and toggle LED when left footswitch is let go
    if(hw.switches[Funbox::FOOTSWITCH_1].FallingEdge())
    {
        bypass       = !bypass;
        hold         = false;
        hold_latched = false;
        if(bypass && !pdip[1]) // No trails: stop the synth
        {
            amp     = 0.f;
            note_on = false;
            pending = false;
        }
    }

    // Toggle hold when the right footswitch is pressed
    if(hw.switches[Funbox::FOOTSWITCH_2].RisingEdge() && !bypass)
    {
        hold = !hold;
        if(hold)
        {
            // Hold the current note, or wait for the next note
            hold_latched = amp > 0.05f;
            if(hold_latched)
                note_on = true;
        }
        else
        {
            hold_latched = false;
            note_on      = false; // Release; the next note starts normally
            pending      = false;
        }
    }

    led1.Set(bypass ? 0.0f : 1.0f);
    if(hold)
        led2.Set(1.0f);
    else
        led2.Set(bypass ? 0.0f : amp);

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
#ifdef SYRIUS_LOAD_DEBUG
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
    mix_target   = vparam1;                                              // 0..1
    float attack = 0.002f * powf(1000.f, vparam2);                       // 2 ms .. 2 s
    attack_coeff = TimeToCoeff(attack);
    float release = 0.05f * powf(200.f, vparam3);                        // 50 ms .. 10 s time constant
    release_coeff = vparam3 > 0.97f ? 1.f : 1.f - TimeToCoeff(release);  // Top of the knob: infinite
    trail_coeff   = 1.f - TimeToCoeff(fminf(release, 2.f));              // Bypass with trails: max 2 s
    cutoff_base  = 80.f * powf(2.f, vparam4 * 7.5f);                     // 80 Hz .. 14.5 kHz
    float res    = vparam5 * 1.3f;                                       // 0 .. 1.3 (no self-oscillation)
    env_depth    = vparam6 * 5.f;                                        // 0 .. 5 octaves
    fenv_att     = TimeToCoeff(fmaxf(attack, 0.003f));

    bool  stereo    = pdip[0];
    bool  trails    = pdip[1];
    float sens      = pdip[2] ? kHighSensGain : 1.f;
    float glide     = TimeToCoeff(pdip[3] ? 0.15f : 0.005f);
    float gate_on   = kGateOn * sens;
    float gate_off  = kGateOff * sens;

    // New pitch estimate from the main loop
    bool  got_pitch = false;
    float new_hz    = 0.f;
    if(pitch_count != last_pitch_count)
    {
        last_pitch_count = pitch_count;
        new_hz           = pitch_hz;
        got_pitch        = true;
    }

    // Note logic
    bool frozen = hold && hold_latched;
    if(got_pitch && !frozen && !bypass)
    {
        if(pending && pending_age > (uint32_t)(0.015f * samplerate))
        {
            // New pick attack: start the note at the new pitch
            pending = false;
            StartNote(new_hz);
        }
        else if(note_on && !pending)
        {
            freq_target_log = log2f(new_hz); // Legato: hammer-on, slide, bend
        }
        else if(!note_on && !pending && env_fast > gate_on)
        {
            StartNote(new_hz); // Volume swell without a clear attack
        }
    }
    if(pending && pending_age > (uint32_t)(0.1f * samplerate))
        pending = false; // No pitch found after the attack

    if(hold && !hold_latched && note_on && note_age > (uint32_t)(0.1f * samplerate))
        hold_latched = true; // Hold the note when its pitch is stable

    // Synth pitch and filter settings for this block
    fonepole(freq_log, freq_target_log, glide * size);
    freq = powf(2.f, freq_log);
    switch(voice)
    {
        case VOICE_UNISON:
            osc1.SetFreq(freq * 0.99424f); // -10 cents
            osc2.SetFreq(freq * 1.00579f); // +10 cents
            break;
        case VOICE_OCT_DOWN:
            osc1.SetFreq(freq);
            osc2.SetFreq(freq * 0.5f);
            break;
        case VOICE_FIFTH:
            osc1.SetFreq(freq);
            osc2.SetFreq(freq * 1.5f);
            break;
    }
    float osc2_level = voice == VOICE_FIFTH ? 0.6f : (voice == VOICE_OCT_DOWN ? 0.8f : 1.f);

    float keytrack = sqrtf(freq / 220.f); // Higher notes open the filter a bit
    float cutoff   = cutoff_base * keytrack * powf(2.f, env_depth * fenv);
    cutoff         = fclamp(cutoff, 20.f, 18000.f);
    filter_l.SetFreq(cutoff);
    filter_l.SetRes(res);
    filter_r.SetFreq(cutoff);
    filter_r.SetRes(res);

    for(size_t i = 0; i < size; i++)
    {
        float input = in[0][i];

        // Detector path: high-pass 60 Hz, 2x low-pass 1.5 kHz, decimate to 24 kHz
        det_hp      = det_hp_coeff * (det_hp + input - det_hp_prev);
        det_hp_prev = input;
        det_lp1 += det_lp_coeff * (det_hp - det_lp1);
        det_lp2 += det_lp_coeff * (det_lp1 - det_lp2);
        if(++decim_counter >= kDecimation)
        {
            decim_counter = 0;
            det_ring[det_count & kRingMask] = det_lp2;
            det_count                       = det_count + 1;
        }

        // Input envelope and pick attack detection
        float rect = fabsf(det_lp2); // Low-passed: the callback noise does not hold the gate open
        env_fast += (rect > env_fast ? env_fast_att : env_fast_rel) * (rect - env_fast);
        env_slow += env_slow_coeff * (rect - env_slow);

        if(onset_holdoff > 0)
            onset_holdoff--;
        if(!frozen && !bypass && onset_holdoff == 0 && env_fast > gate_on
           && env_fast > 2.f * env_slow + 0.001f)
        {
            pending       = true;
            pending_age   = 0;
            onset_holdoff = (uint32_t)(0.08f * samplerate);
        }
        pending_age++;
        note_age++;

        if(note_on && !frozen && env_fast < gate_off)
            note_on = false; // Guitar note ended: release

        // Amplitude envelope
        if(note_on)
            amp += attack_coeff * (1.f - amp);
        else
            amp *= bypass ? trail_coeff : release_coeff;

        // Filter envelope: attack with the amp, then decay
        if(fenv_rise)
        {
            fenv += fenv_att * (1.1f - fenv);
            if(fenv >= 1.f)
            {
                fenv      = 1.f;
                fenv_rise = false;
            }
        }
        else
        {
            fenv *= fenv_dec;
        }

        if(bypass && !trails)
        {
            out[0][i] = in[0][i];
            out[1][i] = in[0][i];
            continue;
        }

        fonepole(mix_smooth, mix_target, 0.0005f);

        // Synth voice
        float s1 = osc1.Process();
        float s2 = osc2.Process() * osc2_level;
        float wet_l, wet_r;
        if(stereo)
        {
            wet_l = filter_l.Process((s1 + 0.4f * s2) * 0.6f);
            wet_r = filter_r.Process((0.4f * s1 + s2) * 0.6f);
        }
        else
        {
            wet_l = filter_l.Process((s1 + s2) * 0.5f);
            wet_r = wet_l;
        }
        float gain = amp * kSynthLevel;
        wet_l      = SoftClip(wet_l * gain);
        wet_r      = SoftClip(wet_r * gain);

        // Final mix: full dry level up to the center, full synth level from the center
        float dry_level = bypass ? 1.f : fminf(1.f, 2.f * (1.f - mix_smooth));
        float wet_level = fminf(1.f, 2.f * mix_smooth);

        out[0][i] = in[0][i] * dry_level + wet_l * wet_level;
        out[1][i] = in[0][i] * dry_level + wet_r * wet_level;
    }

#ifdef SYRIUS_LOAD_DEBUG
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

    osc1.Init(samplerate);
    osc2.Init(samplerate);
    osc1.SetAmp(1.f);
    osc2.SetAmp(1.f);
    filter_l.Init(samplerate);
    filter_r.Init(samplerate);

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

    det_hp_coeff   = expf(-TWOPI_F * 60.f / samplerate);   // 60 Hz one-pole high-pass
    det_lp_coeff   = TimeToCoeff(1.f / (TWOPI_F * 1500.f)); // 1.5 kHz one-pole low-pass
    env_fast_att   = TimeToCoeff(0.0005f);
    env_fast_rel   = TimeToCoeff(0.02f);
    env_slow_coeff = TimeToCoeff(0.03f);
    fenv_dec       = 1.f - TimeToCoeff(0.4f);
    float det_rate = samplerate / kDecimation;

    // Init the LEDs and set activate bypass
    led1.Init(hw.seed.GetPin(Funbox::LED_1),false);
    led1.Update();
    bypass = true;
    hold   = false;

    led2.Init(hw.seed.GetPin(Funbox::LED_2),false);
    led2.Update();

#ifdef SYRIUS_LOAD_DEBUG
    hw.seed.StartLog(false);
    cpu_load.Init(samplerate, kBlockSize);
    uint32_t last_print = System::GetNow();
#endif

    hw.StartAdc();
    hw.StartAudio(AudioCallback);

    // Pitch detection runs here, outside the audio callback
    uint32_t last_analysis = 0;
    float    prev_hz       = 0.f;
    bool     prev_valid    = false;
    while(1)
    {
        uint32_t count = det_count;
        if(count - last_analysis >= kHop && count >= kAnalysisSize)
        {
            last_analysis = count;
            uint32_t start = count - kAnalysisSize;
            for(int i = 0; i < kAnalysisSize; i++)
                yin_buf[i] = det_ring[(start + i) & kRingMask];

            // Publish a pitch only when two estimates in a row agree (rejects octave errors)
            float hz;
            if(DetectPitch(det_rate, hz))
            {
                if(prev_valid && fabsf(log2f(hz / prev_hz)) < 0.06f) // 0.7 semitones
                {
                    pitch_hz    = hz;
                    pitch_count = pitch_count + 1;
                }
                prev_hz    = hz;
                prev_valid = true;
            }
            else
            {
                prev_valid = false;
            }
        }

#ifdef SYRIUS_LOAD_DEBUG
        if(System::GetNow() - last_print >= 1000)
        {
            last_print = System::GetNow();
            hw.seed.PrintLine("CPU avg " FLT_FMT3 "  max " FLT_FMT3 "  pitch " FLT_FMT3 " Hz",
                              FLT_VAR3(cpu_load.GetAvgCpuLoad() * 100.f),
                              FLT_VAR3(cpu_load.GetMaxCpuLoad() * 100.f),
                              FLT_VAR3(pitch_hz));
            cpu_load.Reset();
        }
#endif
    }
}
