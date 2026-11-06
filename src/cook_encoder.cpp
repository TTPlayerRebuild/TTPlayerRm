/*
 * Independent Cook encoding decisions and packet writer, 2026.
 * Codebook data and categorization algorithm are derived from FFmpeg's
 * LGPL-2.1-or-later Cook decoder, Copyright (c) 2003 Sascha Sommer and
 * (c) 2005 Benjamin Larsson. See libavcodec/cook.c and cookdata.h.
 * This source is distributed under LGPL-2.1-or-later.
 */
#include "cook_encoder.h"
#include <algorithm>
#include <array>
#include <cfenv>
#include <cmath>
#include <limits>
#include <stdexcept>
extern "C" {
#include "libavutil/tx.h"
}
#include "libavcodec/cookdata.h"

namespace ttp::rm {
namespace {
constexpr int kMaxN = 1024, kMaxBands = 53, kMaxBytes = 283;
struct Config { uint32_t flavor, rate, samples, bytes, bands, version, bitrate, channels = 1, joint_start = 0, coupling_bits = 0; };
constexpr Config kConfigs[] = {
    {0,8000,256,32,12,1,8000}, {1,11025,256,32,12,1,11025},
    {2,22050,512,47,18,1,16193}, {3,22050,512,60,23,1,20672},
    {4,44100,1024,93,37,1,32041}, {5,44100,1024,128,47,2,44100},
    {6,44100,1024,186,47,2,64083}, {7,22050,512,93,24,2,32041},
    {8,8000,256,24,9,2,6000}, {9,11025,256,58,11,2,19983,2},
    {10,22050,512,94,18,2,32386,2}, {11,22050,512,128,24,2,44100,2},
    {12,44100,1024,186,37,2,64083,2}, {13,44100,1024,278,47,2,95780,2},
    {14,44100,1024,186,47,2,64083},
    {15,44100,1024,60,47,2,20672}, {16,44100,1024,93,47,2,32041},
    {17,22050,512,48,16,3,16538,2,1,3},
    {18,22050,512,60,20,3,20672,2,1,3},
    {19,22050,512,60,23,3,20672,2,1,3},
    {20,22050,512,93,24,3,32041,2,2,4},
    {21,44100,1024,93,32,3,32041,2,2,4},
    {22,44100,1024,128,32,3,44100,2,5,5},
    {23,44100,1024,128,37,3,44100,2,2,4},
    {24,44100,1024,186,37,3,64083,2,6,5},
    {25,44100,1024,280,37,3,96469,2,8,5},
    {26,11025,256,36,9,3,12403,2,1,2},
    {27,44100,1024,186,30,3,64083,2,17,5},
    {28,44100,1024,280,34,3,96469,2,19,5},
    {29,22050,512,128,23,3,44100,2,17,5},
    {30,44100,1024,279,0,2,96124,5},
    {31,44100,1024,381,0,2,131266,6},
    {32,44100,1024,532,0,2,183291,6},
    {33,44100,1024,778,0,2,268045,6}
};
class ScopedRounding {
    int previous_;
public:
    ScopedRounding() : previous_(std::fegetround()) { std::fesetround(FE_TONEAREST); }
    ~ScopedRounding() { if (previous_ != -1) std::fesetround(previous_); }
};
struct Code { unsigned bits = 0; int length = 0; };
struct Tuple {
    Code code;
    std::array<unsigned char, 5> digit{};
    int bit_count = 0;
};
struct Book {
    std::array<std::array<Code, 24>, 13> envelope{};
    std::array<std::vector<Tuple>, 7> tuples;
    Book() {
        for (int table = 0; table < 13; ++table) {
            unsigned code = 0; int index = 0;
            for (int length = 1; length <= 16; ++length) {
                for (int j = 0; j < envelope_quant_index_huffcounts[table][length - 1]; ++j)
                    envelope[table][envelope_quant_index_huffsyms[table][index++]] = {code++, length};
                code <<= 1;
            }
        }
        for (int category = 0; category < 7; ++category) {
            unsigned code = 0; int index = 0;
            for (int length = 1; length <= 16; ++length) {
                for (int j = 0; j < cvh_huffcounts[category][length - 1]; ++j) {
                    unsigned symbol = category == 3 ? static_cast<const uint16_t*>(cvh_huffsyms[category])[index++] :
                                                      static_cast<const uint8_t*>(cvh_huffsyms[category])[index++];
                    Tuple t; t.code = {code++, length}; t.bit_count = length;
                    for (int d = vd_tab[category] - 1; d >= 0; --d) {
                        t.digit[d] = static_cast<unsigned char>(symbol % (kmax_tab[category] + 1));
                        symbol /= kmax_tab[category] + 1;
                        t.bit_count += t.digit[d] != 0;
                    }
                    tuples[category].push_back(t);
                }
                code <<= 1;
            }
        }
    }
};
struct BitWriter {
    std::array<uint8_t, kMaxBytes> data{};
    int used = 0, limit;
    explicit BitWriter(int bytes) : limit(bytes * 8) {}
    void put(unsigned value, int count) {
        if (count < 0 || count > 24 || used + count > limit)
            throw std::runtime_error("Cook bit budget exceeded");
        while (count--) {
            data[used >> 3] |= ((value >> count) & 1) << (7 - (used & 7));
            ++used;
        }
    }
    void put(Code code) { if (!code.length) throw std::runtime_error("missing Cook codeword"); put(code.bits, code.length); }
    void spectral(unsigned value, int count, bool bounded_tail) {
        if (bounded_tail && count > limit - used) {
            const int remaining = limit - used;
            put(value >> (count - remaining), remaining);
        } else put(value, count);
    }
};
int category_clip(int v) { return std::max(0, std::min(7, v)); }
// Preserve the decoder's division toward zero and tie-breaking exactly.
int categorize(const std::array<int, kMaxBands>& power, int bands, int samples, int vectors, int bits_left,
                std::array<int, kMaxBands>& category, std::array<int, 127>& changes) {
    std::array<int, kMaxBands> high{}, low{};
    std::array<int, 256> history{};
    int up = vectors, down = vectors, bias = -32;
    if (bits_left > samples) bits_left = samples + ((bits_left - samples) * 5) / 8;
    for (int step = 32; step; step /= 2) {
        int bits = 0;
        for (int i = 0; i < bands; ++i) bits += expbits_tab[category_clip((step - power[i] + bias) / 2)];
        if (bits >= bits_left - 32) bias += step;
    }
    int high_bits = 0;
    for (int i = 0; i < bands; ++i) {
        high[i] = low[i] = category_clip((bias - power[i]) / 2);
        high_bits += expbits_tab[high[i]];
    }
    int low_bits = high_bits;
    for (int n = 1; n < vectors; ++n) {
        int chosen = -1;
        if (high_bits + low_bits > 2 * bits_left) {
            int maximum = -999999;
            for (int i = 0; i < bands; ++i) if (high[i] < 7) {
                int value = -2 * high[i] - power[i] + bias;
                if (value >= maximum) { maximum = value; chosen = i; }
            }
            if (chosen < 0) break;
            history[up++] = chosen;
            high_bits -= expbits_tab[high[chosen]] - expbits_tab[high[chosen] + 1];
            ++high[chosen];
        } else {
            int minimum = 999999;
            for (int i = 0; i < bands; ++i) if (low[i] > 0) {
                int value = -2 * low[i] - power[i] + bias;
                if (value < minimum) { minimum = value; chosen = i; }
            }
            if (chosen < 0) break;
            history[--down] = chosen;
            low_bits -= expbits_tab[low[chosen]] - expbits_tab[low[chosen] - 1];
            --low[chosen];
        }
    }
    category = low;
    const int transitions = up - down;
    for (int i = 0; i < transitions; ++i) changes[i] = history[down++];
    return transitions;
}
struct VectorChoice { double error = 0.; int tuple = 0; };
struct BandChoice {
    double error = 0.; int bits = 0;
    std::array<int, 10> tuple{};
};
}

struct CookEncoder::Impl {
    Config config;
    uint32_t channels;
    std::unique_ptr<Impl> right;
    std::vector<std::unique_ptr<Impl>> parts;
    std::vector<unsigned> part_offsets;
    Book book;
    AVTXContext* transform = nullptr;
    av_tx_fn forward = nullptr;
    std::vector<uint8_t> extra;
    std::array<float, kMaxN> current{}, incoming{}, window{};
    std::array<float, kMaxBands * 20> coefficients{};
    std::array<int, 20> coupling{};
    std::array<float, 2 * kMaxN> input{};
    std::array<int, 9> packet_gain{};
    std::array<float, kMaxN> current_curve{}, next_curve{};
    size_t buffered = 0;
    uint64_t input_frames = 0;
    bool started = false, finished = false;
    const char* error = "";

    explicit Impl(Config value) : config(value), channels(value.channels) {
        ScopedRounding rounding;
        const int samples = int(config.samples);
        if (channels > 2) {
            switch (config.flavor) {
            case 30: 
                add_part(Config{30,44100,1024,93,32,3,0,2,2,4},0x3,0);
                add_part(Config{30,44100,1024,92,47,2,0,1,0,0},0x4,2);
                add_part(Config{30,44100,1024,92,32,3,0,2,2,4},0x30,3);
                break;
            case 31: 
                add_part(Config{31,44100,1024,130,37,3,0,2,2,4},0x3,0);
                add_part(Config{31,44100,1024,126,47,2,0,1,0,0},0x4,2);
                add_part(Config{31,44100,1024,30,1,2,0,1,0,0},0x8,3);
                add_part(Config{31,44100,1024,92,32,3,0,2,2,4},0x30,4);
                break;
            case 32: 
                add_part(Config{32,44100,1024,189,37,3,0,2,6,5},0x3,0);
                add_part(Config{32,44100,1024,184,47,2,0,1,0,0},0x4,2);
                add_part(Config{32,44100,1024,30,1,2,0,1,0,0},0x8,3);
                add_part(Config{32,44100,1024,126,37,3,0,2,2,4},0x30,4);
                break;
            case 33: 
                add_part(Config{33,44100,1024,283,37,3,0,2,8,5},0x3,0);
                add_part(Config{33,44100,1024,184,47,2,0,1,0,0},0x4,2);
                add_part(Config{33,44100,1024,30,1,2,0,1,0,0},0x8,3);
                add_part(Config{33,44100,1024,278,37,3,0,2,8,5},0x30,4);
                break;
            default: throw std::runtime_error("unsupported Cook multichannel layout");
            }
            unsigned total = unsigned(parts.size() - 1);
            for (const auto& part : parts) total += part->packet_bytes();
            if (total != config.bytes) throw std::runtime_error("Cook multichannel packet size mismatch");
            return;
        }
        const unsigned values = samples * channels;
        extra = {1, 0, 0, uint8_t(config.version), uint8_t(values >> 8), uint8_t(values), 0, uint8_t(config.bands)};
        if (config.coupling_bits) {
            extra.insert(extra.end(), {0,0,0,0,0,uint8_t(config.joint_start),0,uint8_t(config.coupling_bits)});
        }
        if (channels == 2) {
            if (!config.coupling_bits) config.bytes /= 2;
            Config mono = config; mono.channels = 1; mono.joint_start = 0; mono.coupling_bits = 0;
            right.reset(new Impl(mono));
        }
        current_curve.fill(1.f);
        next_curve.fill(1.f);
        const float scale = 1.f;
        if (av_tx_init(&transform, &forward, AV_TX_FLOAT_MDCT, 0, samples, &scale, AV_TX_UNALIGNED) < 0)
            throw std::runtime_error("Cook transform initialization failed");
        for (int n = 0; n < samples; ++n)
            window[n] = static_cast<float>(std::sin((n + .5) * 3.14159265358979323846 / (2 * samples)) * std::sqrt(2. / samples));
    }
    ~Impl() { av_tx_uninit(&transform); }
    unsigned packet_bytes() const {
        return config.bytes * (channels > 2 || config.coupling_bits ? 1 : channels);
    }
    void add_part(Config value, unsigned mask, unsigned offset) {
        std::unique_ptr<Impl> part(new Impl(value));
        const unsigned samples = value.samples * value.channels;
        const uint8_t record[20] = {2,0,0,0,uint8_t(samples >> 8),uint8_t(samples),0,uint8_t(value.bands),
            0,0,0,0,0,uint8_t(value.joint_start),0,uint8_t(value.coupling_bits),
            uint8_t(mask >> 24),uint8_t(mask >> 16),uint8_t(mask >> 8),uint8_t(mask)};
        extra.insert(extra.end(), record, record + 20);
        part_offsets.push_back(offset); parts.push_back(std::move(part));
    }
    void append_pcm(const int16_t* data, size_t count) {
        if (parts.empty()) {
            for (size_t i = 0; i < count; ++i) {
                incoming[buffered + i] = data[i * channels];
                if (right) right->incoming[buffered + i] = data[i * channels + 1];
            }
        } else {
            for (size_t p = 0; p < parts.size(); ++p) for (size_t i = 0; i < count; ++i) {
                auto& part = *parts[p];
                part.incoming[buffered + i] = data[i * channels + part_offsets[p]];
                if (part.right) part.right->incoming[buffered + i] = data[i * channels + part_offsets[p] + 1];
            }
        }
    }
    void multi_block(std::vector<uint8_t>& output) {
        std::vector<std::vector<uint8_t>> packets(parts.size());
        size_t count = 0;
        for (size_t p = 0; p < parts.size(); ++p) {
            parts[p]->block(packets[p]);
            const unsigned bytes = parts[p]->packet_bytes();
            if (packets[p].size() % bytes) throw std::runtime_error("Cook subpacket alignment");
            if (p == 0) count = packets[p].size() / bytes;
            else if (packets[p].size() / bytes != count) throw std::runtime_error("Cook subpacket count mismatch");
        }
        for (size_t packet = 0; packet < count; ++packet) {
            for (size_t p = 0; p < parts.size(); ++p) {
                const unsigned bytes = parts[p]->packet_bytes();
                output.insert(output.end(), packets[p].begin() + packet * bytes, packets[p].begin() + (packet + 1) * bytes);
            }
            for (size_t p = 1; p < parts.size(); ++p) {
                const unsigned bytes = parts[p]->packet_bytes();
                if ((bytes & 1) || bytes > 510) throw std::runtime_error("Cook secondary subpacket size");
                output.push_back(uint8_t(bytes / 2));
            }
        }
        buffered = 0;
    }

    // Independent attack detector: compare segment energy to the quieter
    // segments, normalize large transients, then hold/release gain smoothly.
    // This uses Cook's normal gain syntax; it does not reproduce RealCodec's
    // psychoacoustic decisions.
    void choose_gain() {
        std::array<double, 8> rms{}, ordered{}, peaks{};
        const unsigned piece = config.samples / 8;
        for (unsigned band = 0; band < 8; ++band) {
            double sum = 0.;
            for (unsigned j = 0; j < piece; ++j) {
                double x = incoming[band * piece + j]; sum += x * x;
                peaks[band] = std::max(peaks[band], std::fabs(x));
                if (config.coupling_bits) {
                    double y = right->incoming[band * piece + j]; sum += y * y;
                    peaks[band] = std::max(peaks[band], std::fabs(y));
                }
            }
            rms[band] = std::sqrt(sum / (piece * (config.coupling_bits ? 2 : 1)));
        }
        ordered = rms; std::sort(ordered.begin(), ordered.end());
        const double target = std::max(200., ordered[2] * 4.);
        packet_gain.fill(0);
        for (unsigned band = 0; band < 8; ++band) {
            int gain = rms[band] > target && peaks[band] > 3000. ? int(std::floor(std::log2(peaks[band] / 3000.))) : 0;
            gain = std::min(7, gain);
            for (unsigned k = band; k < std::min(8u, band + 4); ++k)
                packet_gain[k] = std::max(packet_gain[k], gain);
            if (band + 4 < 8) packet_gain[band + 4] = std::max(packet_gain[band + 4], std::max(0, gain - 2));
        }
        for (unsigned band = 0; band < 8; ++band) {
            float value = std::pow(2.f, float(packet_gain[band]));
            const float step = std::pow(2.f, float(packet_gain[band + 1] - packet_gain[band]) / piece);
            for (unsigned j = 0; j < piece; ++j) {
                next_curve[band * piece + j] = value; value *= step;
            }
        }
    }

    BandChoice quantize(int band, int category, int power, double lambda) const {
        BandChoice result;
        const double scale = std::pow(2., power * .5);
        const float* values = coefficients.data() + band * 20;
        if (category == 7) {
            for (int i = 0; i < 20; ++i) result.error += double(values[i]) * values[i] + .5 * scale * scale;
            return result;
        }
        const int dim = vd_tab[category];
        const double noise = dither_tab[category] * scale;
        for (int v = 0; v < vpr_tab[category]; ++v) {
            double best = std::numeric_limits<double>::max(), distortion = 0.;
            int chosen = 0;
            for (size_t t = 0; t < book.tuples[category].size(); ++t) {
                const Tuple& tuple = book.tuples[category][t];
                double err = 0.;
                for (int d = 0; d < dim; ++d) {
                    double diff = std::fabs(values[v * dim + d]) - quant_centroid_tab[category][tuple.digit[d]] * scale;
                    err += diff * diff;
                    if (!tuple.digit[d]) err += noise * noise;
                }
                double cost = err + lambda * tuple.bit_count;
                if (cost < best) { best = cost; chosen = int(t); distortion = err; }
            }
            result.tuple[v] = chosen;
            result.bits += book.tuples[category][chosen].bit_count;
            result.error += distortion;
        }
        return result;
    }

    int envelope_table(int band) const {
        if (config.coupling_bits) {
            if (band >= int(config.joint_start * 2)) band -= config.joint_start;
            else band = std::max(1, band / 2);
        }
        return std::min(band, 13) - 1;
    }
    void packet(std::vector<uint8_t>& output) {
        std::array<int, kMaxBands> initial{}, power{};
        const int bands = int(config.bands + config.joint_start), budget = int(config.bytes * 8);
        const int control_bits = config.coupling_bits ? (config.samples > 512 ? 7 : config.samples > 256 ? 6 : 5) : 5;
        const int vectors = 1 << control_bits;
        const int coupling_start = config.coupling_bits ? cplband[config.joint_start] : 0;
        const int coupling_end = config.coupling_bits ? cplband[config.bands - 1] : -1;
        const int coupling_cost = config.coupling_bits ? 1 + (coupling_end - coupling_start + 1) * config.coupling_bits : 0;
        int gain_points = 0;
        for (int i = 0; i < 8; ++i) gain_points += packet_gain[i] != packet_gain[i + 1];
        const int gain_bits = 1 + 9 * gain_points;
        double energy = 0.;
        for (int band = 0; band < bands; ++band) {
            double sum = 0.;
            for (int j = 0; j < 20; ++j) { double v = coefficients[band * 20 + j]; sum += v * v; }
            energy += sum;
            initial[band] = sum > 0 ? int(std::floor(std::log2(sum / 20.) + .5)) : -63;
            initial[band] = std::max(-63, std::min(57, initial[band]));
        }
        // Usually the original envelope fits. Increasingly smooth upper
        // envelopes provide a bounded response to unusually expensive headers.
        for (int smoothing : {11, 8, 4, 2, 1, 0}) {
            power = initial;
            for (int b = bands - 1; b > 0; --b) power[b - 1] = std::max(power[b - 1], power[b] - smoothing);
            power[0] = std::max(-6, power[0]);
            for (int b = 1; b < bands; ++b) power[b] = std::max(power[b], power[b - 1] - (smoothing ? smoothing + 1 : 0));
            int header_bits = gain_bits + coupling_cost + 6 + control_bits;
            for (int b = 1; b < bands; ++b) header_bits += book.envelope[envelope_table(b)][power[b] - power[b - 1] + 12].length;
            if (header_bits >= budget) continue;
            std::array<int, kMaxBands> base{}, chosen{};
            std::array<int, 127> changes{};
            const int transitions = categorize(power, bands, int(config.samples), vectors, budget - header_bits, base, changes);
            // Do not select undefined transitions. In particular, the original
            // single-band LFE decoder does not tolerate FFmpeg's zero-filled
            // extension of the categorization history.
            const int choices = std::min(vectors, transitions + 1);
            static const int minimum_band_bits[8] = {10,10,10,10,10,8,4,0};
            auto minimum_cats = base; int minimum_total = budget + 1;
            for (int control = 0; control < choices; ++control) {
                int bits = header_bits;
                for (int b = 0; b < bands; ++b) bits += minimum_band_bits[minimum_cats[b]];
                minimum_total = std::min(minimum_total, bits);
                if (control < choices - 1) minimum_cats[changes[control]] = std::min(7, minimum_cats[changes[control]] + 1);
            }
            // Cook defines a noise-coded spectral tail when the fixed packet
            // ends within a vector. RealCodec's writer60F162F0 explicitly
            // writes the remaining high bits of that word, then stops. This
            // is needed at low bitrates when even the shortest complete VQ
            // payload cannot fit. Keep the true energy envelope; do not lower
            // signal level merely to avoid the format's bounded-tail syntax.
            bool bounded_tail = minimum_total > budget;
            std::array<std::array<BandChoice, 8>, kMaxBands> options;
            for (int attempt = 0; attempt < 18; ++attempt) {
                double lambda = attempt ? std::max(1.e-12, energy / budget) * std::pow(2., attempt - 8) : 0.;
                for (int b = 0; b < bands; ++b)
                    for (int cat = 0; cat < 8; ++cat) options[b][cat] = quantize(b, cat, power[b], lambda);
                auto cats = base;
                int best_control = -1;
                double best_error = std::numeric_limits<double>::max();
                for (int control = 0; control < choices; ++control) {
                    int bits = header_bits; double error_sum = 0.;
                    for (int b = 0; b < bands; ++b) { bits += options[b][cats[b]].bits; error_sum += options[b][cats[b]].error; }
                    if (bits <= budget && error_sum < best_error) { best_error = error_sum; best_control = control; chosen = cats; }
                    if (control < choices - 1) cats[changes[control]] = std::min(7, cats[changes[control]] + 1);
                }
                // Preserve transient spectral structure at maximum rate
                // control instead of biasing every vector toward zero just
                // to force a complete payload. Cook's bounded tail supplies
                // the remaining noise bands, as in the original writer.
                if (bounded_tail || (best_control < 0 && gain_points)) {
                    bounded_tail = true; best_control = choices - 1; chosen = cats;
                }
                if (best_control < 0) continue;
                BitWriter writer(int(config.bytes));
                writer.put((1u << gain_points) - 1, gain_points);
                writer.put(0, 1);
                for (int i = 0; i < 8; ++i) if (packet_gain[i] != packet_gain[i + 1]) {
                    writer.put(i, 3); writer.put(1, 1); writer.put(packet_gain[i] + 7, 4);
                }
                if (config.coupling_bits) {
                    writer.put(0, 1); // raw coupling indices
                    for (int group = coupling_start; group <= coupling_end; ++group)
                        writer.put(coupling[group], config.coupling_bits);
                }
                writer.put(power[0] + 6, 6);
                for (int b = 1; b < bands; ++b) writer.put(book.envelope[envelope_table(b)][power[b] - power[b - 1] + 12]);
                writer.put(best_control, control_bits);
                for (int b = 0; b < bands; ++b) {
                    int cat = chosen[b];
                    if (cat == 7) continue;
                    for (int v = 0; v < vpr_tab[cat]; ++v) {
                        const Tuple& t = book.tuples[cat][options[b][cat].tuple[v]];
                        writer.spectral(t.code.bits, t.code.length, bounded_tail);
                        for (int d = 0; d < vd_tab[cat]; ++d)
                            if (t.digit[d]) writer.spectral(coefficients[b * 20 + v * vd_tab[cat] + d] < 0, 1, bounded_tail);
                    }
                }
                static const uint8_t xor_bytes[4] = {0x37, 0xc5, 0x11, 0xf2};
                for (uint32_t i = 0; i < config.bytes; ++i) output.push_back(writer.data[i] ^ xor_bytes[i & 3]);
                return;
            }
        }
        throw std::runtime_error("Cook cannot quantize within packet budget");
    }
    void transform_channel() {
        const float gain0 = std::pow(2.f, float(packet_gain[0]));
        for (uint32_t n = 0; n < config.samples; ++n) {
            input[n] = incoming[n] / next_curve[n] * window[config.samples - 1 - n];
            input[n + config.samples] = -current[n] / (current_curve[n] * gain0) * window[n];
        }
        forward(transform, coefficients.data(), input.data(), sizeof(float));
        current = incoming; current_curve = next_curve;
        incoming.fill(0.f); buffered = 0;
    }
    void channel_block(std::vector<uint8_t>& output) {
        if (!started) { coefficients.fill(0.f); packet(output); started = true; }
        choose_gain(); transform_channel(); packet(output);
    }
    void couple() {
        std::array<float, kMaxBands * 20> combined{};
        for (unsigned band = 0; band < config.joint_start; ++band)
            for (unsigned k = 0; k < 20; ++k) {
                combined[band * 40 + k] = coefficients[band * 20 + k];
                combined[band * 40 + 20 + k] = right->coefficients[band * 20 + k];
            }
        const float* scales = cplscales[config.coupling_bits - 2];
        const int maximum = (1 << config.coupling_bits) - 1;
        for (int group = cplband[config.joint_start]; group <= cplband[config.bands - 1]; ++group) {
            double best = std::numeric_limits<double>::max(); int selected = 0;
            for (int index = 0; index < maximum; ++index) {
                const double l = scales[index + 1], rr = scales[maximum - index];
                double error_sum = 0.;
                for (unsigned band = config.joint_start; band < config.bands; ++band) if (cplband[band] == group)
                    for (unsigned k = 0; k < 20; ++k) {
                        const double a = coefficients[band * 20 + k], b = right->coefficients[band * 20 + k];
                        const double x = (l * a + rr * b) / (l * l + rr * rr);
                        const double da = a - l * x, db = b - rr * x;
                        error_sum += da * da + db * db;
                    }
                if (error_sum < best) { best = error_sum; selected = index; }
            }
            coupling[group] = selected;
            const float l = scales[selected + 1], rr = scales[maximum - selected];
            for (unsigned band = config.joint_start; band < config.bands; ++band) if (cplband[band] == group)
                for (unsigned k = 0; k < 20; ++k)
                    combined[(config.joint_start + band) * 20 + k] =
                        (l * coefficients[band * 20 + k] + rr * right->coefficients[band * 20 + k]) / (l * l + rr * rr);
        }
        coefficients = combined;
    }
    void joint_block(std::vector<uint8_t>& output) {
        if (!started) { coefficients.fill(0.f); packet(output); started = true; }
        choose_gain(); right->packet_gain = packet_gain; right->next_curve = next_curve;
        transform_channel(); right->transform_channel(); couple(); packet(output);
    }
    void block(std::vector<uint8_t>& output) {
        if (!parts.empty()) { multi_block(output); return; }
        if (config.coupling_bits) { joint_block(output); return; }
        if (channels == 1) { channel_block(output); return; }
        std::vector<uint8_t> left_packets, right_packets;
        channel_block(left_packets);
        right->channel_block(right_packets);
        if (left_packets.size() != right_packets.size() || left_packets.size() % config.bytes)
            throw std::runtime_error("Cook stereo channel packet mismatch");
        for (size_t offset = 0; offset < left_packets.size(); offset += config.bytes) {
            output.insert(output.end(), left_packets.begin() + offset, left_packets.begin() + offset + config.bytes);
            output.insert(output.end(), right_packets.begin() + offset, right_packets.begin() + offset + config.bytes);
        }
    }
};

CookEncoder::CookEncoder(uint32_t flavor) noexcept {
    const Config* config = nullptr;
    for (const auto& candidate : kConfigs) if (candidate.flavor == flavor) { config = &candidate; break; }
    if (!config) { startup_error_ = "unsupported Cook flavor"; return; }
    try { impl_.reset(new Impl(*config)); } catch (...) { startup_error_ = "cannot initialize Cook encoder"; }
}
CookEncoder::~CookEncoder() = default;
bool CookEncoder::Encode(const int16_t* data, size_t frames, std::vector<uint8_t>& packets) noexcept {
    if (!impl_ || *impl_->error) return false;
    auto& s = *impl_;
    if (s.finished) { s.error = "Cook encoder already finished"; return false; }
    if ((!data && frames) || frames > std::numeric_limits<size_t>::max() / (sizeof(int16_t) * s.channels) ||
        frames > std::numeric_limits<uint64_t>::max() - s.input_frames - 4 * kMaxN) {
        s.error = "invalid Cook PCM input"; return false;
    }
    size_t previous = packets.size(), submitted = frames;
    try {
        ScopedRounding rounding;
        while (frames) {
            size_t n = std::min(frames, size_t(s.config.samples) - s.buffered);
            s.append_pcm(data, n);
            s.buffered += n; frames -= n; data += n * s.channels;
            if (s.buffered == s.config.samples) s.block(packets);
        }
        s.input_frames += submitted;
        return true;
    } catch (...) { packets.resize(previous); s.error = "Cook encoding failed"; return false; }
}
bool CookEncoder::Finish(std::vector<uint8_t>& packets) noexcept {
    if (!impl_ || *impl_->error) return false;
    auto& s = *impl_;
    if (s.finished) return true;
    size_t previous = packets.size();
    try {
        ScopedRounding rounding;
        if (s.input_frames) {
            if (s.buffered) s.block(packets);
            s.block(packets); // overlap of the final real block with zero
            s.block(packets); // publish the original decoder's pending block
        }
        s.finished = true;
        return true;
    } catch (...) { packets.resize(previous); s.error = "Cook finalization failed"; return false; }
}
uint32_t CookEncoder::FrameBytes() const noexcept { return impl_ ? impl_->packet_bytes() : 0; }
uint32_t CookEncoder::FrameSamples() const noexcept { return impl_ ? impl_->config.samples : 0; }
uint32_t CookEncoder::SampleRate() const noexcept { return impl_ ? impl_->config.rate : 0; }
uint32_t CookEncoder::Channels() const noexcept { return impl_ ? impl_->channels : 0; }
uint32_t CookEncoder::Bitrate() const noexcept { return impl_ ? impl_->config.bitrate : 0; }
uint32_t CookEncoder::DelaySamples() const noexcept { return impl_ ? 3 * impl_->config.samples : 0; }
uint64_t CookEncoder::InputFrames() const noexcept { return impl_ ? impl_->input_frames : 0; }
const std::vector<uint8_t>& CookEncoder::ExtraData() const noexcept { static const std::vector<uint8_t> empty; return impl_ ? impl_->extra : empty; }
const char* CookEncoder::Error() const noexcept { return impl_ ? impl_->error : startup_error_; }
}
