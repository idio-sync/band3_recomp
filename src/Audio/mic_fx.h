#pragma once
#include <span>

// RB3's mic effects, for band3's own playback of the microphones
// (usb_mic_monitor), so a mic it plays sounds as the game's playback does.
//
// The game plays a mic through the FxSend chain in sfx/gen/mic_fx.milo:
// synapse.send (pitch correction) -> FxSendEQ.send -> FxSendCompress.send
// (which also feeds the game's reverb) -> delay.send (the dry signal, plus an
// echo the game turns up while the singer deploys overdrive). band3 plays the
// EQ and the compressor here, the way the game's EQEffect and
// CompressionEffect XAPOs do (rb3-xenon's src/system/synth/EQEffect.cpp and
// CompressionEffect.cpp), with the milo's settings; the game keeps playing the
// reverb and the echo, which come late anyway (Hooks/usb_mic.cpp). Not copied:
// the pitch correction, which the game applies, gently, to a single singer in
// Solo vocals.

namespace band3::audio::mic_fx {

// EQEffect: a shelf at 6.6 kHz (+5 dB), a bell at 5.5 kHz 1 kHz wide (+4 dB)
// and a shelf at 240 Hz (-10 dB), as FxSendEQ.send sets it. One channel.
class Eq {
public:
    Eq();
    float Process(float x);
    void Reset();

private:
    void SetParameter(int param, float value);

    float band1_freq_ = 12000, band1_gain_ = 0, band1_q_ = 8000, band2_freq_ = 1000;
    float band2_gain_ = 0, band2_q_ = 2000, band3_freq_ = 0, band3_gain_ = 20000, band3_q_ = 0;
    float band4_freq_ = 20, band4_gain_ = 0;

    bool band0_on_ = false;
    float band0_b0_ = 0, band0_shelf_ = 0, band0_coeff_ = 0;
    bool band1_on_ = false;
    float band1_b0_ = 0, band1_b1_ = 0, band1_b2_ = 0, band1_a1_ = 0, band1_a2_ = 0;
    bool band2_on_ = false;
    float band2_b0_ = 0, band2_shelf_ = 0, band2_coeff_ = 0;
    bool band3_on_ = false;
    float band3_b0_ = 0, band3_b1_ = 0, band3_b2_ = 0, band3_a1_ = 0, band3_a2_ = 0;
    bool band4_on_ = false;
    float band4_b0_ = 0, band4_b1_ = 0, band4_b2_ = 0, band4_a1_ = 0, band4_a2_ = 0;

    float delay_a_ = 0, delay_b_ = 0, delay_c_ = 0, delay_d_ = 0, delay_e_ = 0, delay_f_ = 0;
    float band3_x_[2] = {}, band3_z_[2] = {}, band4_x_[2] = {}, band4_z_[2] = {};
};

// CompressionEffect as FxSendCompress.send sets it: -33 dB threshold, 80:1,
// -9 dB out, 10 ms attack and 500 ms release, a gate at -40 dB. With its
// make-up gain it lifts everything over the gate to about the same level. One
// channel.
class Compressor {
public:
    Compressor();
    void Process(std::span<float> samples);

private:
    float threshold_ratio_, makeup_ratio_, ratio_, output_ratio_;
    float attack_coeff_, release_coeff_, gate_attack_coeff_, gate_release_coeff_;
    float gate_min_, gate_max_;
    float dc_block_ = 1.0f;
    float envelope_ = 1.0f;
    float prev_peak_ = 0.0f, prev_sample_ = 0.0f;
};

// The chain from the playback voice to delay.send's dry signal: the voice's
// volume (MicXbox's, a ratio), the EQ and the compressor, each at its send's
// wet gain. One channel, in place.
class Chain {
public:
    void Process(std::span<float> samples, float volume);
    void Reset();

private:
    Eq eq_;
    Compressor compressor_;
};

}
