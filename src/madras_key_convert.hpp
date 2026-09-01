#pragma once
// madras_key_convert.hpp
// Shared value <-> sortable-key conversion, used by madras_cli and the Python
// bindings. No dependency beyond madras/dv1.

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>

#include "madras/dv1/common.hpp"
#include "madras/dv1/reader/static_trie_map.hpp"

namespace madras_cli {

using namespace madras::dv1;

// Encodes a signed 64-bit integer into a fixed 8-byte big-endian, sign-flipped
// form so unsigned lexicographic key comparison matches numeric ordering.
inline void EncodeInt64Sortable(int64_t v, uint8_t *out) {
    uint64_t u = (uint64_t) v;
    u ^= (1ULL << 63); // flip sign bit so negative < positive in unsigned compare
    for (int i = 7; i >= 0; i--) {
        out[i] = (uint8_t)(u & 0xFF);
        u >>= 8;
    }
}

inline int64_t DecodeInt64Sortable(const uint8_t *in) {
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) u = (u << 8) | in[i];
    u ^= (1ULL << 63);
    return (int64_t) u;
}

inline void EncodeDoubleSortable(double d, uint8_t *out) {
    uint64_t bits;
    memcpy(&bits, &d, sizeof(bits));
    if (bits & (1ULL << 63)) {
        bits = ~bits;           // negative: flip all bits
    } else {
        bits |= (1ULL << 63);   // positive: flip sign bit only
    }
    for (int i = 7; i >= 0; i--) {
        out[i] = (uint8_t)(bits & 0xFF);
        bits >>= 8;
    }
}

// Small integer power-of-10 helper for MST_DEC0..MST_DEC9 scaling.
inline double Pow10(int n) {
    double r = 1.0;
    bool neg = n < 0;
    if (neg) n = -n;
    for (int i = 0; i < n; i++) r *= 10.0;
    return neg ? (1.0 / r) : r;
}

// Converts a user-supplied text value into the sortable key encoding matching
// `data_type`. `converted` must have room for at least 8 bytes (numeric) or
// value.size() bytes (text/blob) -- caller sizes the buffer beforehand.
inline void ConvertValueToKey(const std::string &value, char data_type,
                               uint8_t *converted, uint32_t &cvt_len) {
    switch (data_type) {
        case MST_TEXT:
        case MST_BIN: {
            memcpy(converted, value.data(), value.size());
            cvt_len = (uint32_t) value.size();
            break;
        }
        case MST_INT:
        case MST_BIGINT:
        case MST_DATE:
        case MST_TIME:
        case MST_TIME_TZ:
        case MST_TIMESTAMP:
        case MST_TIMESTAMP_TZ:
        case MST_TIMESTAMP_MS:
        case MST_TIMESTAMP_NS:
        case MST_TIMESTAMP_SEC: {
            int64_t i64 = strtoll(value.c_str(), nullptr, 10);
            EncodeInt64Sortable(i64, converted);
            cvt_len = 8;
            break;
        }
        case MST_DECV: {
            double d = strtod(value.c_str(), nullptr);
            EncodeDoubleSortable(d, converted);
            cvt_len = 8;
            break;
        }
        default: {
            // MST_DEC0..MST_DEC9: fixed-point decimal scaled then stored as int64
            if (data_type >= MST_DEC0 && data_type <= MST_DEC9) {
                double d = strtod(value.c_str(), nullptr);
                d *= Pow10(data_type - MST_DEC0);
                int64_t i64 = (int64_t) d;
                EncodeInt64Sortable(i64, converted);
                cvt_len = 8;
            } else {
                // Unknown type: fall back to raw text copy.
                memcpy(converted, value.data(), value.size());
                cvt_len = (uint32_t) value.size();
            }
        }
    }
}

// Formats a single column value (already fetched via get_col_val) as text.
inline void AppendColValueText(char data_type, const col_value_ptr &cv, std::string &out) {
    if (cv.length == UINT32_MAX) return; // NULL -> empty field
    switch (data_type) {
        case MST_TEXT:
        case MST_BIN:
            out.append((const char *) cv.u8_ptr, cv.length);
            break;
        case MST_INT:
        case MST_DATE:
            out += std::to_string(*cv.i32_ptr);
            break;
        case MST_BIGINT:
        case MST_TIME: case MST_TIME_TZ:
        case MST_TIMESTAMP: case MST_TIMESTAMP_TZ:
        case MST_TIMESTAMP_MS: case MST_TIMESTAMP_NS:
        case MST_TIMESTAMP_SEC:
            out += std::to_string(*cv.i64_ptr);
            break;
        case MST_DECV:
        case MST_DEC0: case MST_DEC1: case MST_DEC2: case MST_DEC3: case MST_DEC4:
        case MST_DEC5: case MST_DEC6: case MST_DEC7: case MST_DEC8: case MST_DEC9:
            out += std::to_string(*cv.dbl_ptr);
            break;
        default:
            break;
    }
}

} // namespace madras_cli
