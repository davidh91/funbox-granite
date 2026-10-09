#pragma once
#include "daisysp.h"
#include <cmath>

// Moog style 4-pole ladder filter, zero-delay-feedback (TPT) form after V. Zavalishin,
// "The Art of VA Filter Design". It runs at the sample rate (no oversampling) with one
// tanh saturation on the filter input, so it costs much less CPU than the DaisySP LadderFilter.
class TptLadder
{
  public:
    enum class Mode { LP24, BP12, HP12 };

    void Init(float sample_rate)
    {
        sample_rate_ = sample_rate;
        for(int i = 0; i < 4; i++)
            s_[i] = 0.f;
        drive_ = 1.f;
        pbg_   = 0.25f;
        mode_  = Mode::LP24;
        k_     = 0.f;
        SetFreq(1000.f);
    }

    void SetFreq(float freq)
    {
        freq = daisysp::fclamp(freq, 10.f, 0.45f * sample_rate_);
        float w = PI_F * freq / sample_rate_;
        float g = sinf(w) / cosf(w); // tan(w); sinf and cosf are already in the firmware, tanf is not
        G_      = g / (1.f + g);
        beta_   = 1.f / (1.f + g);
        Update();
    }

    void SetRes(float res) // 0 .. 1.8, self-oscillation from about 1.0
    {
        k_ = 4.f * daisysp::fclamp(res, 0.f, 1.8f);
        Update();
    }

    void SetInputDrive(float drive) { drive_ = drive; }
    void SetPassbandGain(float pbg) { pbg_ = pbg; }   // Bass compensation at high resonance, 0 .. 0.5
    void SetMode(Mode mode) { mode_ = mode; }

    inline float Process(float in)
    {
        float x = in * drive_;
        // Solve the feedback loop: y4 = G^4 * u + S
        float S = beta_ * (G3_ * s_[0] + G2_ * s_[1] + G_ * s_[2] + s_[3]);
        float u = (x * (1.f + pbg_ * k_) - k_ * S) * inv_;
        u       = FastTanh(u);

        float v  = (u - s_[0]) * G_;
        float y1 = v + s_[0];
        s_[0]    = y1 + v;
        v        = (y1 - s_[1]) * G_;
        float y2 = v + s_[1];
        s_[1]    = y2 + v;
        v        = (y2 - s_[2]) * G_;
        float y3 = v + s_[2];
        s_[2]    = y3 + v;
        v        = (y3 - s_[3]) * G_;
        float y4 = v + s_[3];
        s_[3]    = y4 + v;

        switch(mode_)
        {
            case Mode::BP12: return (y1 - y2) * 2.f;
            case Mode::HP12: return x + y2 - 2.f * y1;
            default: return y4;
        }
    }

  private:
    static inline float FastTanh(float x)
    {
        if(x > 3.f)
            return 1.f;
        if(x < -3.f)
            return -1.f;
        float x2 = x * x;
        return x * (27.f + x2) / (27.f + 9.f * x2);
    }

    void Update()
    {
        G2_  = G_ * G_;
        G3_  = G2_ * G_;
        inv_ = 1.f / (1.f + k_ * G3_ * G_);
    }

    float sample_rate_;
    float s_[4];
    float G_, G2_, G3_, beta_, inv_;
    float k_, drive_, pbg_;
    Mode  mode_;
};
