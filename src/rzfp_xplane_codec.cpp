#include "erwt3d/rzfp_xplane_codec.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#ifdef ERWT3D_HAVE_RZFP
#include <zfp.h>
#include <zfp/bitstream.h>
#endif

namespace erwt3d {

namespace {

constexpr uint16_t XPLANE_DESCRIPTOR_SIZE_MASK = 0x1FFFu;
constexpr uint16_t XPLANE_DESCRIPTOR_CODEC_SHIFT = 13u;

inline uint16_t makeXPlaneDescriptor(RzfpXPlaneCodec codec, uint16_t size) {
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(codec) << XPLANE_DESCRIPTOR_CODEC_SHIFT) |
        size);
}

inline RzfpXPlaneCodec descriptorCodec(uint16_t descriptor) {
    return static_cast<RzfpXPlaneCodec>(descriptor >> XPLANE_DESCRIPTOR_CODEC_SHIFT);
}

inline uint16_t descriptorSize(uint16_t descriptor) {
    return descriptor & XPLANE_DESCRIPTOR_SIZE_MASK;
}

static uint32_t buildMandatoryExceptionMask(const float input[16], uint32_t valid_mask) {
    uint32_t mask = 0;
    for (uint32_t i = 0; i < 16; ++i) {
        if ((valid_mask & (1u << i)) == 0) continue;
        const float v = input[i];
        if (v == 0.0f || !std::isfinite(v) || std::fpclassify(v) == FP_SUBNORMAL) {
            mask |= (1u << i);
        }
    }
    return mask;
}

static bool allZero(const float input[16], uint32_t valid_mask) {
    for (uint32_t i = 0; i < 16; ++i) {
        if ((valid_mask & (1u << i)) == 0) continue;
        if (input[i] != 0.0f) return false;
    }
    return true;
}

static bool allEqual(const float input[16], uint32_t valid_mask, float& value) {
    bool first = true;
    float ref = 0.0f;
    for (uint32_t i = 0; i < 16; ++i) {
        if ((valid_mask & (1u << i)) == 0) continue;
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

static float computeFillValue(const float input[16], uint32_t valid_mask, uint32_t exception_mask) {
    double sum = 0.0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < 16; ++i) {
        const uint32_t bit = 1u << i;
        if ((valid_mask & bit) == 0 || (exception_mask & bit) != 0) continue;
        if (!std::isfinite(input[i])) continue;
        sum += input[i];
        ++count;
    }
    return count == 0 ? 0.0f : static_cast<float>(sum / count);
}

static int16_t toleranceExponent(double tolerance) {
    const double e = std::floor(std::log2(tolerance));
    if (e < std::numeric_limits<int16_t>::min() || e > std::numeric_limits<int16_t>::max()) {
        return 0;
    }
    return static_cast<int16_t>(e);
}

static double minAbsNonException(const float input[16], uint32_t valid_mask, uint32_t exception_mask) {
    double min_abs = std::numeric_limits<double>::infinity();
    for (uint32_t i = 0; i < 16; ++i) {
        const uint32_t bit = 1u << i;
        if ((valid_mask & bit) == 0 || (exception_mask & bit) != 0) continue;
        min_abs = std::min(min_abs, std::abs(static_cast<double>(input[i])));
    }
    return min_abs;
}

static bool checkBlock(const float input[16], const float decoded[16], uint32_t valid_mask,
                       const RelativeErrorConfig& cfg, double& max_rel) {
    max_rel = 0.0;
    bool pass = true;
    for (uint32_t i = 0; i < 16; ++i) {
        if ((valid_mask & (1u << i)) == 0) continue;
        auto r = checkPointwiseError(input[i], decoded[i], cfg);
        max_rel = std::max(max_rel, r.relative_error);
        if (!r.passed) pass = false;
    }
    return pass;
}

static void patchExceptions(float decoded[16], uint32_t exception_mask, const float input[16]) {
    for (uint32_t i = 0; i < 16; ++i) {
        if (exception_mask & (1u << i)) {
            decoded[i] = input[i];
        }
    }
}

struct Candidate {
    RzfpXPlaneCodec codec = RzfpXPlaneCodec::RawFloat32;
    std::vector<uint8_t> payload;
    bool passed = false;
};

static Candidate makeRaw(const float input[16]) {
    Candidate c;
    c.codec = RzfpXPlaneCodec::RawFloat32;
    c.payload.assign(reinterpret_cast<const uint8_t*>(input),
                     reinterpret_cast<const uint8_t*>(input) + 16 * sizeof(float));
    c.passed = true;
    return c;
}

static Candidate makeConstantZero() {
    Candidate c;
    c.codec = RzfpXPlaneCodec::ConstantZero;
    c.passed = true;
    return c;
}

static Candidate makeConstantValue(float value) {
    Candidate c;
    c.codec = RzfpXPlaneCodec::ConstantValue;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&value);
    c.payload.assign(p, p + sizeof(float));
    c.passed = true;
    return c;
}

#ifdef ERWT3D_HAVE_RZFP

static Candidate tryAccuracy(const float input[16], uint32_t valid_mask,
                             uint32_t exception_mask, const RelativeErrorConfig& cfg,
                             bool with_exceptions, bool verify = true) {
    Candidate result;
    result.codec = with_exceptions ? RzfpXPlaneCodec::ZfpAccuracyExceptions
                                   : RzfpXPlaneCodec::ZfpAccuracy;

    const double min_abs = minAbsNonException(input, valid_mask, exception_mask);
    if (!std::isfinite(min_abs) || min_abs <= 0.0) return result;

    const double target_tolerance = cfg.internal_bound * min_abs;
    const int16_t min_exp = toleranceExponent(target_tolerance);
    const double tolerance = std::ldexp(1.0, min_exp);

    float block[16];
    std::memcpy(block, input, sizeof(block));
    if (with_exceptions) {
        const float fill = computeFillValue(input, valid_mask, exception_mask);
        for (uint32_t i = 0; i < 16; ++i) {
            if (exception_mask & (1u << i)) block[i] = fill;
        }
    }

    zfp_stream* stream = zfp_stream_open(nullptr);
    zfp_stream_set_accuracy(stream, tolerance);
    zfp_field* field = zfp_field_2d(block, zfp_type_float, 4, 4);
    size_t max_size = zfp_stream_maximum_size(stream, field);
    std::vector<uint8_t> buffer(max_size);
    bitstream* bits = stream_open(buffer.data(), buffer.size());
    zfp_stream_set_bit_stream(stream, bits);

    size_t zfp_size = zfp_compress(stream, field);
    if (zfp_size == 0) {
        zfp_field_free(field);
        stream_close(bits);
        zfp_stream_close(stream);
        return result;
    }

    if (!verify) {
        if (with_exceptions) {
            std::vector<float> exc_values;
            for (uint32_t i = 0; i < 16; ++i) {
                if (exception_mask & (1u << i)) exc_values.push_back(input[i]);
            }
            if (exc_values.size() > 255) {
                zfp_field_free(field);
                stream_close(bits);
                zfp_stream_close(stream);
                return result;
            }
            result.payload.reserve(1 + 1 + sizeof(uint16_t) + zfp_size + exc_values.size() * sizeof(float));
            result.payload.push_back(static_cast<uint8_t>(min_exp));
            result.payload.push_back(static_cast<uint8_t>(exc_values.size()));
            const uint16_t mask16 = static_cast<uint16_t>(exception_mask);
            const uint8_t* mask_p = reinterpret_cast<const uint8_t*>(&mask16);
            result.payload.insert(result.payload.end(), mask_p, mask_p + sizeof(uint16_t));
            result.payload.insert(result.payload.end(), buffer.data(), buffer.data() + zfp_size);
            for (float v : exc_values) {
                const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
                result.payload.insert(result.payload.end(), p, p + sizeof(float));
            }
        } else {
            result.payload.reserve(1 + zfp_size);
            result.payload.push_back(static_cast<uint8_t>(min_exp));
            result.payload.insert(result.payload.end(), buffer.data(), buffer.data() + zfp_size);
        }
        result.passed = true;
        zfp_field_free(field);
        stream_close(bits);
        zfp_stream_close(stream);
        return result;
    }

    float decoded[16];
    zfp_field* out_field = zfp_field_2d(decoded, zfp_type_float, 4, 4);
    zfp_stream_rewind(stream);
    stream_rewind(bits);
    if (!zfp_decompress(stream, out_field)) {
        zfp_field_free(field);
        zfp_field_free(out_field);
        stream_close(bits);
        zfp_stream_close(stream);
        return result;
    }

    if (with_exceptions) patchExceptions(decoded, exception_mask, input);

    double max_rel = 0.0;
    bool passed = checkBlock(input, decoded, valid_mask, cfg, max_rel);

    zfp_field_free(field);
    zfp_field_free(out_field);
    stream_close(bits);
    zfp_stream_close(stream);

    if (!passed) return result;

    if (with_exceptions) {
        std::vector<float> exc_values;
        for (uint32_t i = 0; i < 16; ++i) {
            if (exception_mask & (1u << i)) exc_values.push_back(input[i]);
        }
        if (exc_values.size() > 255) return result;
        result.payload.reserve(1 + 1 + sizeof(uint16_t) + zfp_size + exc_values.size() * sizeof(float));
        result.payload.push_back(static_cast<uint8_t>(min_exp));
        result.payload.push_back(static_cast<uint8_t>(exc_values.size()));
        const uint16_t mask16 = static_cast<uint16_t>(exception_mask);
        const uint8_t* mask_p = reinterpret_cast<const uint8_t*>(&mask16);
        result.payload.insert(result.payload.end(), mask_p, mask_p + sizeof(uint16_t));
        result.payload.insert(result.payload.end(), buffer.data(), buffer.data() + zfp_size);
        for (float v : exc_values) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
            result.payload.insert(result.payload.end(), p, p + sizeof(float));
        }
    } else {
        result.payload.reserve(1 + zfp_size);
        result.payload.push_back(static_cast<uint8_t>(min_exp));
        result.payload.insert(result.payload.end(), buffer.data(), buffer.data() + zfp_size);
    }
    result.passed = true;
    return result;
}

static Candidate tryPrecision(const float input[16], uint32_t valid_mask,
                              uint32_t exception_mask, const RelativeErrorConfig& cfg,
                              uint8_t precision, bool with_exceptions) {
    Candidate result;
    result.codec = with_exceptions ? RzfpXPlaneCodec::ZfpPrecisionExceptions
                                   : RzfpXPlaneCodec::ZfpPrecision;

    float block[16];
    std::memcpy(block, input, sizeof(block));
    if (with_exceptions) {
        const float fill = computeFillValue(input, valid_mask, exception_mask);
        for (uint32_t i = 0; i < 16; ++i) {
            if (exception_mask & (1u << i)) block[i] = fill;
        }
    }

    zfp_stream* stream = zfp_stream_open(nullptr);
    zfp_stream_set_precision(stream, precision);
    zfp_field* field = zfp_field_2d(block, zfp_type_float, 4, 4);
    size_t max_size = zfp_stream_maximum_size(stream, field);
    std::vector<uint8_t> buffer(max_size);
    bitstream* bits = stream_open(buffer.data(), buffer.size());
    zfp_stream_set_bit_stream(stream, bits);

    size_t zfp_size = zfp_compress(stream, field);
    if (zfp_size == 0) {
        zfp_field_free(field);
        stream_close(bits);
        zfp_stream_close(stream);
        return result;
    }

    float decoded[16];
    zfp_field* out_field = zfp_field_2d(decoded, zfp_type_float, 4, 4);
    zfp_stream_rewind(stream);
    stream_rewind(bits);
    if (!zfp_decompress(stream, out_field)) {
        zfp_field_free(field);
        zfp_field_free(out_field);
        stream_close(bits);
        zfp_stream_close(stream);
        return result;
    }

    if (with_exceptions) patchExceptions(decoded, exception_mask, input);

    double max_rel = 0.0;
    bool passed = checkBlock(input, decoded, valid_mask, cfg, max_rel);

    zfp_field_free(field);
    zfp_field_free(out_field);
    stream_close(bits);
    zfp_stream_close(stream);

    if (!passed) return result;

    if (with_exceptions) {
        std::vector<float> exc_values;
        for (uint32_t i = 0; i < 16; ++i) {
            if (exception_mask & (1u << i)) exc_values.push_back(input[i]);
        }
        if (exc_values.size() > 255) return result;
        result.payload.reserve(1 + 1 + sizeof(uint16_t) + zfp_size + exc_values.size() * sizeof(float));
        result.payload.push_back(precision);
        result.payload.push_back(static_cast<uint8_t>(exc_values.size()));
        const uint16_t mask16 = static_cast<uint16_t>(exception_mask);
        const uint8_t* mask_p = reinterpret_cast<const uint8_t*>(&mask16);
        result.payload.insert(result.payload.end(), mask_p, mask_p + sizeof(uint16_t));
        result.payload.insert(result.payload.end(), buffer.data(), buffer.data() + zfp_size);
        for (float v : exc_values) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
            result.payload.insert(result.payload.end(), p, p + sizeof(float));
        }
    } else {
        result.payload.reserve(1 + zfp_size);
        result.payload.push_back(precision);
        result.payload.insert(result.payload.end(), buffer.data(), buffer.data() + zfp_size);
    }
    result.passed = true;
    return result;
}

#endif

static Candidate encodeBestBlock(const float input[16], uint32_t valid_mask,
                                 const RzfpXPlaneCodecConfig& cfg) {
    Candidate best = makeRaw(input);
    uint32_t min_size = static_cast<uint32_t>(2 + best.payload.size());

    if (allZero(input, valid_mask)) {
        auto c = makeConstantZero();
        uint32_t size = 2;
        if (size < min_size) { best = std::move(c); min_size = size; }
    } else {
        float const_value = 0.0f;
        if (allEqual(input, valid_mask, const_value) &&
            const_value != 0.0f && std::isfinite(const_value) &&
            std::fpclassify(const_value) != FP_SUBNORMAL) {
            auto c = makeConstantValue(const_value);
            uint32_t size = 2 + static_cast<uint32_t>(c.payload.size());
            if (size < min_size) { best = std::move(c); min_size = size; }
        }
    }

#ifdef ERWT3D_HAVE_RZFP
    const uint32_t exception_mask = buildMandatoryExceptionMask(input, valid_mask);

    if (cfg.fast_accuracy_only) {
        if (exception_mask == 0) {
            auto acc = tryAccuracy(input, valid_mask, exception_mask, cfg.error, false, true);
            if (acc.passed) {
                uint32_t size = 2 + static_cast<uint32_t>(acc.payload.size());
                if (size < min_size) { best = std::move(acc); min_size = size; }
            }
        }

        auto acc_exc = tryAccuracy(input, valid_mask, exception_mask, cfg.error, true, true);
        if (acc_exc.passed) {
            uint32_t size = 2 + static_cast<uint32_t>(acc_exc.payload.size());
            if (size < min_size) { best = std::move(acc_exc); min_size = size; }
        }
    } else {
        auto acc = tryAccuracy(input, valid_mask, exception_mask, cfg.error, false);
        if (acc.passed) {
            uint32_t size = 2 + static_cast<uint32_t>(acc.payload.size());
            if (size < min_size) { best = std::move(acc); min_size = size; }
        }

        auto acc_exc = tryAccuracy(input, valid_mask, exception_mask, cfg.error, true);
        if (acc_exc.passed) {
            uint32_t size = 2 + static_cast<uint32_t>(acc_exc.payload.size());
            if (size < min_size) { best = std::move(acc_exc); min_size = size; }
        }

        for (uint8_t prec : cfg.precisions) {
            auto p = tryPrecision(input, valid_mask, exception_mask, cfg.error, prec, false);
            if (p.passed) {
                uint32_t size = 2 + static_cast<uint32_t>(p.payload.size());
                if (size < min_size) { best = std::move(p); min_size = size; }
            }
            if (cfg.try_precision_exceptions) {
                auto p_exc = tryPrecision(input, valid_mask, exception_mask, cfg.error, prec, true);
                if (p_exc.passed) {
                    uint32_t size = 2 + static_cast<uint32_t>(p_exc.payload.size());
                    if (size < min_size) { best = std::move(p_exc); min_size = size; }
                }
            }
        }
    }
#endif

    return best;
}

#ifdef ERWT3D_HAVE_RZFP

static bool decodeZfpBlock(const uint8_t* payload, size_t payload_size, float decoded[16],
                           bool is_accuracy, bool has_exceptions) {
    const uint8_t parameter = payload[0];
    size_t zfp_offset = 1;
    size_t zfp_size = payload_size - 1;
    uint16_t exc_mask16 = 0;
    const float* exc_vals = nullptr;

    if (has_exceptions) {
        const uint8_t exc_count = payload[1];
        exc_mask16 = *reinterpret_cast<const uint16_t*>(payload + 2);
        const size_t header_size = 2 + sizeof(uint16_t);
        zfp_offset = header_size;
        zfp_size = payload_size - header_size - exc_count * sizeof(float);
        exc_vals = reinterpret_cast<const float*>(payload + header_size + zfp_size);
    }

    zfp_stream* stream = zfp_stream_open(nullptr);
    if (is_accuracy) {
        const double tolerance = std::ldexp(1.0, static_cast<int8_t>(parameter));
        zfp_stream_set_accuracy(stream, tolerance);
    } else {
        zfp_stream_set_precision(stream, parameter);
    }

    std::vector<uint8_t> buf(payload, payload + payload_size);
    bitstream* bits = stream_open(buf.data() + zfp_offset, zfp_size);
    zfp_stream_set_bit_stream(stream, bits);
    float tmp[16];
    zfp_field* out_field = zfp_field_2d(tmp, zfp_type_float, 4, 4);
    bool ok = zfp_decompress(stream, out_field);
    if (ok) {
        std::memcpy(decoded, tmp, sizeof(tmp));
        if (has_exceptions) {
            uint32_t em = exc_mask16;
            size_t pos = 0;
            for (uint32_t i = 0; i < 16; ++i) {
                if (em & (1u << i)) decoded[i] = exc_vals[pos++];
            }
        }
    }
    zfp_field_free(out_field);
    stream_close(bits);
    zfp_stream_close(stream);
    return ok;
}

#endif

} // namespace

std::vector<uint8_t> encodeXPlane2D(
    const float* plane,
    uint64_t ny,
    uint64_t nz,
    const RzfpXPlaneCodecConfig& config
) {
    std::vector<uint8_t> record;
    const uint64_t blocks_y = (ny + 3) / 4;
    const uint64_t blocks_z = (nz + 3) / 4;
    float block[16];

    for (uint64_t bz = 0; bz < blocks_z; ++bz) {
        for (uint64_t by = 0; by < blocks_y; ++by) {
            uint32_t valid_mask = 0;
            for (uint32_t lz = 0; lz < 4; ++lz) {
                for (uint32_t ly = 0; ly < 4; ++ly) {
                    const uint32_t i = lz * 4 + ly;
                    const uint64_t gy = by * 4 + ly;
                    const uint64_t gz = bz * 4 + lz;
                    if (gy < ny && gz < nz) {
                        valid_mask |= (1u << i);
                        block[i] = plane[gz * ny + gy];
                    } else {
                        block[i] = 0.0f;
                    }
                }
            }

            auto c = encodeBestBlock(block, valid_mask, config);
            const uint16_t descriptor = makeXPlaneDescriptor(c.codec, static_cast<uint16_t>(c.payload.size()));
            const uint8_t* desc_p = reinterpret_cast<const uint8_t*>(&descriptor);
            record.insert(record.end(), desc_p, desc_p + sizeof(descriptor));
            record.insert(record.end(), c.payload.begin(), c.payload.end());
        }
    }
    return record;
}

bool decodeXPlane2D(
    const uint8_t* record,
    size_t record_size,
    float* plane,
    uint64_t ny,
    uint64_t nz
) {
    const uint64_t blocks_y = (ny + 3) / 4;
    const uint64_t blocks_z = (nz + 3) / 4;
    const uint64_t total_blocks = blocks_y * blocks_z;

    size_t offset = 0;
    for (uint64_t b = 0; b < total_blocks; ++b) {
        if (offset + sizeof(uint16_t) > record_size) return false;
        uint16_t descriptor;
        std::memcpy(&descriptor, record + offset, sizeof(descriptor));
        offset += sizeof(descriptor);
        const RzfpXPlaneCodec codec = descriptorCodec(descriptor);
        const uint16_t payload_size = descriptorSize(descriptor);
        if (offset + payload_size > record_size) return false;

        float decoded[16];
        std::fill(decoded, decoded + 16, 0.0f);

#ifdef ERWT3D_HAVE_RZFP
        switch (codec) {
            case RzfpXPlaneCodec::RawFloat32: {
                if (payload_size != 16 * sizeof(float)) return false;
                std::memcpy(decoded, record + offset, payload_size);
                break;
            }
            case RzfpXPlaneCodec::ConstantZero: {
                if (payload_size != 0) return false;
                std::fill(decoded, decoded + 16, 0.0f);
                break;
            }
            case RzfpXPlaneCodec::ConstantValue: {
                if (payload_size != sizeof(float)) return false;
                float v = 0.0f;
                std::memcpy(&v, record + offset, sizeof(float));
                std::fill(decoded, decoded + 16, v);
                break;
            }
            case RzfpXPlaneCodec::ZfpAccuracy:
                if (!decodeZfpBlock(record + offset, payload_size, decoded, true, false)) return false;
                break;
            case RzfpXPlaneCodec::ZfpAccuracyExceptions:
                if (!decodeZfpBlock(record + offset, payload_size, decoded, true, true)) return false;
                break;
            case RzfpXPlaneCodec::ZfpPrecision:
                if (!decodeZfpBlock(record + offset, payload_size, decoded, false, false)) return false;
                break;
            case RzfpXPlaneCodec::ZfpPrecisionExceptions:
                if (!decodeZfpBlock(record + offset, payload_size, decoded, false, true)) return false;
                break;
            default:
                return false;
        }
#else
        std::memcpy(decoded, record + offset, sizeof(decoded));
#endif
        offset += payload_size;

        const uint64_t bz = b / blocks_y;
        const uint64_t by = b % blocks_y;
        for (uint32_t lz = 0; lz < 4; ++lz) {
            for (uint32_t ly = 0; ly < 4; ++ly) {
                const uint64_t gy = by * 4 + ly;
                const uint64_t gz = bz * 4 + lz;
                if (gy < ny && gz < nz) {
                    plane[gz * ny + gy] = decoded[lz * 4 + ly];
                }
            }
        }
    }
    return offset == record_size;
}

} // namespace erwt3d
