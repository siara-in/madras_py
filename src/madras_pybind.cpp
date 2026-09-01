// madras_pybind.cpp
// pybind11 bindings exposing madras::dv1::static_trie_map (read path) as a
// Python extension module. Mirrors the same primitives used in madras_cli.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>
#include <cmath>
#include <fstream>
#include <memory>
#include <vector>
#include <string>

#include "madras/dv1/common.hpp"
#include "madras/dv1/reader/static_trie_map.hpp"
#include "madras_key_convert.hpp"

typedef int64_t idx_t;

namespace py = pybind11;
using namespace madras::dv1;
using namespace madras_cli;

/* ---------------------------------------------------------
   Column value -> Python object
--------------------------------------------------------- */

static py::object ColValueToPy(char data_type, const col_value_ptr &cv) {
    if (cv.length == UINT32_MAX) return py::none();
    switch (data_type) {
        case MST_TEXT:
            return py::str(std::string((const char *) cv.u8_ptr, cv.length));
        case MST_BIN:
            return py::bytes(std::string((const char *) cv.u8_ptr, cv.length));
        case MST_INT:
        case MST_DATE:
            return py::int_(*cv.i32_ptr);
        case MST_BIGINT:
        case MST_TIME: case MST_TIME_TZ:
        case MST_TIMESTAMP: case MST_TIMESTAMP_TZ:
        case MST_TIMESTAMP_MS: case MST_TIMESTAMP_NS:
        case MST_TIMESTAMP_SEC:
            return py::int_(*cv.i64_ptr);
        case MST_DECV:
        case MST_DEC0: case MST_DEC1: case MST_DEC2: case MST_DEC3: case MST_DEC4:
        case MST_DEC5: case MST_DEC6: case MST_DEC7: case MST_DEC8: case MST_DEC9:
            return py::float_(*cv.dbl_ptr);
        default:
            return py::none();
    }
}

/* ---------------------------------------------------------
   MadrasReader: thin RAII wrapper, one per opened file
--------------------------------------------------------- */

class MadrasReader {
public:
    explicit MadrasReader(const std::string &path, bool use_mmap = true) {
        stm_ = std::unique_ptr<static_trie_map>(new static_trie_map());
        if (use_mmap) {
            // NOTE: assumes static_trie_map has a mmap-backed load(path) method.
            // If your build only supports load_from_mem, remove this branch and
            // always take the else path below (mmap performance is then simply
            // unavailable from Python, matching e.g. AVR/Arduino behavior).
            stm_->load(path.c_str());
            // if (!stm_->load(path.c_str())) {
            //     throw std::runtime_error("Failed to open/mmap: " + path);
            // }
        } else {
            std::ifstream f(path, std::ios::binary | std::ios::ate);
            if (!f) throw std::runtime_error("Cannot open file: " + path);
            std::streamsize len = f.tellg();
            f.seekg(0);
            owned_buf_.resize((size_t) len);
            f.read((char *) owned_buf_.data(), len);
            stm_->load_from_mem(owned_buf_.data(), owned_buf_.size());
            // if (!stm_->load_from_mem(owned_buf_.data(), owned_buf_.size())) {
            //     throw std::runtime_error("load_from_mem failed: " + path);
            // }
        }
    }

    py::dict metadata() {
        py::dict meta;
        uint32_t col_count = stm_->get_column_count();
        idx_t key_count = stm_->get_key_count();
        idx_t row_count = key_count > 0 ? key_count : stm_->get_node_count();
        meta["rows"] = row_count;
        meta["pk_columns"] = stm_->get_pk_col_count();
        py::list cols;
        for (uint32_t i = 0; i < col_count; i++) {
            py::dict c;
            c["index"] = i;
            c["name"] = std::string(stm_->get_column_name(i));
            c["type"] = std::string(1, stm_->get_column_type(i));
            c["encoding"] = std::string(1, stm_->get_column_encoding(i));
            cols.append(c);
        }
        meta["columns"] = cols;
        return meta;
    }

    std::vector<uint32_t> ResolveColumns(const std::vector<std::string> &names_or_empty) {
        std::vector<uint32_t> cols;
        uint32_t col_count = stm_->get_column_count();
        if (names_or_empty.empty()) {
            for (uint32_t i = 0; i < col_count; i++) {
                if (stm_->get_column_type(i) == 'S') break;
                cols.push_back(i);
            }
            return cols;
        }
        for (auto &name : names_or_empty) {
            for (uint32_t i = 0; i < col_count; i++) {
                if (name == stm_->get_column_name(i)) { cols.push_back(i); break; }
            }
        }
        return cols;
    }

    py::list get_rows(uint64_t offset, int64_t count, const std::vector<std::string> &cols_in) {
        auto cols = ResolveColumns(cols_in);
        idx_t key_count = stm_->get_key_count();
        idx_t row_count = key_count > 0 ? key_count : stm_->get_node_count();
        if (count < 0) count = (int64_t)(row_count > offset ? row_count - offset : 0);
        uint64_t end = offset + (uint64_t) count;
        if (end > (uint64_t) row_count) end = row_count;

        py::list rows;
        size_t max_len = ScratchSize(cols);
        std::vector<uint8_t> scratch(max_len + 8);
        for (uint64_t rid = offset; rid < end; rid++) {
            py::dict row;
            for (auto c : cols) {
                char dt = stm_->get_column_type(c);
                col_value_ptr cv; cv.u8_ptr = scratch.data();
                stm_->get_col_val(rid, c, cv);
                row[py::str(stm_->get_column_name(c))] = ColValueToPy(dt, cv);
            }
            rows.append(row);
        }
        return rows;
    }

    py::dict get_columns(uint64_t offset, int64_t count, const std::vector<std::string> &cols_in) {
        auto cols = ResolveColumns(cols_in);
        idx_t key_count = stm_->get_key_count();
        idx_t row_count = key_count > 0 ? key_count : stm_->get_node_count();
        if (count < 0) count = (int64_t)(row_count > offset ? row_count - offset : 0);
        uint64_t end = offset + (uint64_t) count;
        if (end > (uint64_t) row_count) end = row_count;
        uint64_t n = (end > offset) ? (end - offset) : 0;

        py::dict result;
        size_t max_len = ScratchSize(cols);
        std::vector<uint8_t> scratch(max_len + 8);

        for (auto c : cols) {
            char dt = stm_->get_column_type(c);
            std::string name = stm_->get_column_name(c);
            if (dt == MST_INT || dt == MST_DATE) {
                py::array_t<double> arr(n); // double so NULL can be represented as NaN
                auto buf = arr.mutable_unchecked<1>();
                for (uint64_t i = 0; i < n; i++) {
                    col_value_ptr cv; cv.u8_ptr = scratch.data();
                    stm_->get_col_val(offset + i, c, cv);
                    buf(i) = cv.length == UINT32_MAX ? std::nan("") : (double) *cv.i32_ptr;
                }
                result[py::str(name)] = arr;
            } else if (dt == MST_BIGINT ||
                       (dt >= MST_TIME && dt <= MST_TIMESTAMP_SEC)) {
                py::array_t<double> arr(n);
                auto buf = arr.mutable_unchecked<1>();
                for (uint64_t i = 0; i < n; i++) {
                    col_value_ptr cv; cv.u8_ptr = scratch.data();
                    stm_->get_col_val(offset + i, c, cv);
                    buf(i) = cv.length == UINT32_MAX ? std::nan("") : (double) *cv.i64_ptr;
                }
                result[py::str(name)] = arr;
            } else if (dt == MST_DECV || (dt >= MST_DEC0 && dt <= MST_DEC9)) {
                py::array_t<double> arr(n);
                auto buf = arr.mutable_unchecked<1>();
                for (uint64_t i = 0; i < n; i++) {
                    col_value_ptr cv; cv.u8_ptr = scratch.data();
                    stm_->get_col_val(offset + i, c, cv);
                    buf(i) = cv.length == UINT32_MAX ? std::nan("") : *cv.dbl_ptr;
                }
                result[py::str(name)] = arr;
            } else { // TEXT / BIN -> python list of str/bytes/None
                py::list col_list;
                for (uint64_t i = 0; i < n; i++) {
                    col_value_ptr cv; cv.u8_ptr = scratch.data();
                    stm_->get_col_val(offset + i, c, cv);
                    col_list.append(ColValueToPy(dt, cv));
                }
                result[py::str(name)] = col_list;
            }
        }
        return result;
    }

    std::vector<uint64_t> lookup_row_ids(uint32_t col_idx, const std::string &value) {
        char col_enc = stm_->get_column_encoding(col_idx);
        char data_type = stm_->get_column_type(col_idx);
        std::vector<uint64_t> row_ids;

        static_trie_map *trie_map = stm_.get();
        bool is_col_trie = false;
        if (col_idx >= stm_->get_pk_col_count() && col_enc == 'T') {
            trie_map = stm_->get_col_trie_map(col_idx);
            if (!trie_map) {
                throw std::runtime_error(
                    "get_col_trie_map returned null for column " + std::to_string(col_idx));
            }
            is_col_trie = true;
        }

        size_t max_len = stm_->get_max_key_len();
        uintxx_t vmax = stm_->get_max_val_len(col_idx);
        if (vmax > max_len) max_len = vmax;
        if (is_col_trie) {
            size_t ctm = trie_map->get_max_key_len();
            if (ctm + 1 > max_len) max_len = ctm + 1;
        }

        if (col_enc == 'W') {
            std::vector<uintxx_t> word_positions(value.size() + 1);
            splitter_result sr = get_dflt_word_splitter().split_into_words(
                (const uint8_t *) value.data(), value.size(), UINT32_MAX, word_positions.data());
            struct row_collect_ctx { std::vector<uint64_t> *ids; } ctx { &row_ids };
            auto cb = [](void *c, uintxx_t rid) -> bool {
                ((row_collect_ctx *) c)->ids->push_back(rid);
                return false;
            };
            for (size_t i = 0; i < sr.word_count; i++) {
                const char *word = value.data() + word_positions[i];
                size_t wlen = word_positions[i + 1] - word_positions[i];
                stm_->shortlist_word_records(col_idx, word, wlen, cb, &ctx);
            }
            if (sr.word_count > 1) {
                std::sort(row_ids.begin(), row_ids.end());
                std::vector<uint64_t> filtered;
                uint64_t cur = row_ids.empty() ? 0 : row_ids[0];
                size_t cnt = 0;
                for (size_t i = 0; i < row_ids.size(); i++) {
                    if (row_ids[i] == cur) cnt++;
                    else { if (cnt >= sr.word_count) filtered.push_back(cur); cur = row_ids[i]; cnt = 1; }
                }
                if (cnt >= sr.word_count) filtered.push_back(cur);
                row_ids.swap(filtered);
                std::vector<uint8_t> cv_buf(vmax);
                col_value_ptr cv; cv.u8_ptr = cv_buf.data();
                std::vector<uint64_t> verified;
                for (auto rid : row_ids) {
                    stm_->get_col_val(rid, col_idx, cv);
                    if (cv.length >= value.size() &&
                        memmem(cv.u8_ptr, cv.length, value.data(), value.size()) != nullptr)
                        verified.push_back(rid);
                }
                row_ids.swap(verified);
            }
        } else {
            std::vector<uint8_t> key(max_len);
            uint32_t key_len = 0;
            ConvertValueToKey(value, data_type, key.data(), key_len);

            iter_ctx it_ctx;
            it_ctx.init(trie_map->get_max_key_len(), trie_map->get_max_level());
            std::vector<uint8_t> out_key_buf(max_len);
            trie_map->find_first(key.data(), key_len, it_ctx, true);
            int out_key_len = trie_map->next(it_ctx, out_key_buf.data());
            while (out_key_len != -2) {
                if ((uint32_t) out_key_len == key_len &&
                    memcmp(out_key_buf.data(), key.data(), key_len) == 0) {
                    uintxx_t row_id = trie_map->leaf_rank1(it_ctx.node_path[it_ctx.cur_idx]);
                    if (is_col_trie) {
                        struct row_collect_ctx { std::vector<uint64_t> *ids; } rcc { &row_ids };
                        auto cb = [](void *c, uintxx_t rid) -> bool {
                            ((row_collect_ctx *) c)->ids->push_back(rid);
                            return false;
                        };
                        static_trie_map::emit_rev_rids(trie_map, row_id, cb, &rcc);
                    } else {
                        row_ids.push_back(row_id);
                    }
                    out_key_len = trie_map->next(it_ctx, out_key_buf.data());
                } else break;
            }
        }
        return row_ids;
    }

    py::dict get_columns_by_ids(const std::vector<uint64_t> &row_ids,
                                 const std::vector<std::string> &cols_in) {
        auto cols = ResolveColumns(cols_in);
        py::dict result;
        size_t max_len = ScratchSize(cols);
        std::vector<uint8_t> scratch(max_len + 8);
        for (auto c : cols) {
            char dt = stm_->get_column_type(c);
            py::list col_list;
            for (auto rid : row_ids) {
                col_value_ptr cv; cv.u8_ptr = scratch.data();
                stm_->get_col_val(rid, c, cv);
                col_list.append(ColValueToPy(dt, cv));
            }
            result[py::str(stm_->get_column_name(c))] = col_list;
        }
        return result;
    }

    uint32_t column_count() const { return stm_->get_column_count(); }

private:
    size_t ScratchSize(const std::vector<uint32_t> &cols) {
        size_t max_len = stm_->get_max_key_len();
        for (auto c : cols) {
            uintxx_t vlen = stm_->get_max_val_len(c);
            if (vlen > max_len) max_len = vlen;
        }
        return max_len;
    }

    std::unique_ptr<static_trie_map> stm_;
    std::vector<uint8_t> owned_buf_; // only used in the load_from_mem path
};

PYBIND11_MODULE(_madras_native, m) {
    m.doc() = "Native read bindings for madras (.mdsi) files";

    py::class_<MadrasReader>(m, "MadrasReader")
        .def(py::init<const std::string &, bool>(), py::arg("path"), py::arg("mmap") = true)
        .def("metadata", &MadrasReader::metadata)
        .def("get_rows", &MadrasReader::get_rows,
             py::arg("offset") = 0, py::arg("count") = -1,
             py::arg("columns") = std::vector<std::string>())
        .def("get_columns", &MadrasReader::get_columns,
             py::arg("offset") = 0, py::arg("count") = -1,
             py::arg("columns") = std::vector<std::string>())
        .def("lookup_row_ids", &MadrasReader::lookup_row_ids)
        .def("get_columns_by_ids", &MadrasReader::get_columns_by_ids,
             py::arg("row_ids"), py::arg("columns") = std::vector<std::string>())
        .def("column_count", &MadrasReader::column_count);
}
