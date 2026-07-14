#include "erwt3d/relative_error.hpp"
#include "erwt3d/rzfp_codec.hpp"
#include "erwt3d/rzfp_xplane_codec.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

#ifdef ERWT3D_HAVE_RZFP
#include <zfp.h>
#include <zfp/bitstream.h>
#endif

namespace {

using namespace erwt3d;

enum class Codec2D : uint8_t {
    RawFloat32 = 0,
    ConstantZero = 1,
    ConstantValue = 2,
    ZfpAccuracy = 3,
    ZfpAccuracyExceptions = 4,
    ZfpPrecision = 5,
    ZfpPrecisionExceptions = 6,
};

struct ProbeOptions {
    std::string raw_path;
    uint64_t nx = 0;
    uint64_t ny = 0;
    uint64_t nz = 0;
    int samples = 10;
    double rel_bound = 1e-3;
    uint32_t seed = 20260511;
    bool fast = false;
    std::vector<uint8_t> custom_precisions;
};

struct ProbeStats {
    uint64_t total_raw_bytes = 0;
    uint64_t total_compressed_bytes = 0;
    uint64_t total_blocks = 0;
    uint64_t raw_blocks = 0;
    uint64_t zero_blocks = 0;
    uint64_t constant_blocks = 0;
    uint64_t accuracy_blocks = 0;
    uint64_t accuracy_exc_blocks = 0;
    uint64_t precision_blocks = 0;
    uint64_t precision_exc_blocks = 0;
    double max_relative_error = 0.0;
    uint64_t violation_count = 0;
};

static bool readRawPlane(int fd, uint64_t x, uint64_t nx, uint64_t ny, uint64_t nz, float* plane) {
    const uint64_t row_bytes = ny * sizeof(float);
    for (uint64_t z = 0; z < nz; ++z) {
        const uint64_t off = (z * ny) * nx + x;
        if (pread(fd, plane + z * ny, row_bytes, off * sizeof(float)) != static_cast<ssize_t>(row_bytes)) {
            return false;
        }
    }
    return true;
}

static uint32_t buildMandatoryExceptionMask2D(const float input[16], uint32_t valid_mask) {
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

static bool allZero2D(const float input[16], uint32_t valid_mask) {
    for (uint32_t i = 0; i < 16; ++i) {
        if ((valid_mask & (1u << i)) == 0) continue;
        if (input[i] != 0.0f) return false;
    }
    return true;
}

static bool allEqual2D(const float input[16], uint32_t valid_mask, float& value) {
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

static float computeFillValue2D(const float input[16], uint32_t valid_mask, uint32_t exception_mask) {
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

static double minAbsNonException2D(const float input[16], uint32_t valid_mask, uint32_t exception_mask) {
    double min_abs = std::numeric_limits<double>::infinity();
    for (uint32_t i = 0; i < 16; ++i) {
        const uint32_t bit = 1u << i;
        if ((valid_mask & bit) == 0 || (exception_mask & bit) != 0) continue;
        min_abs = std::min(min_abs, std::abs(static_cast<double>(input[i])));
    }
    return min_abs;
}

static bool checkBlock2D(const float input[16], const float decoded[16], uint32_t valid_mask,
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

static void patchExceptions2D(float decoded[16], uint32_t exception_mask, const float input[16]) {
    for (uint32_t i = 0; i < 16; ++i) {
        if (exception_mask & (1u << i)) {
            decoded[i] = input[i];
        }
    }
}

struct Candidate2D {
    Codec2D codec = Codec2D::RawFloat32;
    std::vector<uint8_t> payload;
    bool passed = false;
};

static Candidate2D makeRawCandidate2D(const float input[16]) {
    Candidate2D c;
    c.codec = Codec2D::RawFloat32;
    c.payload.assign(reinterpret_cast<const uint8_t*>(input),
                     reinterpret_cast<const uint8_t*>(input) + 16 * sizeof(float));
    c.passed = true;
    return c;
}

static Candidate2D makeConstantZeroCandidate2D() {
    Candidate2D c;
    c.codec = Codec2D::ConstantZero;
    c.passed = true;
    return c;
}

static Candidate2D makeConstantValueCandidate2D(float value) {
    Candidate2D c;
    c.codec = Codec2D::ConstantValue;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&value);
    c.payload.assign(p, p + sizeof(float));
    c.passed = true;
    return c;
}

#ifdef ERWT3D_HAVE_RZFP

static Candidate2D tryAccuracy2D(const float input[16], uint32_t valid_mask,
                                 uint32_t exception_mask, const RelativeErrorConfig& cfg,
                                 bool with_exceptions, bool verify = true) {
    Candidate2D result;
    result.codec = with_exceptions ? Codec2D::ZfpAccuracyExceptions : Codec2D::ZfpAccuracy;

    const double min_abs = minAbsNonException2D(input, valid_mask, exception_mask);
    if (!std::isfinite(min_abs) || min_abs <= 0.0) return result;

    const double target_tolerance = cfg.internal_bound * min_abs;
    const int16_t min_exp = toleranceExponent(target_tolerance);
    const double tolerance = std::ldexp(1.0, min_exp);

    float block[16];
    std::memcpy(block, input, sizeof(block));
    if (with_exceptions) {
        const float fill = computeFillValue2D(input, valid_mask, exception_mask);
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

    if (with_exceptions) {
        patchExceptions2D(decoded, exception_mask, input);
    }

    double max_rel = 0.0;
    bool passed = checkBlock2D(input, decoded, valid_mask, cfg, max_rel);

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

static Candidate2D tryPrecision2D(const float input[16], uint32_t valid_mask,
                                  uint32_t exception_mask, const RelativeErrorConfig& cfg,
                                  uint8_t precision, bool with_exceptions) {
    Candidate2D result;
    result.codec = with_exceptions ? Codec2D::ZfpPrecisionExceptions : Codec2D::ZfpPrecision;

    float block[16];
    std::memcpy(block, input, sizeof(block));
    if (with_exceptions) {
        const float fill = computeFillValue2D(input, valid_mask, exception_mask);
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

    if (with_exceptions) {
        patchExceptions2D(decoded, exception_mask, input);
    }

    double max_rel = 0.0;
    bool passed = checkBlock2D(input, decoded, valid_mask, cfg, max_rel);

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

static Candidate2D encodeBest2D(const float input[16], uint32_t valid_mask,
                                const RzfpXPlaneCodecConfig& cfg) {
    Candidate2D best = makeRawCandidate2D(input);
    uint32_t min_size = static_cast<uint32_t>(2 + best.payload.size());

    if (allZero2D(input, valid_mask)) {
        auto c = makeConstantZeroCandidate2D();
        uint32_t size = 2;
        if (size < min_size) { best = std::move(c); min_size = size; }
    } else {
        float const_value = 0.0f;
        if (allEqual2D(input, valid_mask, const_value) &&
            const_value != 0.0f && std::isfinite(const_value) &&
            std::fpclassify(const_value) != FP_SUBNORMAL) {
            auto c = makeConstantValueCandidate2D(const_value);
            uint32_t size = 2 + static_cast<uint32_t>(c.payload.size());
            if (size < min_size) { best = std::move(c); min_size = size; }
        }
    }

#ifdef ERWT3D_HAVE_RZFP
    uint32_t exception_mask = buildMandatoryExceptionMask2D(input, valid_mask);

    if (cfg.fast_accuracy_only) {
        if (exception_mask == 0) {
            auto acc = tryAccuracy2D(input, valid_mask, exception_mask, cfg.error, false, true);
            if (acc.passed) {
                uint32_t size = 2 + static_cast<uint32_t>(acc.payload.size());
                if (size < min_size) { best = std::move(acc); min_size = size; }
            }
        }

        auto acc_exc = tryAccuracy2D(input, valid_mask, exception_mask, cfg.error, true, true);
        if (acc_exc.passed) {
            uint32_t size = 2 + static_cast<uint32_t>(acc_exc.payload.size());
            if (size < min_size) { best = std::move(acc_exc); min_size = size; }
        }
    } else {
        auto acc = tryAccuracy2D(input, valid_mask, exception_mask, cfg.error, false);
        if (acc.passed) {
            uint32_t size = 2 + static_cast<uint32_t>(acc.payload.size());
            if (size < min_size) { best = std::move(acc); min_size = size; }
        }

        auto acc_exc = tryAccuracy2D(input, valid_mask, exception_mask, cfg.error, true);
        if (acc_exc.passed) {
            uint32_t size = 2 + static_cast<uint32_t>(acc_exc.payload.size());
            if (size < min_size) { best = std::move(acc_exc); min_size = size; }
        }

        for (uint8_t prec : {12, 14, 16, 18, 20, 22, 24}) {
            auto p = tryPrecision2D(input, valid_mask, exception_mask, cfg.error, prec, false);
            if (p.passed) {
                uint32_t size = 2 + static_cast<uint32_t>(p.payload.size());
                if (size < min_size) { best = std::move(p); min_size = size; }
            }
            if (cfg.try_precision_exceptions) {
                auto p_exc = tryPrecision2D(input, valid_mask, exception_mask, cfg.error, prec, true);
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

static bool probePlane(int fd, uint64_t x, const ProbeOptions& opt,
                       const RzfpXPlaneCodecConfig& cfg, ProbeStats& stats) {
    const uint64_t ny = opt.ny;
    const uint64_t nz = opt.nz;
    std::vector<float> plane(ny * nz);
    if (!readRawPlane(fd, x, opt.nx, ny, nz, plane.data())) {
        std::cerr << "Error: failed to read plane x=" << x << std::endl;
        return false;
    }

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

            auto c = encodeBest2D(block, valid_mask, cfg);
            ++stats.total_blocks;
            stats.total_raw_bytes += 16 * sizeof(float);
            stats.total_compressed_bytes += 2 + c.payload.size();

            switch (c.codec) {
                case Codec2D::RawFloat32: ++stats.raw_blocks; break;
                case Codec2D::ConstantZero: ++stats.zero_blocks; break;
                case Codec2D::ConstantValue: ++stats.constant_blocks; break;
                case Codec2D::ZfpAccuracy: ++stats.accuracy_blocks; break;
                case Codec2D::ZfpAccuracyExceptions: ++stats.accuracy_exc_blocks; break;
                case Codec2D::ZfpPrecision: ++stats.precision_blocks; break;
                case Codec2D::ZfpPrecisionExceptions: ++stats.precision_exc_blocks; break;
            }

            double max_rel = 0.0;
            float decoded[16];
            std::fill(decoded, decoded + 16, 0.0f);
#ifdef ERWT3D_HAVE_RZFP
            if (c.codec == Codec2D::RawFloat32) {
                std::memcpy(decoded, c.payload.data(), c.payload.size());
            } else if (c.codec == Codec2D::ConstantZero) {
                std::fill(decoded, decoded + 16, 0.0f);
            } else if (c.codec == Codec2D::ConstantValue) {
                float v = 0.0f;
                std::memcpy(&v, c.payload.data(), sizeof(float));
                std::fill(decoded, decoded + 16, v);
            } else if (c.codec == Codec2D::ZfpAccuracy ||
                       c.codec == Codec2D::ZfpAccuracyExceptions ||
                       c.codec == Codec2D::ZfpPrecision ||
                       c.codec == Codec2D::ZfpPrecisionExceptions) {
                const bool is_accuracy = (c.codec == Codec2D::ZfpAccuracy ||
                                          c.codec == Codec2D::ZfpAccuracyExceptions);
                const bool has_exceptions = (c.codec == Codec2D::ZfpAccuracyExceptions ||
                                             c.codec == Codec2D::ZfpPrecisionExceptions);

                const uint8_t parameter = c.payload[0];
                size_t zfp_offset = 1;
                size_t zfp_size = c.payload.size() - 1;
                uint16_t exc_mask16 = 0;
                const float* exc_vals = nullptr;
                if (has_exceptions) {
                    const uint8_t exc_count = c.payload[1];
                    exc_mask16 = *reinterpret_cast<const uint16_t*>(c.payload.data() + 2);
                    const size_t header_size = 2 + sizeof(uint16_t);
                    zfp_offset = header_size;
                    zfp_size = c.payload.size() - header_size - exc_count * sizeof(float);
                    exc_vals = reinterpret_cast<const float*>(c.payload.data() + header_size + zfp_size);
                }

                zfp_stream* stream = zfp_stream_open(nullptr);
                if (is_accuracy) {
                    const double tolerance = std::ldexp(1.0, static_cast<int8_t>(parameter));
                    zfp_stream_set_accuracy(stream, tolerance);
                } else {
                    zfp_stream_set_precision(stream, parameter);
                }
                std::vector<uint8_t> buf = c.payload;
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
            }
#else
            std::memcpy(decoded, block, sizeof(block));
#endif
            checkBlock2D(block, decoded, valid_mask, cfg.error, max_rel);
            stats.max_relative_error = std::max(stats.max_relative_error, max_rel);
            if (max_rel >= cfg.error.contest_bound) ++stats.violation_count;
        }
    }
    return true;
}

static std::vector<uint8_t> parsePrecisions(const std::string& s) {
    std::vector<uint8_t> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find(',', start);
        if (end == std::string::npos) end = s.size();
        int v = std::stoi(s.substr(start, end - start));
        out.push_back(static_cast<uint8_t>(v));
        start = end + 1;
    }
    return out;
}

static void printUsage(const char* prog) {
    std::cerr << "Usage: " << prog << " --input PATH --nx N --ny N --nz N [options]\n"
              << "Options:\n"
              << "  --samples N      Number of X-planes to sample (default: 10)\n"
              << "  --rel-bound V    Contest relative bound (default: 0.001)\n"
              << "  --seed N         Random seed (default: 20260511)\n"
              << "  --fast           Use fast accuracy-only encoding\n"
              << "  --precisions LIST e.g. 16,18,20,22 (default full set)\n";
}

} // namespace

int main(int argc, char* argv[]) {
    ProbeOptions opt;
    for (int i = 1; i < argc; ++i) {
        auto next = [&]() -> const char* {
            if (i + 1 < argc) return argv[++i];
            std::cerr << "Error: " << argv[i] << " requires a value\n";
            std::exit(1);
            return nullptr;
        };
        if (std::strcmp(argv[i], "--input") == 0) opt.raw_path = next();
        else if (std::strcmp(argv[i], "--nx") == 0) opt.nx = std::stoull(next());
        else if (std::strcmp(argv[i], "--ny") == 0) opt.ny = std::stoull(next());
        else if (std::strcmp(argv[i], "--nz") == 0) opt.nz = std::stoull(next());
        else if (std::strcmp(argv[i], "--samples") == 0) opt.samples = std::stoi(next());
        else if (std::strcmp(argv[i], "--rel-bound") == 0) opt.rel_bound = std::stod(next());
        else if (std::strcmp(argv[i], "--seed") == 0) opt.seed = static_cast<uint32_t>(std::stoul(next()));
        else if (std::strcmp(argv[i], "--fast") == 0) opt.fast = true;
        else if (std::strcmp(argv[i], "--precisions") == 0) opt.custom_precisions = parsePrecisions(next());
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]); return 0;
        } else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            printUsage(argv[0]); return 1;
        }
    }

    if (opt.raw_path.empty() || opt.nx == 0 || opt.ny == 0 || opt.nz == 0) {
        std::cerr << "Error: --input, --nx, --ny, --nz are required\n";
        printUsage(argv[0]); return 1;
    }

    int fd = open(opt.raw_path.c_str(), O_RDONLY);
    if (fd < 0) {
        std::cerr << "Error: cannot open raw file\n";
        return 1;
    }

    RelativeErrorConfig cfg;
    cfg.contest_bound = opt.rel_bound;
    cfg.internal_bound = opt.rel_bound * 0.75;
    cfg.policy = RelativeErrorPolicy::Strict;

    RzfpXPlaneCodecConfig codec_cfg;
    codec_cfg.error = cfg;
    codec_cfg.fast_accuracy_only = opt.fast;
    if (!opt.custom_precisions.empty()) {
        codec_cfg.precisions = opt.custom_precisions;
        codec_cfg.try_precision_exceptions = false;
    }

    std::mt19937 rng(opt.seed);
    std::uniform_int_distribution<uint64_t> dist(0, opt.nx - 1);

    ProbeStats total;
    for (int s = 0; s < opt.samples; ++s) {
        uint64_t x = dist(rng);
        ProbeStats st;
        if (!probePlane(fd, x, opt, codec_cfg, st)) {
            close(fd);
            return 1;
        }
        total.total_raw_bytes += st.total_raw_bytes;
        total.total_compressed_bytes += st.total_compressed_bytes;
        total.total_blocks += st.total_blocks;
        total.raw_blocks += st.raw_blocks;
        total.zero_blocks += st.zero_blocks;
        total.constant_blocks += st.constant_blocks;
        total.accuracy_blocks += st.accuracy_blocks;
        total.accuracy_exc_blocks += st.accuracy_exc_blocks;
        total.precision_blocks += st.precision_blocks;
        total.precision_exc_blocks += st.precision_exc_blocks;
        total.max_relative_error = std::max(total.max_relative_error, st.max_relative_error);
        total.violation_count += st.violation_count;
    }
    close(fd);

    const double ratio = total.total_raw_bytes > 0
                             ? static_cast<double>(total.total_compressed_bytes) / static_cast<double>(total.total_raw_bytes)
                             : 0.0;

    std::cout << "X-plane sidecar probe results (" << opt.samples << " samples)\n"
              << "  total blocks:        " << total.total_blocks << "\n"
              << "  raw blocks:          " << total.raw_blocks << "\n"
              << "  zero blocks:         " << total.zero_blocks << "\n"
              << "  constant blocks:     " << total.constant_blocks << "\n"
              << "  accuracy blocks:     " << total.accuracy_blocks << "\n"
              << "  accuracy+exc blocks: " << total.accuracy_exc_blocks << "\n"
              << "  precision blocks:    " << total.precision_blocks << "\n"
              << "  precision+exc blocks:" << total.precision_exc_blocks << "\n"
              << "  raw bytes:           " << total.total_raw_bytes << "\n"
              << "  compressed bytes:    " << total.total_compressed_bytes << "\n"
              << std::fixed << std::setprecision(4)
              << "  ratio:               " << ratio << "x\n"
              << "  max rel error:       " << total.max_relative_error << "\n"
              << "  violations:          " << total.violation_count << "\n";

    return total.violation_count == 0 ? 0 : 1;
}
