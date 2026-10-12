#include "mic_fx.h"
#include <cmath>

namespace band3::audio::mic_fx {

namespace {

float DbToRatio(float db) { return std::pow(10.0f, db / 20.0f); }

// fsel(a, b, c): a >= 0 ? b : c
float Sel(float a, float b, float c) { return a >= 0.0f ? b : c; }

// mic_fx.milo's FxSendEQ.send, in EQEffect::Params order (FxSendEQ360::
// SyncEffectParams): high cutoff and gain, mid cutoff, bandwidth and gain, low
// cutoff and gain, low-pass cutoff and resonance, high-pass cutoff and
// resonance. The low-pass at 20 kHz and high-pass at 20 Hz are off.
constexpr float kEqParams[11] = {6600, 5, 5500, 1000, 4, 240, -10, 20000, 0, 20, 0};

// mic_fx.milo's FxSendCompress.send
constexpr float kThresholdDb = -33.0f;
constexpr float kRatio = 80.0f;
constexpr float kOutputDb = -9.0f;
constexpr float kAttack = 0.01f;
constexpr float kRelease = 0.5f;
// the expander's attack and release, which CompressionEffect uses for the gate
constexpr float kGateAttack = 0.01f;
constexpr float kGateRelease = 0.15f;
constexpr float kGateDb = -40.0f;

// the sends' wet gains (their dry gains are -96 dB)
constexpr float kEqWetDb = 5.5f;
constexpr float kCompressorWetDb = 10.0f;
// Voice::SetVolume's range
constexpr float kMaxVolume = 4.0f;

float Coeff(float seconds) { return 1.0f - std::exp(-1.0f / (seconds * 48000.0f)); }

}

// --- Eq: EQEffect::SetParameter and Process, for one channel ---

Eq::Eq() {
    for (int i = 0; i < 11; i++) SetParameter(i, kEqParams[i]);
}

void Eq::Reset() {
    delay_a_ = delay_b_ = delay_c_ = delay_d_ = delay_e_ = delay_f_ = 0;
    for (int i = 0; i < 2; i++) band3_x_[i] = band3_z_[i] = band4_x_[i] = band4_z_[i] = 0;
}

void Eq::SetParameter(int param, float value) {
    switch (param) {
    case 0: band1_freq_ = Sel(-Sel(24000 - value, value, 24000), 0, Sel(24000 - value, value, 24000)); break;
    case 1: band1_gain_ = Sel(-42 - Sel(42 - value, value, 42), -42, Sel(42 - value, value, 42)); break;
    case 2: band1_q_ = Sel(-Sel(24000 - value, value, 24000), 0, Sel(24000 - value, value, 24000)); break;
    case 3: band2_freq_ = Sel(-Sel(24000 - value, value, 24000), 0, Sel(24000 - value, value, 24000)); break;
    case 4: band2_gain_ = Sel(-42 - Sel(42 - value, value, 42), -42, Sel(42 - value, value, 42)); break;
    case 5: band2_q_ = Sel(-Sel(24000 - value, value, 24000), 0, Sel(24000 - value, value, 24000)); break;
    case 6: band3_freq_ = Sel(-42 - Sel(42 - value, value, 42), -42, Sel(42 - value, value, 42)); break;
    case 7: band3_gain_ = Sel(20 - Sel(20000 - value, value, 20000), 20, Sel(20000 - value, value, 20000)); break;
    case 8: band3_q_ = Sel(-25 - Sel(25 - value, value, 25), -25, Sel(25 - value, value, 25)); break;
    case 9: band4_freq_ = Sel(20 - Sel(20000 - value, value, 20000), 20, Sel(20000 - value, value, 20000)); break;
    case 10: band4_gain_ = Sel(-25 - Sel(25 - value, value, 25), -25, Sel(25 - value, value, 25)); break;
    default: return;
    }

    if (param <= 1) {
        band0_on_ = band1_gain_ != 0;
        band0_b0_ = static_cast<float>(std::tan(double(band1_freq_ * 6.544985e-05f)));
        const float g = static_cast<float>(std::pow(10.0, double(band1_gain_ * 0.05f)));
        band0_shelf_ = (g - 1) * 0.5f;
        const float t = band1_gain_ > 0 ? band0_b0_ : g * band0_b0_;
        band0_coeff_ = (t - 1) / (t + 1);
    } else if (param <= 4) {
        band1_on_ = band2_gain_ != 0;
        band1_b1_ = static_cast<float>(std::tan(double(band2_freq_ * 6.544985e-05f)));
        band1_b2_ = static_cast<float>(std::pow(10.0, double(band2_gain_ * 0.05f)));
        band1_a1_ = (band1_b2_ - 1) * 0.5f;
        const float neg_cos = -static_cast<float>(std::cos(double(band1_q_ * 0.00013089970161672682f)));
        band1_b0_ = band2_gain_ > 0 ? (band1_b1_ - 1) / (band1_b1_ + 1)
                                    : (band1_b1_ - band1_b2_) / (band1_b1_ + band1_b2_);
        band1_a2_ = (1 - band1_b0_) * neg_cos;
    } else if (param <= 6) {
        band2_on_ = band3_freq_ != 0;
        band2_b0_ = static_cast<float>(std::tan(double(band2_q_ * 6.544985e-05f)));
        const float g = static_cast<float>(std::pow(10.0, double(band3_freq_ * 0.05f)));
        band2_shelf_ = (g - 1) * 0.5f;
        band2_coeff_ = band3_freq_ > 0 ? (band2_b0_ - 1) / (band2_b0_ + 1)
                                       : (band2_b0_ - g) / (band2_b0_ + g);
    } else if (param <= 8) {
        band3_on_ = band3_gain_ < 19999.0f;
        const float wc = band3_gain_ * 4.1666666e-05f * 3.1415927f;
        const float alpha = static_cast<float>(std::sin(double(wc))) *
                            static_cast<float>(std::pow(10.0, double(band3_q_ * -0.05f))) * 0.5f;
        const float k = (1 - alpha) * 0.5f / (alpha + 1);
        const float k_half = k + 0.5f;
        const float cos_k = static_cast<float>(std::cos(double(wc))) * k_half;
        band3_a2_ = k * 2;
        band3_a1_ = cos_k * -2;
        const float fk4 = (k_half - cos_k) * 0.25f;
        band3_b0_ = band3_b2_ = fk4 * 2;
        band3_b1_ = fk4 * 4;
    } else {
        band4_on_ = band4_freq_ > 21.0f;
        const float wc = band4_freq_ * 4.1666666e-05f * 3.1415927f;
        const float alpha = static_cast<float>(std::sin(double(wc))) *
                            static_cast<float>(std::pow(10.0, double(band4_gain_ * -0.05f))) * 0.5f;
        const float k = (1 - alpha) * 0.5f / (alpha + 1);
        const float cos_k = static_cast<float>(std::cos(double(wc))) * (k + 0.5f);
        band4_a2_ = k * 2;
        band4_a1_ = cos_k * -2;
        const float fk4 = (cos_k + k + 0.5f) * 0.25f;
        band4_b0_ = band4_b2_ = fk4 * 2;
        band4_b1_ = fk4 * -4;
    }
}

float Eq::Process(float s) {
    if (band0_on_) {
        const float x = s;
        const float y = -(band0_coeff_ * delay_e_ - x);
        s = (x - (band0_coeff_ * y + delay_e_)) * band0_shelf_ + x;
        delay_e_ = y;
    }
    if (band1_on_) {
        const float x = s;
        const float xn = delay_a_, xn1 = delay_b_, zn = delay_c_, zn1 = delay_d_;
        delay_b_ = xn;
        delay_a_ = x;
        delay_d_ = zn;
        float acc = band1_a2_ * xn - x * band1_b0_;
        acc = acc + xn1;
        acc = -(band1_a2_ * zn - acc);
        const float y = zn1 * band1_b0_ + acc;
        delay_c_ = y;
        s = (x - y) * band1_a1_ + x;
    }
    if (band2_on_) {
        const float x = s;
        const float y = -(band2_coeff_ * delay_f_ - x);
        s = ((band2_coeff_ * y + delay_f_) + x) * band2_shelf_ + x;
        delay_f_ = y;
    }
    if (band3_on_) {
        float acc = band3_b1_ * band3_x_[0] + band3_b2_ * band3_x_[1] + band3_b0_ * s;
        acc = -(band3_a1_ * band3_z_[0] - acc);
        acc = -(band3_a2_ * band3_z_[1] - acc);
        band3_x_[1] = band3_x_[0];
        band3_x_[0] = s;
        band3_z_[1] = band3_z_[0];
        band3_z_[0] = acc;
        s = acc;
    }
    if (band4_on_) {
        float acc = band4_b1_ * band4_x_[0] + band4_b2_ * band4_x_[1] + band4_b0_ * s;
        acc = -(band4_a1_ * band4_z_[0] - acc);
        acc = -(band4_a2_ * band4_z_[1] - acc);
        band4_x_[1] = band4_x_[0];
        band4_x_[0] = s;
        band4_z_[1] = band4_z_[0];
        band4_z_[0] = acc;
        s = acc;
    }
    return s;
}

// --- Compressor: CompressionEffect::SetParameters and Process, one channel ---

Compressor::Compressor()
    : threshold_ratio_(DbToRatio(kThresholdDb)),
      makeup_ratio_(DbToRatio(kThresholdDb / kRatio - kThresholdDb)),
      ratio_(kRatio),
      output_ratio_(DbToRatio(kOutputDb)),
      attack_coeff_(Coeff(kAttack)),
      release_coeff_(Coeff(kRelease)),
      gate_attack_coeff_(Coeff(kGateAttack)),
      gate_release_coeff_(Coeff(kGateRelease)),
      gate_min_(DbToRatio(kGateDb)),
      gate_max_(DbToRatio(kGateDb)) {}

void Compressor::Process(std::span<float> samples) {
    float envelope = envelope_;
    for (float& sample : samples) {
        // a flat stretch away from zero (a clipped microphone) turns the
        // level down, and it recovers slowly
        const float tmp = prev_peak_;
        prev_peak_ = prev_sample_;
        prev_sample_ = sample;
        if ((tmp > 0.07f || tmp < -0.07f) && std::fabs(tmp - prev_peak_) < 0.0003f &&
            std::fabs(sample - prev_peak_) < 0.0003f) {
            dc_block_ = (0.08f - dc_block_) * 0.02f + dc_block_;
        }
        dc_block_ = (1.0f - dc_block_) * 0.002f + dc_block_;

        float peak = 0.00001f;
        if (peak < std::fabs(sample)) peak = std::fabs(sample);

        float level = makeup_ratio_ * peak;
        if (peak > threshold_ratio_) {
            level = ((peak - threshold_ratio_) / ratio_ + threshold_ratio_) * makeup_ratio_;
            if (level > 10000.0f) level = 10000.0f;
        }
        if (peak < gate_min_) {
            gate_min_ = (peak - gate_min_) * 0.05f + gate_min_;
        } else {
            gate_min_ = (gate_max_ - gate_min_) * 0.001f + gate_min_;
        }
        const bool gated = peak < gate_min_ * 1.15f;
        if (gated) level = 0.0f;

        const float gain = level / peak;
        float coeff = envelope > gain ? attack_coeff_ : release_coeff_;
        if (gated) coeff = envelope > gain ? gate_release_coeff_ : gate_attack_coeff_;
        envelope += (gain - envelope) * coeff;
        if (envelope >= 100000.0f) envelope = 100000.0f;

        sample = output_ratio_ * (envelope * (dc_block_ * sample));
    }
    envelope_ = envelope;
}

// --- Chain ---

void Chain::Process(std::span<float> samples, float volume) {
    const float v = volume < 0 ? 0 : (volume > kMaxVolume ? kMaxVolume : volume);
    const float eq_wet = DbToRatio(kEqWetDb);
    for (float& s : samples) s = eq_.Process(s * v) * eq_wet;
    compressor_.Process(samples);
    const float compressor_wet = DbToRatio(kCompressorWetDb);
    for (float& s : samples) s *= compressor_wet;
}

void Chain::Reset() {
    eq_ = Eq();
    compressor_ = Compressor();
}

}
