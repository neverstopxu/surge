/*
** Surge Synthesizer is Free and Open Source Software
**
** Surge is made available under the Gnu General Public License, v3.0
** https://www.gnu.org/licenses/gpl-3.0.en.html
**
** Copyright 2004-2020 by various individuals as described by the Git transaction log
**
** All source at: https://github.com/surge-synthesizer/surge.git
**
** Surge was a commercial product from 2004-2018, with Copyright and ownership
** in that period held by Claes Johanson at Vember Audio. Claes made Surge
** open source in September 2018.
*/

#include "ResonatorEffect.h"
#include "DebugHelpers.h"

ResonatorEffect::ResonatorEffect(SurgeStorage *storage, FxStorage *fxdata, pdata *pd)
    : Effect(storage, fxdata, pd), halfbandIN(6, true), halfbandOUT(6, true)
{
    gain.set_blocksize(BLOCK_SIZE);
    mix.set_blocksize(BLOCK_SIZE);

    qfus = (QuadFilterUnitState *)_aligned_malloc(2 * sizeof(QuadFilterUnitState), 16);
    for (int e = 0; e < 3; ++e)
        for (int c = 0; c < 2; ++c)
            for (int q = 0; q < n_filter_registers; ++q)
                Reg[e][c][q] = 0;
}

ResonatorEffect::~ResonatorEffect() { _aligned_free(qfus); }

void ResonatorEffect::init()
{
    setvars(true);
    bi = 0;
}

void ResonatorEffect::setvars(bool init)
{
    if (init)
    {
        gain.set_target(1.f);
        mix.set_target(1.f);

        gain.instantize();
        mix.instantize();

        halfbandOUT.reset();
        halfbandIN.reset();
    }
    else
    {
    }
}

inline void set1f(__m128 &m, int i, float f) { *((float *)&m + i) = f; }

inline float get1f(__m128 m, int i) { return *((float *)&m + i); }

void ResonatorEffect::process(float *dataL, float *dataR)
{
    if (bi == 0)
        setvars(false);
    bi = (bi + 1) & slowrate_m1;

    /*
     * Strategy: Upsample, Set the coefficients, run the filter, Downsample
     */

    // Upsample phase. This wqorks and needs no more attention.
    float dataOS alignas(16)[2][BLOCK_SIZE_OS];
    halfbandIN.process_block_U2(dataL, dataR, dataOS[0], dataOS[1]);

    /*
     * Select the coefficients. Here you have to base yourself on the mode switch and
     * call CoeffMaker with one fo the known models and subtypes, along with the resonance
     * and the frequency of the particular band.
     */
    auto whichModel = *pdata_ival[resonator_mode];

    int type = 0, subtype = 0;
    switch (whichModel)
    {
    case 0:
        type = fut_lp12;
        subtype = 0;
        // You need to adjust this switch statement
        break;
    default:
        type = fut_none;
        subtype = 0;
        break;
    }
    FilterUnitQFPtr filtptr = GetQFPtrFilterUnit(type, subtype);

    /*
     * So now set up across the voices (e for 'entry' to match SurgeVoice) and the channels (c)
     */
    for (int e = 0; e < 3; ++e)
    {
        float frequencyInPitch = *f[resonator_freq1 + e * 3];
        float resonance = *f[resonator_res1 + e * 3];
        for (int c = 0; c < 2; ++c)
        {
            coeff[e][c].MakeCoeffs(frequencyInPitch, resonance, type, subtype, storage);

            for (int i = 0; i < n_cm_coeffs; i++)
            {
                set1f(qfus[c].C[i], e, coeff[e][c].C[i]);
                set1f(qfus[c].dC[i], e, coeff[e][c].dC[i]);
            }

            for (int i = 0; i < n_filter_registers; i++)
            {
                set1f(qfus[c].R[i], e, Reg[e][c][i]);
            }

            qfus[c].DB[e] = filterDelay[e][c];
            qfus[c].WP[e] = WP[e][c];
            qfus[c].WP[0] = subtype;

            qfus[c].active[e] = 0xFFFFFFFF;
            qfus[c].active[3] = 0;
        }
    }

    /*
     * FIXME - smooth this
     */
    float bankGain[3];
    for (int i = 0; i < 3; ++i)
    {
        bankGain[i] = *f[resonator_gain1 + i * 3]; // probably some units wrk here
    }

    /* Run the filters. You need to grab a smoothed gain here rather than the hardcoded 0.3 */
    for (int s = 0; s < BLOCK_SIZE_OS; ++s)
    {
        auto l128 = _mm_setzero_ps();
        auto r128 = _mm_setzero_ps();

        if (filtptr)
        {
            l128 = filtptr(&(qfus[0]), _mm_set1_ps(dataOS[0][s]));
            r128 = filtptr(&(qfus[1]), _mm_set1_ps(dataOS[1][s]));
        }
        else
        {
            l128 = _mm_set1_ps(dataOS[0][s]);
            r128 = _mm_set1_ps(dataOS[1][s]);
        }

        float tl[4], tr[4];
        _mm_store_ps(tl, l128);
        _mm_store_ps(tr, r128);
        float mixl = 0, mixr = 0;
        for (int i = 0; i < 3; ++i)
        {
            mixl += tl[i] * bankGain[i];
            mixr += tr[i] * bankGain[i];
        }

        dataOS[0][s] = mixl;
        dataOS[1][s] = mixr;
    }

    /* and preserve those registers and stuff. This all works */
    for (int c = 0; c < 2; ++c)
    {
        for (int i = 0; i < n_filter_registers; i++)
        {
            for (int e = 0; e < 3; ++e)
            {
                Reg[e][c][i] = get1f(qfus[c].R[i], e);
                coeff[e][c].C[i] = get1f(qfus[c].C[i], e);
            }
        }
    }
    for (int e = 0; e < 3; ++e)
    {
        for (int c = 0; c < 2; ++c)
        {
            WP[e][c] = qfus[c].WP[e];
        }
    }

    /* Downsample out */
    halfbandOUT.process_block_D2(dataOS[0], dataOS[1]);
    copy_block(dataOS[0], L, BLOCK_SIZE_QUAD);
    copy_block(dataOS[1], R, BLOCK_SIZE_QUAD);

    // And gain and mix
    gain.set_target_smoothed(db_to_linear(*f[resonator_gain]));
    gain.multiply_2_blocks(L, R, BLOCK_SIZE_QUAD);

    mix.set_target_smoothed(limit_range(*f[resonator_mix], -1.f, 1.f));
    mix.fade_2_blocks_to(dataL, L, dataR, R, dataL, dataR, BLOCK_SIZE_QUAD);
}

void ResonatorEffect::suspend() { init(); }

const char *ResonatorEffect::group_label(int id)
{
    switch (id)
    {
    case 0:
        return "Low Band";
    case 1:
        return "Mid Band";
    case 2:
        return "High Band";
    case 3:
        return "Output";
    }
    return 0;
}
int ResonatorEffect::group_label_ypos(int id)
{
    switch (id)
    {
    case 0:
        return 1;
    case 1:
        return 9;
    case 2:
        return 17;
    case 3:
        return 25;
    }
    return 0;
}

void ResonatorEffect::init_ctrltypes()
{
    Effect::init_ctrltypes();

    fxdata->p[resonator_freq1].set_name("Frequency 1");
    fxdata->p[resonator_freq1].set_type(ct_freq_reson_band1);
    fxdata->p[resonator_freq1].posy_offset = 1;
    fxdata->p[resonator_res1].set_name("Resonance 1");
    fxdata->p[resonator_res1].set_type(ct_percent);
    fxdata->p[resonator_res1].posy_offset = 1;
    fxdata->p[resonator_res1].val_default.f = 0.5f;
    fxdata->p[resonator_gain1].set_name("Gain 1");
    fxdata->p[resonator_gain1].set_type(ct_amplitude);
    fxdata->p[resonator_gain1].posy_offset = 1;
    fxdata->p[resonator_gain1].val_default.f = 0.5f;

    fxdata->p[resonator_freq2].set_name("Frequency 2");
    fxdata->p[resonator_freq2].set_type(ct_freq_reson_band2);
    fxdata->p[resonator_freq2].posy_offset = 3;
    fxdata->p[resonator_res2].set_name("Resonance 2");
    fxdata->p[resonator_res2].set_type(ct_percent);
    fxdata->p[resonator_res2].posy_offset = 3;
    fxdata->p[resonator_res2].val_default.f = 0.5f;
    fxdata->p[resonator_gain2].set_name("Gain 2");
    fxdata->p[resonator_gain2].set_type(ct_amplitude);
    fxdata->p[resonator_gain2].posy_offset = 3;
    fxdata->p[resonator_gain2].val_default.f = 0.5f;

    fxdata->p[resonator_freq3].set_name("Frequency 3");
    fxdata->p[resonator_freq3].set_type(ct_freq_reson_band3);
    fxdata->p[resonator_freq3].posy_offset = 5;
    fxdata->p[resonator_res3].set_name("Resonance 3");
    fxdata->p[resonator_res3].set_type(ct_percent);
    fxdata->p[resonator_res3].posy_offset = 5;
    fxdata->p[resonator_res3].val_default.f = 0.5f;
    fxdata->p[resonator_gain3].set_name("Gain 3");
    fxdata->p[resonator_gain3].set_type(ct_amplitude);
    fxdata->p[resonator_gain3].posy_offset = 5;
    fxdata->p[resonator_gain3].val_default.f = 0.5f;

    fxdata->p[resonator_mode].set_name("Mode");
    fxdata->p[resonator_mode].set_type(ct_reson_mode);
    fxdata->p[resonator_mode].posy_offset = 7;
    fxdata->p[resonator_gain].set_name("Gain");
    fxdata->p[resonator_gain].set_type(ct_decibel);
    fxdata->p[resonator_gain].posy_offset = 7;
    fxdata->p[resonator_mix].set_name("Mix");
    fxdata->p[resonator_mix].set_type(ct_percent);
    fxdata->p[resonator_mix].posy_offset = 7;
    fxdata->p[resonator_mix].val_default.f = 1.f;
}

void ResonatorEffect::init_default_values()
{
    fxdata->p[resonator_freq1].val.f = -25.65f; // 100 Hz
    fxdata->p[resonator_res1].val.f = 0.5f;
    fxdata->p[resonator_gain1].val.f = 0.5f;

    fxdata->p[resonator_freq2].val.f = 8.038216f; // 700 Hz
    fxdata->p[resonator_res2].val.f = 0.5f;
    fxdata->p[resonator_gain2].val.f = 0.5f;

    fxdata->p[resonator_freq3].val.f = 35.90135f; // 3500 Hz
    fxdata->p[resonator_res3].val.f = 0.5f;
    fxdata->p[resonator_gain3].val.f = 0.5f;

    fxdata->p[resonator_mode].val.i = 1;
    fxdata->p[resonator_gain].val.f = 0.f;
    fxdata->p[resonator_mix].val.f = 1.f;
}