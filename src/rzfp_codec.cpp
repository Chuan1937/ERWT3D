#include "erwt3d/rzfp_codec.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

#ifdef ERWT3D_HAVE_RZFP
#include <zfp.h>
#include <zfp/bitstream.h>
#endif

namespace erwt3d {

namespace {

static uint64_t buildMandatoryExceptionMask(
    const float input[64],
    uint64_t valid_mask
) {
    uint64_t mask = 0;
    for (uint32_t i = 0; i < 64; ++i) {
        if ((valid_mask & (uint64_t{1} << i)) == 0) continue;
        const float value = input[i];
        if (
            value == 0.0f ||
            !std::isfinite(value) ||
            std::fpclassify(value) == FP_SUBNORMAL
        ) {
            mask |= uint64_t{1} << i;
        }
    }
    return mask;
}

static float computeFillValue(
    const float input[64],
    uint64_t valid_mask,
    uint64_t exception_mask,
    RzfpExceptionFill fill
) {
    if (fill == RzfpExceptionFill::Zero) return 0.0f;
    double sum = 0.0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < 64; ++i) {
        const uint64_t bit = uint64_t{1} << i;
        if ((valid_mask & bit) == 0 || (exception_mask & bit) != 0) continue;
        if (!std::isfinite(input[i])) continue;
        sum += input[i];
        ++count;
    }
    return count == 0 ? 0.0f : static_cast<float>(sum / count);
}

static bool patchExceptions(
    float decoded[64],
    uint64_t exception_mask,
    const float* values,
    uint8_t exc_count
) {
    uint8_t pos = 0;
    for (uint32_t i = 0; i < 64; ++i) {
        if (exception_mask & (uint64_t{1} << i)) {
            if (pos >= exc_count) return false;
            decoded[i] = values[pos++];
        }
    }
    return pos == exc_count;
}

static int16_t safeToleranceExponent(double target_tolerance) {
    if (!(target_tolerance > 0.0)) {
        throw std::invalid_argument("target tolerance must be positive");
    }
    const double e = std::floor(std::log2(target_tolerance));
    if (e < std::numeric_limits<int16_t>::min() ||
        e > std::numeric_limits<int16_t>::max()) {
        throw std::overflow_error("tolerance exponent out of range");
    }
    return static_cast<int16_t>(e);
}

static bool allFiniteNormalNonZero(const float input[64], uint64_t valid_mask) {
    for (uint32_t i = 0; i < 64; ++i) {
        if ((valid_mask & (uint64_t{1} << i)) == 0) continue;
        const float v = input[i];
        if (v == 0.0f || !std::isfinite(v) || std::fpclassify(v) == FP_SUBNORMAL) {
            return false;
        }
    }
    return true;
}

static bool allEqual(const float input[64], uint64_t valid_mask, float& value) {
    bool first = true;
    float ref = 0.0f;
    for (uint32_t i = 0; i < 64; ++i) {
        if ((valid_mask & (uint64_t{1} << i)) == 0) continue;
        if (first) {
            ref = input[i];
            first = false;
        } else if (input[i] != ref) {
            return false;
        }
    }
    if (first) return false;
    value = ref;
    return true;
}

static bool allZero(const float input[64], uint64_t valid_mask) {
    for (uint32_t i = 0; i < 64; ++i) {
        if ((valid_mask & (uint64_t{1} << i)) == 0) continue;
        if (input[i] != 0.0f) return false;
    }
    return true;
}

static double minAbsNonException(
    const float input[64],
    uint64_t valid_mask,
    uint64_t exception_mask
) {
    double min_abs = std::numeric_limits<double>::infinity();
    for (uint32_t i = 0; i < 64; ++i) {
        const uint64_t bit = uint64_t{1} << i;
        if ((valid_mask & bit) == 0 || (exception_mask & bit) != 0) continue;
        min_abs = std::min(min_abs, std::abs(static_cast<double>(input[i])));
    }
    return min_abs;
}

static std::vector<uint8_t> buildAccuracyRecord(
    int8_t min_exp,
    const std::vector<uint8_t>& zfp_payload
) {
    std::vector<uint8_t> record;
    record.reserve(1 + zfp_payload.size());
    record.push_back(static_cast<uint8_t>(min_exp));
    record.insert(record.end(), zfp_payload.begin(), zfp_payload.end());
    return record;
}

static std::vector<uint8_t> buildAccuracyExceptionsRecord(
    int8_t min_exp,
    uint64_t exception_mask,
    const std::vector<float>& exception_values,
    const std::vector<uint8_t>& zfp_payload
) {
    std::vector<uint8_t> record;
    record.reserve(1 + 1 + sizeof(uint64_t) + zfp_payload.size() + exception_values.size() * sizeof(float));
    record.push_back(static_cast<uint8_t>(min_exp));
    if (exception_values.size() > std::numeric_limits<uint8_t>::max()) {
        throw std::runtime_error("exception count exceeds uint8");
    }
    record.push_back(static_cast<uint8_t>(exception_values.size()));
    const size_t mask_offset = record.size();
    record.resize(mask_offset + sizeof(uint64_t));
    std::memcpy(record.data() + mask_offset, &exception_mask, sizeof(uint64_t));
    record.insert(record.end(), zfp_payload.begin(), zfp_payload.end());
    for (float v : exception_values) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
        record.insert(record.end(), p, p + sizeof(float));
    }
    return record;
}

static std::vector<uint8_t> buildPrecisionRecord(
    uint8_t precision,
    const std::vector<uint8_t>& zfp_payload
) {
    std::vector<uint8_t> record;
    record.reserve(1 + zfp_payload.size());
    record.push_back(precision);
    record.insert(record.end(), zfp_payload.begin(), zfp_payload.end());
    return record;
}

} // namespace

struct RzfpCodec::Impl {
#ifdef ERWT3D_HAVE_RZFP
    zfp_stream* stream = nullptr;
    zfp_field* input_field = nullptr;
    zfp_field* output_field = nullptr;
    bitstream* bits = nullptr;

    alignas(64) float work[64]{};
    alignas(64) float decoded[64]{};

    std::vector<uint8_t> buffer;

    Impl() {
        buffer.resize(4096);
        input_field = zfp_field_3d(work, zfp_type_float, 4, 4, 4);
        output_field = zfp_field_3d(decoded, zfp_type_float, 4, 4, 4);
        stream = zfp_stream_open(nullptr);
        bits = stream_open(buffer.data(), buffer.size());
        zfp_stream_set_bit_stream(stream, bits);
        zfp_stream_set_execution(stream, zfp_exec_serial);
    }

    ~Impl() {
        if (input_field) zfp_field_free(input_field);
        if (output_field) zfp_field_free(output_field);
        if (stream) zfp_stream_close(stream);
        if (bits) stream_close(bits);
    }

    void ensureBuffer(size_t need) {
        if (buffer.size() >= need) return;
        buffer.resize(need);
        if (bits) stream_close(bits);
        bits = stream_open(buffer.data(), buffer.size());
        zfp_stream_set_bit_stream(stream, bits);
    }

    void rewind() {
        zfp_stream_rewind(stream);
        stream_rewind(bits);
    }
#endif
};

RzfpCodec::RzfpCodec() : impl_(new Impl()) {}
RzfpCodec::~RzfpCodec() { delete impl_; }

RzfpCandidate RzfpCodec::encodeBest(
    const float input[64],
    uint64_t valid_mask,
    const RzfpCodecConfig& config
) {
#ifdef ERWT3D_HAVE_RZFP
    RzfpCandidate raw_candidate;
    raw_candidate.codec = RzfpLeafCodec::RawFloat32;
    raw_candidate.payload.assign(
        reinterpret_cast<const uint8_t*>(input),
        reinterpret_cast<const uint8_t*>(input) + 256
    );
    raw_candidate.serialized_size = 2 + 256;
    raw_candidate.passed = true;
    raw_candidate.error_stats = checkBlockError(input, input, 64, valid_mask, config.error);

    std::vector<RzfpCandidate> candidates;
    candidates.push_back(raw_candidate);

    const uint64_t mandatory_mask = buildMandatoryExceptionMask(input, valid_mask);

    if (config.try_constant) {
        if (allZero(input, valid_mask)) {
            RzfpCandidate c;
            c.codec = RzfpLeafCodec::ConstantZero;
            c.serialized_size = 2;
            c.passed = true;
            float decoded[64] = {};
            c.error_stats = checkBlockError(input, decoded, 64, valid_mask, config.error);
            candidates.push_back(std::move(c));
        } else {
            float constant_value = 0.0f;
            if (allEqual(input, valid_mask, constant_value) &&
                allFiniteNormalNonZero(input, valid_mask)) {
                RzfpCandidate c;
                c.codec = RzfpLeafCodec::ConstantValue;
                const uint8_t* p = reinterpret_cast<const uint8_t*>(&constant_value);
                c.payload.assign(p, p + sizeof(float));
                c.serialized_size = 2 + sizeof(float);
                c.passed = true;
                float decoded[64];
                std::fill(decoded, decoded + 64, constant_value);
                c.error_stats = checkBlockError(input, decoded, 64, valid_mask, config.error);
                candidates.push_back(std::move(c));
            }
        }
    }

    bool has_compressible = false;
    for (uint32_t i = 0; i < 64; ++i) {
        const uint64_t bit = uint64_t{1} << i;
        if ((valid_mask & bit) == 0 || (mandatory_mask & bit) != 0) continue;
        has_compressible = true;
        break;
    }

    if (has_compressible) {
        uint32_t best_accuracy_size = std::numeric_limits<uint32_t>::max();

        if (config.try_accuracy || config.try_accuracy_exceptions) {
            const double min_abs = minAbsNonException(input, valid_mask, mandatory_mask);
            if (std::isfinite(min_abs) && min_abs > 0.0) {
                const double target_tolerance = config.error.internal_bound * min_abs;
                const int16_t requested_min_exp = safeToleranceExponent(target_tolerance);
                const double requested_tolerance = std::ldexp(1.0, requested_min_exp);

                // Precompute magnitude-sorted value indices once for optional exceptions.
                std::vector<std::pair<double, uint32_t>> sorted_vals;
                if (config.try_accuracy_exceptions) {
                    sorted_vals.reserve(64);
                    for (uint32_t i = 0; i < 64; ++i) {
                        const uint64_t bit = uint64_t{1} << i;
                        if ((valid_mask & bit) == 0 || (mandatory_mask & bit) != 0) continue;
                        sorted_vals.emplace_back(std::abs(static_cast<double>(input[i])), i);
                    }
                    std::sort(sorted_vals.begin(), sorted_vals.end());
                }

                bool last_count_improved = true;

                for (uint8_t opt_count : config.optional_exception_counts) {
                    if (opt_count == 0 && !config.try_accuracy) continue;
                    if (opt_count > 0 && !config.try_accuracy_exceptions) continue;

                    uint64_t optional_mask = 0;
                    if (opt_count > 0) {
                        const uint32_t take = static_cast<uint32_t>(
                            std::min<size_t>(opt_count, sorted_vals.size()));
                        for (uint32_t k = 0; k < take; ++k) {
                            optional_mask |= uint64_t{1} << sorted_vals[k].second;
                        }
                    }

                    uint64_t exception_mask = mandatory_mask | optional_mask;
                    const uint32_t exc_count = static_cast<uint32_t>(__builtin_popcountll(exception_mask));
                    if (exc_count > config.max_total_exceptions) continue;

                    for (RzfpExceptionFill fill : config.fill_modes) {
                        RzfpCandidate c;
                        c.codec = (exception_mask == 0)
                                      ? RzfpLeafCodec::ZfpAccuracy
                                      : RzfpLeafCodec::ZfpAccuracyExceptions;
                        c.fill_mode = fill;
                        c.exception_mask = exception_mask;

                        const float fill_value = computeFillValue(
                            input, valid_mask, exception_mask, fill
                        );
                        for (uint32_t i = 0; i < 64; ++i) {
                            const uint64_t bit = uint64_t{1} << i;
                            if ((valid_mask & bit) == 0) {
                                impl_->work[i] = 0.0f;
                            } else if ((exception_mask & bit) != 0) {
                                impl_->work[i] = fill_value;
                            } else {
                                impl_->work[i] = input[i];
                            }
                        }

                        zfp_stream_set_accuracy(impl_->stream, requested_tolerance);
                        int actual_minexp = 0;
                        zfp_stream_params(impl_->stream, nullptr, nullptr, nullptr, &actual_minexp);
                        const int16_t record_min_exp = static_cast<int16_t>(actual_minexp);

                        const size_t max_size = zfp_stream_maximum_size(impl_->stream, impl_->input_field);
                        impl_->ensureBuffer(max_size);
                        impl_->rewind();

                        const size_t compressed_size = zfp_compress(impl_->stream, impl_->input_field);
                        if (compressed_size == 0) continue;
                        stream_flush(impl_->bits);
                        const size_t flushed_size = zfp_stream_compressed_size(impl_->stream);

                        const size_t payload_zfp_bytes = std::max(compressed_size, flushed_size);

                        c.exception_values.clear();
                        for (uint32_t i = 0; i < 64; ++i) {
                            if (exception_mask & (uint64_t{1} << i)) {
                                c.exception_values.push_back(input[i]);
                            }
                        }

                        impl_->rewind();
                        if (!zfp_decompress(impl_->stream, impl_->output_field)) continue;

                        c.zfp_payload_size = static_cast<uint32_t>(compressed_size);
                        if (exception_mask == 0) {
                            c.payload = buildAccuracyRecord(
                                record_min_exp,
                                std::vector<uint8_t>(
                                    impl_->buffer.begin(),
                                    impl_->buffer.begin() + payload_zfp_bytes)
                            );
                        } else {
                            c.payload = buildAccuracyExceptionsRecord(
                                record_min_exp, exception_mask, c.exception_values,
                                std::vector<uint8_t>(
                                    impl_->buffer.begin(),
                                    impl_->buffer.begin() + payload_zfp_bytes)
                            );
                        }
                        c.parameter = record_min_exp;
                        c.exception_count = exc_count;
                        c.serialized_size = 2 + static_cast<uint32_t>(c.payload.size());

                        if (!patchExceptions(impl_->decoded, exception_mask,
                                             c.exception_values.data(),
                                             static_cast<uint8_t>(c.exception_values.size()))) continue;
                        c.error_stats = checkBlockError(
                            input, impl_->decoded, 64, valid_mask, config.error
                        );
                        if (!c.error_stats.passed) continue;
                        c.passed = true;
                        candidates.push_back(std::move(c));

                        if (c.serialized_size < best_accuracy_size) {
                            best_accuracy_size = c.serialized_size;
                            last_count_improved = true;
                        } else {
                            last_count_improved = false;
                        }
                    }

                    // If adding more exceptions did not help for two consecutive counts,
                    // further exceptions are unlikely to help.
                    if (opt_count >= 4 && !last_count_improved) break;
                }
            }
        }

        if (config.try_precision && mandatory_mask == 0) {
            uint32_t best_precision_size = std::numeric_limits<uint32_t>::max();
            for (uint8_t prec : config.precisions) {
                RzfpCandidate c;
                c.codec = RzfpLeafCodec::ZfpPrecision;

                for (uint32_t i = 0; i < 64; ++i) {
                    impl_->work[i] = ((valid_mask & (uint64_t{1} << i)) != 0) ? input[i] : 0.0f;
                }

                const uint8_t actual_prec = static_cast<uint8_t>(zfp_stream_set_precision(
                    impl_->stream, prec
                ));
                const size_t max_size = zfp_stream_maximum_size(impl_->stream, impl_->input_field);
                impl_->ensureBuffer(max_size);
                impl_->rewind();

                const size_t compressed_size = zfp_compress(impl_->stream, impl_->input_field);
                if (compressed_size == 0) continue;
                stream_flush(impl_->bits);
                const size_t flushed_size = zfp_stream_compressed_size(impl_->stream);

                std::vector<uint8_t> zfp_payload(
                    impl_->buffer.begin(),
                    impl_->buffer.begin() + std::max(compressed_size, flushed_size)
                );

                impl_->rewind();
                if (!zfp_decompress(impl_->stream, impl_->output_field)) continue;

                c.error_stats = checkBlockError(input, impl_->decoded, 64, valid_mask, config.error);
                if (!c.error_stats.passed) continue;

                c.zfp_payload_size = static_cast<uint32_t>(compressed_size);
                c.payload = buildPrecisionRecord(actual_prec, zfp_payload);
                c.parameter = static_cast<int16_t>(actual_prec);
                c.serialized_size = 2 + static_cast<uint32_t>(c.payload.size());
                c.passed = true;
                candidates.push_back(std::move(c));

                if (c.serialized_size < best_precision_size) {
                    best_precision_size = c.serialized_size;
                } else {
                    // Higher precision only increases size; stop.
                    break;
                }
            }
        }
    }

    const RzfpCandidate* best = &raw_candidate;
    for (const auto& c : candidates) {
        if (!c.passed) continue;
        if (c.serialized_size < best->serialized_size) {
            best = &c;
        }
    }

    const double gain = 1.0 - static_cast<double>(best->serialized_size) / 256.0;
    if (gain < config.minimum_size_gain) {
        return raw_candidate;
    }
    return *best;
#else
    (void)input;
    (void)valid_mask;
    (void)config;
    RzfpCandidate c;
    c.passed = false;
    return c;
#endif
}

bool RzfpCodec::decode(
    const RzfpCandidate& encoded,
    float output[64]
) {
    return decodeRecord(encoded.codec, encoded.payload.data(), encoded.payload.size(), output);
}

bool RzfpCodec::decodeRecord(
    RzfpLeafCodec codec,
    const uint8_t* data,
    size_t size,
    float output[RZFP_LEAF_VALUES],
    RzfpCodecProfile* prof)
{
    using Clock = std::chrono::steady_clock;

#ifdef ERWT3D_HAVE_RZFP
    if (prof) {
        switch (codec) {
            case RzfpLeafCodec::RawFloat32:          ++prof->raw_count; break;
            case RzfpLeafCodec::ConstantZero:         ++prof->zero_count; break;
            case RzfpLeafCodec::ConstantValue:        ++prof->constant_count; break;
            case RzfpLeafCodec::ZfpAccuracy:          ++prof->accuracy_count; break;
            case RzfpLeafCodec::ZfpAccuracyExceptions: ++prof->accuracy_exception_count; break;
            case RzfpLeafCodec::ZfpPrecision:          ++prof->precision_count; break;
        }
        prof->decoded_value_count += 64;
        prof->compressed_payload_bytes += size;
    }
    switch (codec) {
        case RzfpLeafCodec::RawFloat32: {
            if (size != 256) return false;
            if (output) std::memcpy(output, data, 256);
            return true;
        }
        case RzfpLeafCodec::ConstantZero: {
            if (output) std::fill(output, output + 64, 0.0f);
            return true;
        }
        case RzfpLeafCodec::ConstantValue: {
            if (size != sizeof(float)) return false;
            if (output) {
                float v = 0.0f;
                std::memcpy(&v, data, sizeof(float));
                std::fill(output, output + 64, v);
            }
            return true;
        }
        case RzfpLeafCodec::ZfpAccuracy:
        case RzfpLeafCodec::ZfpAccuracyExceptions: {
            if (size < 1) return false;
            const int8_t min_exp = static_cast<int8_t>(data[0]);

            uint64_t exception_mask = 0;
            float exc_buf[64];
            uint8_t exc_count = 0;
            size_t zfp_offset = 1;
            size_t zfp_size = size - 1;
            if (codec == RzfpLeafCodec::ZfpAccuracyExceptions) {
                if (size < 2) return false;
                exc_count = data[1];
                const size_t header_size = 2 + sizeof(uint64_t);
                const size_t exc_bytes = exc_count * sizeof(float);
                if (size < header_size + exc_bytes) return false;
                if (exc_count > 64) return false;
                std::memcpy(&exception_mask, data + 2, sizeof(uint64_t));
                zfp_offset = header_size;
                zfp_size = size - header_size - exc_bytes;

                auto tExcAlloc = Clock::now();
                std::memcpy(exc_buf, data + zfp_offset + zfp_size, exc_bytes);
                if (prof) prof->exception_alloc_ns += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - tExcAlloc).count());
            }

            if (zfp_size == 0) return false;

            const double tolerance = std::ldexp(1.0, min_exp);
            zfp_stream_set_accuracy(impl_->stream, tolerance);

            impl_->ensureBuffer(zfp_size);
            auto tPayloadCopy = Clock::now();
            std::memcpy(impl_->buffer.data(), data + zfp_offset, zfp_size);
            if (prof) prof->payload_copy_ns += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - tPayloadCopy).count());
            impl_->rewind();

            auto tZfp = Clock::now();
            if (!zfp_decompress(impl_->stream, impl_->output_field)) {
                return false;
            }
            if (prof) prof->zfp_decompress_ns += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - tZfp).count());

            auto tExcPatch = Clock::now();
            if (!patchExceptions(impl_->decoded, exception_mask, exc_buf, exc_count)) {
                return false;
            }
            if (prof) prof->exception_patch_ns += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - tExcPatch).count());

            if (output) {
                auto tLeafCopy = Clock::now();
                std::memcpy(output, impl_->decoded, 256);
                if (prof) prof->leaf_copy_ns += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - tLeafCopy).count());
            }
            return true;
        }
        case RzfpLeafCodec::ZfpPrecision: {
            if (size < 1) return false;
            const uint8_t precision = data[0];
            const size_t zfp_size = size - 1;
            if (zfp_size == 0) return false;

            zfp_stream_set_precision(impl_->stream, precision);
            impl_->ensureBuffer(zfp_size);
            auto tPC = Clock::now();
            std::memcpy(impl_->buffer.data(), data + 1, zfp_size);
            if (prof) prof->payload_copy_ns += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - tPC).count());
            impl_->rewind();

            auto tZ = Clock::now();
            if (!zfp_decompress(impl_->stream, impl_->output_field)) {
                return false;
            }
            if (prof) prof->zfp_decompress_ns += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - tZ).count());

            if (output) {
                auto tLC = Clock::now();
                std::memcpy(output, impl_->decoded, 256);
                if (prof) prof->leaf_copy_ns += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - tLC).count());
            }
            return true;
        }
    }
    return false;
#else
    (void)codec;
    (void)data;
    (void)size;
    (void)output;
    return false;
#endif
}

const float* RzfpCodec::decodedData() const {
#ifdef ERWT3D_HAVE_RZFP
    return impl_->decoded;
#else
    return nullptr;
#endif
}

} // namespace erwt3d
