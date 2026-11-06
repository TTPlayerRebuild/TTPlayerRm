/*
 * Independent SIPR 5/6.5/8.5/16 kbps PCM encoder.
 * Copyright (c) 2026 TTPlayer reconstruction contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Format synthesis is derived from FFmpeg's LGPL SIPR/ACELP decoder by
 * Vladimir Voroshilov and Vitor Sessak (2008-2009). Codebooks are included
 * from the configured LGPL FFmpeg source; no tables are extracted from a DLL.
 * LPC analysis, spectrum search, pitch and innovation searches below are new
 * encoder code, not a port of NihAV or the proprietary Real encoder.
 */
#include "sipr_encoder.h"
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4305) // Upstream C codebooks use unsuffixed constants.
#endif
#include "libavcodec/siprdata.h"
#include "libavcodec/sipr16kdata.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <algorithm>
#include <array>
#include <cfenv>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

extern "C" {
// Match the configured RM backend, including its private context layout.
#define TTP_RM_LEGACY_SIPR 1
#include "libavcodec/sipr.h"
extern const float ff_b60_sinc[61];
extern const float ff_pow_0_55[10];
extern const float ff_pow_0_7[10];
}

namespace ttp::rm {
namespace {
constexpr int kOrder = 10, kSub = 48, kHistory = 154;
constexpr double kPi = 3.14159265358979323846;
using Vector = std::array<float, kSub>;
using Lpc = std::array<float, kOrder>;
class ScopedRounding {
    int previous_;
public:
    ScopedRounding() : previous_(std::fegetround()) { std::fesetround(FE_TONEAREST); }
    ~ScopedRounding() { if (previous_ != -1) std::fesetround(previous_); }
};

double Dot(const Vector& a, const Vector& b) {
    double sum = 0;
    for (int i = 0; i < kSub; ++i) sum += double(a[i]) * b[i];
    return sum;
}

// Direct-form all-pole synthesis. Memory is chronological, oldest first.
void Synthesize(const Lpc& a, const float* input, float* output,
                const float* memory) {
    for (int n = 0; n < kSub; ++n) {
        double sum = input[n];
        for (int j = 1; j <= kOrder; ++j)
            sum -= double(a[j - 1]) * (n >= j ? output[n - j] : memory[kOrder + n - j]);
        output[n] = float(sum);
    }
}

Vector Convolve(const Vector& input, const Vector& h) {
    Vector result{};
    for (int n = 0; n < kSub; ++n) {
        double sum = 0;
        for (int j = 0; j <= n; ++j) sum += double(input[j]) * h[n - j];
        result[n] = float(sum);
    }
    return result;
}

// W(z) = A(z/0.9)/A(z/0.6). Applied to the synthesis error, so both
// candidate and target receive identical frequency weighting.
Vector Weight(const Vector& input, const Lpc& a) {
    Lpc numerator{}, denominator{};
    double n = .9, d = .6;
    for (int i = 0; i < kOrder; ++i) {
        numerator[i] = float(a[i] * n); denominator[i] = float(a[i] * d);
        n *= .9; d *= .6;
    }
    Vector tmp{}, result{}; const Lpc zero{};
    for (int i = 0; i < kSub; ++i) {
        double sum = input[i];
        for (int j = 1; j <= kOrder && j <= i; ++j)
            sum += double(numerator[j - 1]) * input[i - j];
        tmp[i] = float(sum);
    }
    Synthesize(denominator, tmp.data(), result.data(), zero.data());
    return result;
}

// ISP polynomial products, retaining only the symmetric first half.
void IspPolynomial(const float* isp, double* p, int roots) {
    p[0] = 1; p[1] = -2 * double(isp[0]);
    for (int i = 2; i <= roots; ++i) {
        const double f = -2 * double(isp[2 * (i - 1)]);
        p[i] = 2 * p[i - 2] + f * p[i - 1];
        for (int j = i - 1; j >= 2; --j) p[j] += f * p[j - 1] + p[j - 2];
        p[1] += f;
    }
}

Lpc IspToLpc(const Lpc& isp) {
    double p[6]{}, q[5]{}; Lpc a{};
    IspPolynomial(isp.data(), p, 5); IspPolynomial(isp.data() + 1, q, 4);
    for (int i = 4; i >= 2; --i) q[i] -= q[i - 2];
    for (int i = 1; i < 5; ++i) {
        const double x = (1 + double(isp[9])) * p[i];
        const double y = (1 - double(isp[9])) * q[i];
        a[i - 1] = float((x + y) * .5); a[9 - i] = float((x - y) * .5);
    }
    a[4] = float((1 + double(isp[9])) * p[5] * .5); a[9] = isp[9];
    return a;
}

double SpectrumPolynomial(const Lpc& a, double angle, bool odd) {
    double sum = odd ? (1 - double(a[9])) * std::sin(5 * angle)
                     : (1 + double(a[9])) * std::cos(5 * angle) + a[4];
    for (int i = 1; i < 5; ++i)
        sum += odd ? (double(a[i - 1]) - a[9 - i]) * std::sin((5 - i) * angle)
                   : (double(a[i - 1]) + a[9 - i]) * std::cos((5 - i) * angle);
    return sum;
}

bool LpcToIsf(const Lpc& a, Lpc& frequencies) {
    int found[2]{};
    for (int parity = 0; parity < 2; ++parity) {
        double left = 1e-7, vl = SpectrumPolynomial(a, left, parity != 0);
        for (int step = 1; step <= 1024 && found[parity] < 5 - parity; ++step) {
            double right = std::min(kPi - 1e-7, step * kPi / 1024);
            const double vr = SpectrumPolynomial(a, right, parity != 0);
            if (vl * vr <= 0) {
                double lo = left, hi = right, vlo = vl;
                for (int n = 0; n < 24; ++n) {
                    const double mid = (lo + hi) * .5;
                    const double vm = SpectrumPolynomial(a, mid, parity != 0);
                    if (vlo * vm <= 0) hi = mid; else { lo = mid; vlo = vm; }
                }
                frequencies[2 * found[parity] + parity] = float((lo + hi) * .5);
                ++found[parity];
            }
            left = right; vl = vr;
        }
    }
    frequencies[9] = float(a[9] * kPi / 6.153848);
    if (found[0] != 5 || found[1] != 4) return false;
    for (int i = 1; i < 9; ++i)
        if (frequencies[i] <= frequencies[i - 1]) return false;
    return true;
}

Lpc Analyze(const float* previous, const float* frame, int samples) {
    double signal[336]{}, r[11]{}, a[11]{};
    const int size = samples + 96;
    // 48 past + the current frame + 48 future, symmetric Hamming window.
    for (int n = 0; n < size; ++n)
        signal[n] = (n < 48 ? previous[n] : frame[n - 48]) *
                    (.54 - .46 * std::cos(2 * kPi * n / (size - 1)));
    for (int lag = 0; lag <= kOrder; ++lag) {
        for (int n = lag; n < size; ++n) r[lag] += signal[n] * signal[n - lag];
        // Lag window stabilizes narrow-band/near-periodic input.
        r[lag] *= std::exp(-.5 * std::pow(2 * kPi * 60 * lag / 8000, 2));
    }
    r[0] = std::max(r[0] * 1.0001, 1e-9);
    a[0] = 1; double error = r[0];
    for (int order = 1; order <= kOrder; ++order) {
        double sum = r[order];
        for (int j = 1; j < order; ++j) sum += a[j] * r[order - j];
        const double reflection = std::clamp(-sum / std::max(error, 1e-12), -.98, .98);
        double old[11]; std::copy_n(a, 11, old);
        for (int j = 1; j < order; ++j) a[j] = old[j] + reflection * old[order - j];
        a[order] = reflection; error *= 1 - reflection * reflection;
    }
    Lpc result{};
    for (int j = 0; j < kOrder; ++j) result[j] = float(a[j + 1]);
    return result;
}

Lpc QuantizeSpectrum(const Lpc& target, Lpc& history, unsigned* indices) {
    constexpr int sizes[5] = {64,128,128,128,32};
    Lpc result{}, weights{};
    for (int i = 0; i < 9; ++i) {
        const double left = i ? target[i] - target[i - 1] : target[0];
        const double right = i == 8 ? kPi - target[8] : target[i + 1] - target[i];
        weights[i] = float(1 / std::max(.05, std::min(left, right)));
    }
    weights[9] = 4;
    for (int pair = 0; pair < 5; ++pair) {
        double best = std::numeric_limits<double>::max(); unsigned selected = 0;
        for (int code = 0; code < sizes[pair]; ++code) {
            double error = 0;
            for (int j = 0; j < 2; ++j) {
                const int i = 2 * pair + j;
                const double d = target[i] - mean_lsf[i] - .33 * history[i] -
                                 lsf_codebooks[pair][2 * code + j];
                error += weights[i] * d * d;
            }
            if (error < best) { best = error; selected = unsigned(code); }
        }
        indices[pair] = selected;
        for (int j = 0; j < 2; ++j) {
            const int i = 2 * pair + j; const float residual = lsf_codebooks[pair][2 * selected + j];
            result[i] = float(.33 * history[i] + residual + mean_lsf[i]);
            history[i] = residual;
        }
    }
    std::sort(result.begin(), result.begin() + 9);
    float floor = float(.0125 * kPi);
    for (int i = 0; i < 9; ++i) {
        result[i] = std::max(result[i], floor); floor = float(result[i] + .0125 * kPi);
        result[i] = float(std::cos(result[i]));
    }
    result[9] = float(std::min(double(result[9]), 1.3 * kPi) * 6.153848 / kPi);
    return result;
}

void Pitch(unsigned code, int first, int subframe, int& lag, int& fraction, bool low5) {
    int x = int(code);
    if (!subframe || (low5 && subframe == 2)) x = x < 197 ? x + 59 : 3 * x - 335;
    else x = x - 1 + 3 * std::clamp(first - 5, 20, 134);
    lag = x * 10923 >> 15; fraction = x - 3 * lag - 1;
}

Vector Adaptive(const float* history, int lag, int fraction) {
    std::array<float, kHistory + kSub> work{};
    std::copy_n(history, kHistory, work.begin());
    float* out = work.data() + kHistory;
    const float* source = out - lag + (fraction <= 0);
    const int phase = 2 * ((2 + fraction) % 3 + 1);
    for (int n = 0; n < kSub; ++n) {
        float sum = 0;
        for (int i = 0; i < 10; ++i) {
            sum += source[n + i] * ff_b60_sinc[6 * i + phase];
            sum += source[n - i - 1] * ff_b60_sinc[6 * (i + 1) - phase];
        }
        out[n] = sum;
    }
    Vector result; std::copy_n(out, kSub, result.begin()); return result;
}

Vector FixedShape(const Lpc& a, int lag, bool low5) {
    Vector numerator{}, shape{}; Lpc denominator{}, zero{}; numerator[0] = 1;
    for (int j = 0; j < kOrder; ++j) {
        numerator[j + 1] = a[j] * ff_pow_0_55[j]; denominator[j] = a[j] * ff_pow_0_7[j];
    }
    Synthesize(denominator, numerator.data(), shape.data(), zero.data());
    for (int n = lag; n < kSub; ++n) shape[n] += (low5 ? .85f : .8f) * shape[n - lag];
    return shape;
}

struct Pair { int position[2]; float sign[2]; };
Pair PulsePair(unsigned code, int track, bool low65) {
    if (low65) return Pair{{3 * int(code & 15) + track, 0}, {code & 16 ? -1.f : 1.f, 0}};
    Pair pair{{3 * int((code >> 4) & 15) + track, 3 * int(code & 15) + track}, {}};
    pair.sign[0] = code & 256 ? -1.f : 1.f;
    pair.sign[1] = pair.position[1] < pair.position[0] ? -pair.sign[0] : pair.sign[0];
    return pair;
}

// Coordinate search over all 512 two-pulse or 32 single-pulse words per track.
// The objective is normalized positive correlation, evaluated with the full
// filtered pulse cross-correlation matrix, not largest-residual peak picking.
void SearchPulses(const Vector& target, const Vector& response, unsigned* codes, bool low65) {
    double correlation[48]{}, gram[48][48]{};
    for (int p = 0; p < kSub; ++p) {
        for (int n = p; n < kSub; ++n) correlation[p] += double(target[n]) * response[n - p];
        for (int q = 0; q <= p; ++q) {
            double sum = 0;
            for (int n = p; n < kSub; ++n) sum += double(response[n - p]) * response[n - q];
            gram[p][q] = gram[q][p] = sum;
        }
    }
    Pair selected[3]{}; bool active[3]{};
    for (int pass = 0; pass < 4; ++pass) {
        for (int track = 0; track < 3; ++track) {
            double other_c = 0, other_e = 0;
            int positions[4]{}, count = 0; float signs[4]{};
            for (int t = 0; t < 3; ++t) if (t != track && active[t])
                for (int i = 0; i < 2; ++i) {
                    positions[count] = selected[t].position[i]; signs[count++] = selected[t].sign[i];
                }
            for (int i = 0; i < count; ++i) {
                other_c += signs[i] * correlation[positions[i]];
                for (int j = 0; j < count; ++j)
                    other_e += signs[i] * signs[j] * gram[positions[i]][positions[j]];
            }
            double best = -1; unsigned best_code = 0;
            for (unsigned code = 0; code < (low65 ? 32u : 512u); ++code) {
                const Pair pair = PulsePair(code, track, low65);
                const int p = pair.position[0], q = pair.position[1];
                const double s = pair.sign[0], t = pair.sign[1];
                const double c = other_c + s * correlation[p] + t * correlation[q];
                double energy = other_e + s * s * gram[p][p] + t * t * gram[q][q] + 2 * s * t * gram[p][q];
                for (int j = 0; j < count; ++j)
                    energy += 2 * signs[j] * (s * gram[p][positions[j]] + t * gram[q][positions[j]]);
                const double score = c > 0 ? c * c / std::max(energy, 1e-15) : 0;
                if (score > best) { best = score; best_code = code; }
            }
            codes[track] = best_code; selected[track] = PulsePair(best_code, track, low65); active[track] = true;
        }
    }
}

Vector Innovation(const Vector& shape, const unsigned* codes, bool low65) {
    Vector result{};
    for (int track = 0; track < 3; ++track) {
        const Pair pair = PulsePair(codes[track], track, low65);
        for (int p = 0; p < 2; ++p)
            for (int n = pair.position[p]; n < kSub; ++n)
                result[n] += pair.sign[p] * shape[n - pair.position[p]];
    }
    return result;
}

struct Pulses5 { int n; int x[3]; float sign[3]; };
Pulses5 Pulses5k(unsigned code, bool low_gain) {
    Pulses5 p{};
    if (low_gain) {
        p.n = 3; const int offset = code & 512 ? 2 : 0;
        for (int i = 0; i < 3; ++i) {
            p.x[i] = int(code & 7) * 6 + 4 - i * 2;
            p.sign[i] = (offset + p.x[i]) & 3 ? -1.f : 1.f;
            code >>= 3;
        }
    } else {
        p.n = 2; const int subset = int(code >> 8) & 1;
        p.x[0] = int((code >> 4) & 15) * 3 + subset;
        p.x[1] = int(code & 15) * 3 + subset + 1;
        p.sign[0] = code & 512 ? -1.f : 1.f; p.sign[1] = -p.sign[0];
    }
    return p;
}

unsigned Search5k(const Vector& target, const Vector& response, bool low_gain) {
    double correlation[48]{}, gram[48][48]{};
    for (int p = 0; p < kSub; ++p) {
        for (int n = p; n < kSub; ++n) correlation[p] += double(target[n]) * response[n - p];
        for (int q = 0; q <= p; ++q) {
            double sum = 0;
            for (int n = p; n < kSub; ++n) sum += double(response[n - p]) * response[n - q];
            gram[p][q] = gram[q][p] = sum;
        }
    }
    double best = -1; unsigned selected = 0;
    for (unsigned code = 0; code < 1024; ++code) {
        const Pulses5 p = Pulses5k(code, low_gain);
        double c = 0, e = 0;
        for (int i = 0; i < p.n; ++i) {
            c += p.sign[i] * correlation[p.x[i]];
            for (int j = 0; j < p.n; ++j) e += p.sign[i] * p.sign[j] * gram[p.x[i]][p.x[j]];
        }
        const double score = c > 0 ? c * c / std::max(e, 1e-15) : 0;
        if (score > best) { best = score; selected = code; }
    }
    return selected;
}

Vector Innovation5k(const Vector& shape, unsigned code, bool low_gain) {
    Vector result{}; const Pulses5 p = Pulses5k(code, low_gain);
    for (int i = 0; i < p.n; ++i)
        for (int n = p.x[i]; n < kSub; ++n) result[n] += p.sign[i] * shape[n - p.x[i]];
    return result;
}

float GainBase(const Vector& fixed, const Lpc& energy) {
    double predicted = 34;
    for (int i = 0; i < 4; ++i) predicted += double(pred[i]) * energy[i];
    const double measured = 10 * std::log10((.01 + Dot(fixed, fixed)) / kSub);
    return float(std::pow(10., (predicted - measured) * .05) / 32768);
}

float GainMemory(float previous, float pitch) {
    const float power = std::min(.5f * pitch * pitch, .4f);
    return std::min(.7f * previous + .3f * power, power);
}

void Pack(const unsigned* fields, uint8_t* packet, unsigned& bit, bool low65, bool low5) {
    constexpr unsigned widths[20] = {6,7,7,7,5,8,9,9,9,7,5,9,9,9,7,5,9,9,9,7};
    constexpr unsigned widths5[20] = {6,7,7,7,5,8,10,7,5,10,7,8,10,7,5,10,7,5,10,7};
    for (int field = 0; field < 20; ++field) {
        const unsigned width = low5 ? widths5[field] : low65 && field >= 5 && (field - 5) % 5 >= 1 && (field - 5) % 5 <= 3 ? 5 : widths[field];
        for (unsigned i = 0; i < width; ++i, ++bit)
            packet[bit >> 3] |= uint8_t(((fields[field] >> i) & 1) << (bit & 7));
    }
}
// Independent 16k analysis and search; synthesis feedback uses the existing
// statically linked, reconstructed LGPL decoder. No external codec is loaded.
static_assert(sizeof(((SiprContext*)nullptr)->highpass_filt_mem) == 4 * sizeof(float),
              "SIPR encoder requires the RM backend's TTP_RM_LEGACY_SIPR layout");
static_assert(offsetof(SiprContext, postfilter_mem) == offsetof(SiprContext, highpass_filt_mem) + 16,
              "SIPR context layout mismatch");
using WideVector = std::array<float, 80>;
using WideLpc = std::array<float, 16>;
using WideBasis = std::array<WideVector, 80>;

void WideSynthesis(const WideLpc& a, const float* input, float* output,
                   const float* memory, int count = 80) {
    for (int n = 0; n < count; ++n) {
        double sum = input[n];
        for (int j = 1; j <= 16; ++j)
            sum -= double(a[j - 1]) * (n >= j ? output[n - j] : memory[16 + n - j]);
        output[n] = float(sum);
    }
}

WideVector WideWeight(const WideVector& x, const WideLpc& a) {
    WideLpc num{}, den{}, zero{}; WideVector tmp{}, result{};
    double f = .9, g = .6;
    for (int i = 0; i < 16; ++i) { num[i] = float(a[i] * f); den[i] = float(a[i] * g); f *= .9; g *= .6; }
    for (int n = 0; n < 80; ++n) {
        double sum = x[n];
        for (int j = 1; j <= 16 && j <= n; ++j) sum += double(num[j - 1]) * x[n - j];
        tmp[n] = float(sum);
    }
    WideSynthesis(den, tmp.data(), result.data(), zero.data()); return result;
}

WideVector ApplyBasis(const WideBasis& basis, const WideVector& input) {
    WideVector result{};
    for (int n = 0; n < 80; ++n) {
        double sum = 0;
        for (int p = 0; p <= n; ++p) sum += double(input[p]) * basis[p][n];
        result[n] = float(sum);
    }
    return result;
}

double WideDot(const WideVector& a, const WideVector& b) {
    double sum = 0; for (int n = 0; n < 80; ++n) sum += double(a[n]) * b[n]; return sum;
}

WideLpc WideIspToLpc(const double* isp) {
    float f[16]; for (int i = 0; i < 16; ++i) f[i] = float(isp[i]);
    double p[9]{}, q[8]{}; WideLpc a{};
    IspPolynomial(f, p, 8); IspPolynomial(f + 1, q, 7);
    for (int i = 7; i >= 2; --i) q[i] -= q[i - 2];
    for (int i = 1; i < 8; ++i) {
        const double x = (1 + double(f[15])) * p[i], y = (1 - double(f[15])) * q[i];
        a[i - 1] = float((x + y) * .5); a[15 - i] = float((x - y) * .5);
    }
    a[7] = float((1 + double(f[15])) * p[8] * .5); a[15] = f[15]; return a;
}

WideLpc WideAnalyze(const float* past, const float* pcm, double* autocorrelation) {
    double x[480]{}, a[17]{};
    for (int n = 0; n < 480; ++n)
        x[n] = (n < 160 ? past[n] : pcm[n - 160]) * (.54 - .46 * std::cos(2 * kPi * n / 479));
    for (int lag = 0; lag <= 16; ++lag) {
        double sum = 0;
        for (int n = lag; n < 480; ++n) sum += x[n] * x[n - lag];
        autocorrelation[lag] = sum * std::exp(-.5 * std::pow(2 * kPi * 60 * lag / 16000, 2));
    }
    autocorrelation[0] = std::max(1e-9, autocorrelation[0] * 1.0001);
    double error = autocorrelation[0]; a[0] = 1;
    for (int order = 1; order <= 16; ++order) {
        double sum = autocorrelation[order], old[17];
        for (int j = 1; j < order; ++j) sum += a[j] * autocorrelation[order - j];
        const double reflection = std::clamp(-sum / std::max(1e-12, error), -.98, .98);
        std::copy_n(a, 17, old);
        for (int j = 1; j < order; ++j) a[j] = old[j] + reflection * old[order - j];
        a[order] = reflection; error *= 1 - reflection * reflection;
    }
    WideLpc result{}; for (int j = 0; j < 16; ++j) result[j] = float(a[j + 1]); return result;
}

double WideLspPolynomial(const WideLpc& a, double w, bool odd) {
    double sum = odd ? std::sin(8.5 * w) : std::cos(8.5 * w);
    for (int i = 0; i < 16; ++i)
        sum += a[i] * (odd ? std::sin((7.5 - i) * w) : std::cos((7.5 - i) * w));
    return sum;
}

bool WideLpcToLsf(const WideLpc& a, WideLpc& lsf) {
    for (int parity = 0; parity < 2; ++parity) {
        int count = 0; double left = 1e-7, vl = WideLspPolynomial(a, left, parity != 0);
        for (int step = 1; step <= 1024 && count < 8; ++step) {
            const double right = std::min(kPi - 1e-7, step * kPi / 1024);
            const double vr = WideLspPolynomial(a, right, parity != 0);
            if (vl * vr <= 0) {
                double lo = left, hi = right, value = vl;
                for (int n = 0; n < 24; ++n) {
                    double mid = (lo + hi) * .5, v = WideLspPolynomial(a, mid, parity != 0);
                    if (value * v <= 0) hi = mid; else { lo = mid; value = v; }
                }
                lsf[2 * count + parity] = float((lo + hi) * .5); ++count;
            }
            left = right; vl = vr;
        }
        if (count != 8) return false;
    }
    for (int i = 1; i < 16; ++i) if (lsf[i] <= lsf[i - 1]) return false;
    return true;
}

void CloneWide(const SiprContext& source, SiprContext& target) {
    target = source;
    for (int i = 0; i < 2; ++i)
        target.filt_mem[i] = target.filt_buf[source.filt_mem[i] == source.filt_buf[0] ? 0 : 1];
}

// Observe the reconstructed decoder's ISP conversion, avoiding a second copy
// of the legacy transform tables. Candidate contexts and output are disposable.
WideLpc WideDecodedSpectrum(const SiprContext& base, const SiprParameters& params,
                            double* isp) {
    SiprContext probe; CloneWide(base, probe); SiprParameters trial = params; float output[160];
    ff_sipr_decode_frame_16k(&probe, &trial, output);
    if (isp) std::copy_n(probe.lsp_history_16k, 16, isp);
    return WideIspToLpc(probe.lsp_history_16k);
}

double SpectrumError(const WideLpc& a, const double* r) {
    double coefficients[17]; coefficients[0] = 1;
    for (int i = 0; i < 16; ++i) coefficients[i + 1] = a[i];
    double error = 0;
    for (int i = 0; i <= 16; ++i)
        for (int j = 0; j <= 16; ++j) error += coefficients[i] * coefficients[j] * r[std::abs(i - j)];
    return error;
}

SiprParameters WideQuantize(const SiprContext& base, const WideLpc& target,
                            const double* r, double* isp) {
    constexpr int counts[5] = {128,256,128,128,128};
    SiprParameters best_params{}; double best_error = std::numeric_limits<double>::max();
    for (int predictor = 0; predictor < 2; ++predictor) {
        SiprParameters trial{}; trial.ma_pred_switch = predictor;
        unsigned shortlist[5][6]{};
        for (int group = 0; group < 5; ++group) {
            std::array<std::pair<double, unsigned>, 256> candidates{};
            const int start = group * 3, dimensions = group == 4 ? 4 : 3;
            for (int code = 0; code < counts[group]; ++code) {
                double score = 0;
                for (int j = 0; j < dimensions; ++j) {
                    const int i = start + j;
                    // Configured legacy synthesis retains its MA history in Hz.
                    const double previous = base.lsf_history[i] * kPi / 8000;
                    const double value = (1 - qu[predictor]) * lsf_codebooks_16k[group][dimensions * code + j]
                                       + qu[predictor] * previous + mean_lsf_16k[i];
                    const double difference = target[i] - value;
                    const double left = i ? target[i] - target[i - 1] : target[0];
                    const double right = i == 15 ? kPi - target[i] : target[i + 1] - target[i];
                    score += difference * difference / std::max(.04, std::min(left, right));
                }
                candidates[code] = {score, unsigned(code)};
            }
            std::partial_sort(candidates.begin(), candidates.begin() + 6, candidates.begin() + counts[group]);
            for (int n = 0; n < 6; ++n) shortlist[group][n] = candidates[n].second;
            trial.vq_indexes[group] = int(shortlist[group][0]);
        }
        // Refine only six analytic shortlist entries per split, scoring the
        // actual quantized LPC residual energy. This is bounded and deterministic.
        for (int group = 0; group < 5; ++group) {
            double group_error = std::numeric_limits<double>::max(); int selected = trial.vq_indexes[group];
            for (int n = 0; n < 6; ++n) {
                trial.vq_indexes[group] = int(shortlist[group][n]);
                const double error = SpectrumError(WideDecodedSpectrum(base, trial, nullptr), r);
                if (error < group_error) { group_error = error; selected = trial.vq_indexes[group]; }
            }
            trial.vq_indexes[group] = selected;
        }
        const double error = SpectrumError(WideDecodedSpectrum(base, trial, nullptr), r);
        if (error < best_error) { best_error = error; best_params = trial; }
    }
    WideDecodedSpectrum(base, best_params, isp); return best_params;
}

int WidePitch(unsigned code, int previous, int subframe) {
    if (!subframe) return code < 390 ? int(code) + 88 : 3 * int(code) - 690;
    return code < 62 ? 3 * std::clamp(previous - 10, 30, 262) + int(code) - 2 : 3 * previous;
}

WideVector WideAdaptive(const float* history, int delay3) {
    float work[372]{}; std::copy_n(history, 292, work); float* out = work + 292;
    const int integer = (delay3 + 2) * 10923 >> 15;
    const int phase = delay3 + 2 - 3 * integer + 1;
    const float* source = out - integer + 1;
    for (int n = 0; n < 80; ++n) {
        float sum = 0;
        for (int j = 0; j < 10; ++j) {
            sum += source[n + j] * sinc_win[3 * j + phase];
            sum += source[n - j - 1] * sinc_win[3 * (j + 1) - phase];
        }
        out[n] = sum;
    }
    WideVector result; std::copy_n(out, 80, result.begin()); return result;
}

WideVector WideOutput(const WideVector& excitation, const WideLpc& a,
                      const WideLpc& new_filter, const WideLpc& old_filter,
                      const float* synth_memory, const float* post_memory, int subframe) {
    WideVector synth{}, current{}, previous{};
    WideSynthesis(a, excitation.data(), synth.data(), synth_memory);
    WideSynthesis(new_filter, synth.data(), current.data(), post_memory);
    if (!subframe) {
        WideSynthesis(old_filter, synth.data(), previous.data(), post_memory, 30);
        for (int n = 0; n < 30; ++n) current[n] = float(previous[n] + n / 30. * (current[n] - previous[n]));
    }
    return current;
}

Pair WidePair(unsigned code, int track) {
    Pair pair{{5 * int((code >> 4) & 15) + track, 5 * int(code & 15) + track}, {}};
    pair.sign[0] = code & 256 ? -1.f : 1.f;
    pair.sign[1] = pair.position[1] < pair.position[0] ? -pair.sign[0] : pair.sign[0]; return pair;
}

void WidePulses(const WideVector& target, const WideBasis& basis, unsigned* codes) {
    double correlation[80]{}, gram[80][80]{};
    for (int p = 0; p < 80; ++p) {
        correlation[p] = WideDot(target, basis[p]);
        for (int q = 0; q <= p; ++q) gram[p][q] = gram[q][p] = WideDot(basis[p], basis[q]);
    }
    Pair selected[5]{}; bool active[5]{};
    for (int pass = 0; pass < 3; ++pass) for (int track = 0; track < 5; ++track) {
        int positions[8]{}, count = 0; float signs[8]{}; double other_c = 0, other_e = 0;
        for (int t = 0; t < 5; ++t) if (t != track && active[t])
            for (int i = 0; i < 2; ++i) { positions[count] = selected[t].position[i]; signs[count++] = selected[t].sign[i]; }
        for (int i = 0; i < count; ++i) {
            other_c += signs[i] * correlation[positions[i]];
            for (int j = 0; j < count; ++j) other_e += signs[i] * signs[j] * gram[positions[i]][positions[j]];
        }
        double best = -1; unsigned selected_code = 0;
        for (unsigned code = 0; code < 512; ++code) {
            const Pair pair = WidePair(code, track); const int p = pair.position[0], q = pair.position[1];
            const double s = pair.sign[0], t = pair.sign[1];
            const double c = other_c + s * correlation[p] + t * correlation[q];
            double e = other_e + gram[p][p] + gram[q][q] + 2 * s * t * gram[p][q];
            for (int i = 0; i < count; ++i) e += 2 * signs[i] * (s * gram[p][positions[i]] + t * gram[q][positions[i]]);
            const double score = c > 0 ? c * c / std::max(e, 1e-15) : 0;
            if (score > best) { best = score; selected_code = code; }
        }
        codes[track] = selected_code; selected[track] = WidePair(selected_code, track); active[track] = true;
    }
}

WideVector WideInnovation(const unsigned* codes, int lag, float sharp) {
    WideVector fixed{};
    for (int track = 0; track < 5; ++track) {
        Pair p = WidePair(codes[track], track);
        for (int i = 0; i < 2; ++i) {
            float sign = p.sign[i];
            for (int n = p.position[i]; n < 80; n += lag) { fixed[n] += sign; sign *= sharp; }
        }
    }
    return fixed;
}

struct WideEncoder {
    SiprContext state{};
    std::array<float, 160> history{};
    WideLpc lsf_history{};
    WideEncoder() {
        ff_sipr_init_16k(&state);
        for (int i = 0; i < 4; ++i) state.energy_history[i] = -14;
        std::copy_n(mean_lsf_16k, 16, lsf_history.begin());
    }
    void Block(const float* pcm, std::vector<uint8_t>& output) {
        double r[17]{}, isp[16], first_isp[16];
        WideLpc lsf{};
        if (WideLpcToLsf(WideAnalyze(history.data(), pcm, r), lsf)) lsf_history = lsf;
        SiprParameters params = WideQuantize(state, lsf_history, r, isp);
        for (int j = 0; j < 16; ++j) first_isp[j] = .5 * (isp[j] + state.lsp_history_16k[j]);
        WideLpc a[2] = {WideIspToLpc(first_isp), WideIspToLpc(isp)};
        WideLpc filter{}, old_filter{}, synth_memory{}, post_memory{};
        double scale = .5;
        for (int j = 0; j < 16; ++j) { filter[j] = float(state.iir_mem[j] * scale); scale *= .5; old_filter[j] = state.filt_mem[1][j]; }
        std::copy_n(state.synth, 16, synth_memory.begin()); std::copy_n(state.mem_preemph, 16, post_memory.begin());
        float excitation[292]; std::copy_n(state.excitation, 292, excitation);
        float energies[2] = {state.energy_history[0], state.energy_history[1]};
        int previous_pitch = state.pitch_lag_prev;
        for (int subframe = 0; subframe < 2; ++subframe) {
            const WideLpc zero_memory{}; const WideVector zero{};
            WideVector zero_response = WideOutput(zero, a[subframe], filter, old_filter, synth_memory.data(), post_memory.data(), subframe);
            WideVector target{};
            for (int n = 0; n < 80; ++n) target[n] = pcm[subframe * 80 + n] - zero_response[n];
            target = WideWeight(target, a[subframe]);
            WideBasis basis{};
            for (int p = 0; p < 80; ++p) {
                WideVector impulse{}; impulse[p] = 1;
                basis[p] = WideWeight(WideOutput(impulse, a[subframe], filter, old_filter, zero_memory.data(), zero_memory.data(), subframe), a[subframe]);
            }
            double best_pitch = std::numeric_limits<double>::max();
            WideVector adaptive{}, adaptive_response{}; unsigned pitch_code = 0, pitch_gain_code = 0; int lag = 30;
            // Use the ordinary fractional pitch-search range; do not emit
            // the decoder fallback indices 62/63. This does not reject input.
            for (unsigned code = 0; code < (subframe ? 62u : 512u); ++code) {
                const int d3 = WidePitch(code, previous_pitch, subframe);
                const WideVector v = WideAdaptive(excitation, d3), response = ApplyBasis(basis, v);
                const double norm = WideDot(response, response), corr = WideDot(target, response);
                for (unsigned gain = 0; gain < 16; ++gain) {
                    const double gp = gain_pitch_cb_16k[gain], cost = gp * gp * norm - 2 * gp * corr;
                    if (cost < best_pitch) { best_pitch = cost; pitch_code = code; pitch_gain_code = gain; adaptive = v; adaptive_response = response; lag = (d3 + 1) * 10923 >> 15; }
                }
            }
            const float gp = gain_pitch_cb_16k[pitch_gain_code], sharp = std::min(gp, 1.f);
            WideBasis sharpened = basis;
            for (int p = 0; p < 80; ++p) {
                float factor = sharp;
                for (int q = p + lag; q < 80; q += lag) {
                    for (int n = q; n < 80; ++n) sharpened[p][n] += factor * basis[q][n];
                    factor *= sharp;
                }
            }
            WideVector residual{};
            for (int n = 0; n < 80; ++n) residual[n] = target[n] - gp * adaptive_response[n];
            unsigned codes[5]{}; WidePulses(residual, sharpened, codes);
            const WideVector fixed = WideInnovation(codes, lag, sharp), response = ApplyBasis(basis, fixed);
            const double measured = 10 * std::log10((.01 + WideDot(fixed, fixed)) / 80);
            const double predicted = 19 + .8 * energies[0] + .6 * energies[1];
            const float base = float(std::pow(10., (predicted - measured) * .05) / 32768);
            unsigned gain_code = 0; double best_gain = std::numeric_limits<double>::max();
            const double gain_norm = WideDot(response, response), gain_corr = WideDot(residual, response);
            for (unsigned code = 0; code < 32; ++code) {
                const double gain = base * gain_cb_16k[code], cost = gain * gain * gain_norm - 2 * gain * gain_corr;
                if (cost < best_gain) { best_gain = cost; gain_code = code; }
            }
            params.pitch_delay[subframe] = int(pitch_code); params.gp_index[subframe] = int(pitch_gain_code); params.gc_index[subframe] = int(gain_code);
            for (int track = 0; track < 5; ++track) { params.fc_indexes[subframe][2 * track] = int16_t(codes[track] & 15); params.fc_indexes[subframe][2 * track + 1] = int16_t(codes[track] >> 4); }
            previous_pitch = lag; energies[1] = energies[0]; energies[0] = float(20 * std::log10(gain_cb_16k[gain_code]));
            if (!subframe) {
                SiprContext probe; CloneWide(state, probe); SiprParameters trial = params; float samples[160];
                ff_sipr_decode_frame_16k(&probe, &trial, samples);
                std::move(excitation + 80, excitation + 292, excitation);
                std::copy_n(probe.excitation + 132, 80, excitation + 212);
                std::copy_n(probe.synth_buf + 16 + 64, 16, synth_memory.begin());
                std::copy_n(samples + 64, 16, post_memory.begin());
            }
        }
        float check[160]; ff_sipr_decode_frame_16k(&state, &params, check);
        for (float sample : check) if (!std::isfinite(sample)) throw std::runtime_error("SIPR16 synthesis nonfinite");
        uint8_t packet[20]{}; unsigned bit = 0;
        const auto put = [&](unsigned value, unsigned bits) { for (unsigned i = 0; i < bits; ++i, ++bit) packet[bit >> 3] |= uint8_t(((value >> i) & 1) << (bit & 7)); };
        put(unsigned(params.ma_pred_switch), 1);
        for (int i = 0; i < 5; ++i) put(unsigned(params.vq_indexes[i]), i == 1 ? 8 : 7);
        for (int s = 0; s < 2; ++s) {
            put(unsigned(params.pitch_delay[s]), s ? 6 : 9); put(unsigned(params.gp_index[s]), 4);
            for (int i = 0; i < 10; ++i) put(unsigned(params.fc_indexes[s][i]), i & 1 ? 5 : 4);
            put(unsigned(params.gc_index[s]), 5);
        }
        output.insert(output.end(), packet, packet + 20); std::copy_n(pcm, 160, history.begin());
    }
};

} // namespace

struct SiprEncoder::Impl {
    bool low65, low5;
    int core_samples;
    int lookahead;
    std::unique_ptr<WideEncoder> wide;
    unsigned packet_bits = 0;
    std::array<uint8_t, 37> packet{};
    uint64_t input_frames = 0;
    size_t buffered = 0;
    bool finished = false;
    const char* error = "";
    std::array<float, 320> pcm{};
    Vector analysis_history{};
    std::array<float, kHistory> excitation{};
    Lpc spectrum_history{}, isp{}, analysis_frequencies{}, synthesis{}, energy{};
    float gain_memory = 0;
    float past_pitch_gain = 0;

    explicit Impl(uint32_t mode) : low65(mode == 6500), low5(mode == 5000), core_samples(mode == 16000 ? 160 : low5 ? 240 : 144), lookahead(mode == 16000 ? 160 : 48) {
        if (mode == 16000) wide.reset(new WideEncoder);
        for (int i = 0; i < 10; ++i) {
            isp[i] = float(std::cos((i + 1) * kPi / 11));
            analysis_frequencies[i] = mean_lsf[i];
        }
        if (low5) {
            for (int i = 0; i < 9; ++i) isp[i] = float(std::cos(mean_lsf[i]));
            isp[9] = float(mean_lsf[9] * 6.153848 / kPi);
        }
        std::fill_n(energy.begin(), 4, -14.f);
    }

    void Block(std::vector<uint8_t>& output) {
        if (wide) {
            wide->Block(pcm.data(), output);
            std::copy_n(pcm.begin() + 160, 160, pcm.begin());
            buffered -= 160; return;
        }
        unsigned fields[20]{};
        const Lpc analyzed = Analyze(analysis_history.data(), pcm.data(), core_samples);
        Lpc frequencies{};
        if (LpcToIsf(analyzed, frequencies)) analysis_frequencies = frequencies;
        const Lpc next_isp = QuantizeSpectrum(analysis_frequencies, spectrum_history, fields);
        int first_lag = 0;
        for (int subframe = 0; subframe < core_samples / kSub; ++subframe) {
            const double t = (subframe + .5) / (core_samples / kSub);
            Lpc interpolated{};
            for (int j = 0; j < 10; ++j) interpolated[j] = float((1 - t) * isp[j] + t * next_isp[j]);
            const Lpc a = IspToLpc(interpolated);
            Vector zero{}, zero_response{}, impulse{}, impulse_response{}, target{};
            const Lpc zero_memory{}; impulse[0] = 1;
            Synthesize(a, zero.data(), zero_response.data(), synthesis.data());
            Synthesize(a, impulse.data(), impulse_response.data(), zero_memory.data());
            const Vector weighted_response = Weight(impulse_response, a);
            for (int n = 0; n < kSub; ++n) target[n] = pcm[subframe * kSub + n] - zero_response[n];
            target = Weight(target, a);

            unsigned pitch_code = 0; int lag = 20, fraction = 0;
            Vector adaptive{}, adaptive_response{}; float continuous_pitch = 0;
            double best_pitch = std::numeric_limits<double>::max();
            for (unsigned code = 0; code < ((!subframe || (low5 && subframe == 2)) ? 256u : 32u); ++code) {
                int d, f; Pitch(code, first_lag, subframe, d, f, low5);
                const Vector v = Adaptive(excitation.data(), d, f);
                const Vector response = Convolve(v, weighted_response);
                const double dot = Dot(target, response), norm = Dot(response, response);
                const double gain = std::clamp(dot / std::max(norm, 1e-15), 0., 1.2);
                const double cost = gain * gain * norm - 2 * gain * dot;
                if (cost < best_pitch) {
                    best_pitch = cost; pitch_code = code; lag = d; fraction = f;
                    adaptive = v; adaptive_response = response; continuous_pitch = float(gain);
                }
            }
            (void)fraction;
            if (!subframe || (low5 && subframe == 2)) first_lag = lag;
            const Vector shape = FixedShape(a, lag, low5);
            const Vector fixed_response = Convolve(shape, weighted_response);
            Vector search_target{};
            for (int n = 0; n < kSub; ++n) search_target[n] = target[n] - continuous_pitch * adaptive_response[n];
            unsigned codes[3]{};
            Vector fixed{};
            if (low5) {
                codes[0] = Search5k(search_target, fixed_response, past_pitch_gain < .8f);
                fixed = Innovation5k(shape, codes[0], past_pitch_gain < .8f);
            } else {
                SearchPulses(search_target, fixed_response, codes, low65);
                fixed = Innovation(shape, codes, low65);
            }
            Vector response = Convolve(fixed, weighted_response);

            unsigned gain_code = 0;
            float base = GainBase(fixed, energy);
            double best_gain = std::numeric_limits<double>::max();
            for (unsigned code = 0; code < 128; ++code) {
                const float gp = gain_cb[code][0];
                const float gc = base * gain_cb[code][1] * (low5 ? 1 : 1 - GainMemory(gain_memory, gp));
                double cost = 0;
                for (int n = 0; n < kSub; ++n) {
                    const double delta = target[n] - gp * double(adaptive_response[n]) - gc * double(response[n]);
                    cost += delta * delta;
                }
                if (cost < best_gain) { best_gain = cost; gain_code = code; }
            }
            const float gp = gain_cb[gain_code][0], gc = base * gain_cb[gain_code][1];
            past_pitch_gain = gp;
            gain_memory = GainMemory(gain_memory, gp);
            for (int i = 0; i < 3; ++i) energy[i] = energy[i + 1];
            energy[3] = float(20 * std::log10(gain_cb[gain_code][1]));
            Vector exc{}, synth_input{}, synth{};
            for (int n = 0; n < kSub; ++n) {
                exc[n] = gp * adaptive[n] + gc * fixed[n];
                // The 5k decoder applies a separate postfilter and energy AGC;
                // its encoder search follows the unpostfiltered synthesis.
                synth_input[n] = low5 ? exc[n] : exc[n] - gc * gain_memory * fixed[n];
            }
            Synthesize(a, synth_input.data(), synth.data(), synthesis.data());
            for (float sample : synth)
                if (!std::isfinite(sample)) throw std::runtime_error("SIPR synthesis became nonfinite");
            std::copy_n(synth.end() - kOrder, kOrder, synthesis.begin());
            std::move(excitation.begin() + kSub, excitation.end(), excitation.begin());
            std::copy(exc.begin(), exc.end(), excitation.end() - kSub);
            const int at = 5 + subframe * (low5 ? 3 : 5);
            fields[at] = pitch_code;
            for (int i = 0; i < (low5 ? 1 : 3); ++i) fields[at + i + 1] = codes[i];
            fields[at + (low5 ? 2 : 4)] = gain_code;
        }
        isp = next_isp;
        Pack(fields, packet.data(), packet_bits, low65, low5);
        if (packet_bits == (low5 ? 296u : low65 ? 232u : 152u)) {
            output.insert(output.end(), packet.begin(), packet.begin() + (low5 ? 37 : low65 ? 29 : 19));
            packet.fill(0); packet_bits = 0;
        }
        std::copy_n(pcm.begin() + core_samples - 48, 48, analysis_history.begin());
        std::copy_n(pcm.begin() + core_samples, 48, pcm.begin());
        buffered -= core_samples;
    }
};

SiprEncoder::SiprEncoder(uint32_t mode) noexcept {
    if (mode != 8500 && mode != 6500 && mode != 5000 && mode != 16000) { startup_error_ = "unsupported SIPR mode"; return; }
    mode_ = mode;
    try { ScopedRounding rounding; impl_.reset(new Impl(mode)); }
    catch (...) { startup_error_ = "cannot allocate SIPR encoder"; }
}
SiprEncoder::~SiprEncoder() = default;
bool SiprEncoder::Encode(const int16_t* pcm, size_t frames, std::vector<uint8_t>& packets) noexcept {
    if (!impl_ || *impl_->error) return false;
    if (impl_->finished) { impl_->error = "SIPR encode after finish"; return false; }
    if ((!pcm && frames) || frames > std::numeric_limits<uint64_t>::max() - impl_->input_frames) {
        impl_->error = "invalid SIPR input"; return false;
    }
    impl_->input_frames += frames;
    try {
        ScopedRounding rounding;
        while (frames) {
            const size_t capacity = impl_->core_samples + impl_->lookahead;
            const size_t count = std::min(frames, capacity - impl_->buffered);
            for (size_t i = 0; i < count; ++i) impl_->pcm[impl_->buffered + i] = pcm[i] * (1.f / 32768);
            impl_->buffered += count; pcm += count; frames -= count;
            if (impl_->buffered == capacity) impl_->Block(packets);
        }
        return true;
    } catch (...) { impl_->error = "SIPR encoding failed"; return false; }
}
bool SiprEncoder::Finish(std::vector<uint8_t>& packets) noexcept {
    if (!impl_ || *impl_->error) return false;
    if (impl_->finished) return true;
    try {
        ScopedRounding rounding;
        size_t remaining = impl_->buffered;
        size_t available = impl_->buffered;
        if (impl_->low5 && impl_->input_frames) remaining += 240;
        while (remaining || impl_->packet_bits) {
            std::fill(impl_->pcm.begin() + available, impl_->pcm.end(), 0.f);
            impl_->buffered = impl_->core_samples + impl_->lookahead;
            impl_->Block(packets);
            remaining = remaining > size_t(impl_->core_samples) ? remaining - impl_->core_samples : 0;
            available = available > size_t(impl_->core_samples) ? available - impl_->core_samples : 0;
            impl_->buffered = available;
        }
        impl_->finished = true; return true;
    } catch (...) { impl_->error = "SIPR finalization failed"; return false; }
}
uint64_t SiprEncoder::InputFrames() const noexcept { return impl_ ? impl_->input_frames : 0; }
const char* SiprEncoder::Error() const noexcept { return impl_ ? impl_->error : startup_error_; }
}
