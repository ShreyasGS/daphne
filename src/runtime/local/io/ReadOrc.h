/*
 * Copyright 2026 The DAPHNE Consortium
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <runtime/local/context/DaphneContext.h>
#include <runtime/local/datastructures/DataObjectFactory.h>
#include <runtime/local/datastructures/DenseMatrix.h>
#include <runtime/local/datastructures/Frame.h>
#include <runtime/local/datastructures/ValueTypeCode.h>
#include <runtime/local/io/FileMetaData.h>

#include <orc/OrcFile.hh>
#include <orc/Reader.hh>
#include <orc/Type.hh>
#include <orc/Vector.hh>

#include <algorithm>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// ****************************************************************************
// readOrc — reader for Apache ORC.
//
// Signature matches the reader/writer extensibility interface introduced in
// PR #993 (daphne-eu/daphne). Until PR #993 is merged, this function is
// invoked from the if/else dispatcher in `Read.h` based on the `.orc` file
// extension.
//
// Internally uses Apache ORC's C++ library directly (not Arrow's ORC adapter,
// since the dev container's Arrow is built without -DARROW_ORC=ON).
// ****************************************************************************

namespace daphne_orc_detail {

inline std::unique_ptr<orc::Reader> openOrcReader(const char *filename) {
    try {
        orc::ReaderOptions opts;
        return orc::createReader(orc::readLocalFile(std::string(filename)), opts);
    } catch (const std::exception &e) {
        throw std::runtime_error(std::string("ORC reader: failed to open file '") + filename + "' (" + e.what() + ")");
    }
}

inline std::unique_ptr<orc::RowReader> openRowReader(orc::Reader &reader, const std::vector<std::string> &names) {
    orc::RowReaderOptions rowOpts;
    if (!names.empty()) {
        std::list<std::string> nameList(names.begin(), names.end());
        rowOpts.include(nameList);
    }
    return reader.createRowReader(rowOpts);
}

// Validate top-level shape (rows, cols, struct-rooted schema). When
// projection is active, the file's total column count is expected to be
// >= fmd.numCols and is not checked here.
inline void validateShape(const orc::Reader &reader, const FileMetaData &fmd, bool projected) {
    if (reader.getNumberOfRows() != fmd.numRows)
        throw std::runtime_error("ORC reader: row count mismatch — meta says " + std::to_string(fmd.numRows) +
                                 ", file has " + std::to_string(reader.getNumberOfRows()));
    const orc::Type &root = reader.getType();
    if (root.getKind() != orc::STRUCT)
        throw std::runtime_error("ORC reader: expected top-level struct in ORC file");
    if (!projected && root.getSubtypeCount() != fmd.numCols)
        throw std::runtime_error("ORC reader: column count mismatch — meta says " + std::to_string(fmd.numCols) +
                                 ", file has " + std::to_string(root.getSubtypeCount()));
}

// Split "a, b,c" into ["a", "b", "c"]. Trim ASCII whitespace around each
// entry, throw on empty entries, throw on duplicates.
inline std::vector<std::string> parseColumnList(const std::string &s) {
    auto trim = [](std::string t) -> std::string {
        const auto b = t.find_first_not_of(" \t");
        if (b == std::string::npos)
            return "";
        const auto e = t.find_last_not_of(" \t");
        return t.substr(b, e - b + 1);
    };

    if (trim(s).empty())
        throw std::runtime_error("ORC reader: columns option must be a non-empty comma-separated list");

    std::vector<std::string> result;
    size_t start = 0;
    while (true) {
        const size_t comma = s.find(',', start);
        const size_t end = (comma == std::string::npos) ? s.size() : comma;
        std::string tok = trim(s.substr(start, end - start));
        if (tok.empty())
            throw std::runtime_error("ORC reader: columns option has an empty entry");
        for (const auto &existing : result)
            if (existing == tok)
                throw std::runtime_error("ORC reader: columns option has duplicate entry '" + tok + "'");
        result.push_back(std::move(tok));
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return result;
}

// Given the file's root struct type and a list of requested column names,
// return the mapping from output index to file column index. Throws with
// a human-readable list of available names if any requested name is
// missing.
inline std::vector<uint64_t> resolveProjection(const orc::Type &root, const std::vector<std::string> &names) {
    std::vector<uint64_t> result;
    result.reserve(names.size());
    for (const auto &name : names) {
        bool found = false;
        for (uint64_t i = 0; i < root.getSubtypeCount(); ++i) {
            if (root.getFieldName(i) == name) {
                result.push_back(i);
                found = true;
                break;
            }
        }
        if (!found) {
            std::string avail;
            for (uint64_t i = 0; i < root.getSubtypeCount(); ++i) {
                if (i > 0)
                    avail += ", ";
                avail += root.getFieldName(i);
            }
            throw std::runtime_error("ORC reader: column '" + name + "' not found in file (available: " + avail + ")");
        }
    }
    return result;
}

// Throw if the column at index `c` does not have the expected ORC TypeKind.
inline void expectColumnKind(const orc::Type &root, uint64_t c, orc::TypeKind expected, const char *expectedLabel) {
    orc::TypeKind got = root.getSubtype(c)->getKind();
    if (got != expected)
        throw std::runtime_error(std::string("ORC reader: column ") + std::to_string(c) + " type mismatch — expected " +
                                 expectedLabel + ", got " + root.getSubtype(c)->toString());
}

// Reject a batch column that contains null entries.
inline void rejectNulls(const orc::ColumnVectorBatch *col, uint64_t cIdx) {
    if (col->hasNulls)
        throw std::runtime_error("ORC reader: null values not supported (yet) — column " + std::to_string(cIdx));
}

} // namespace daphne_orc_detail

// ----------------------------------------------------------------------------
// readOrc — entry point matching PR #993 reader signature.
// ----------------------------------------------------------------------------

inline void readOrc(void *res, const FileMetaData &fmd, const char *filename,
                    const std::map<std::string, std::string> &options, DaphneContext *ctx) {
    (void)ctx;

    // Parse `columns` option if present. Empty vector means no projection.
    std::vector<std::string> projNames;
    if (auto it = options.find("columns"); it != options.end())
        projNames = daphne_orc_detail::parseColumnList(it->second);
    const bool projected = !projNames.empty();

    auto reader = daphne_orc_detail::openOrcReader(filename);
    const orc::Type &root = reader->getType();
    daphne_orc_detail::validateShape(*reader, fmd, projected);

    // Resolve output-column -> file-column index mapping.
    std::vector<uint64_t> fileColIdx;
    if (projected) {
        fileColIdx = daphne_orc_detail::resolveProjection(root, projNames);
        if (fileColIdx.size() != fmd.numCols)
            throw std::runtime_error("ORC reader: columns option requests " + std::to_string(fileColIdx.size()) +
                                     " columns but meta says numCols=" + std::to_string(fmd.numCols));
    } else {
        fileColIdx.reserve(fmd.numCols);
        for (uint64_t i = 0; i < fmd.numCols; ++i)
            fileColIdx.push_back(i);
    }

    // liborc's include() compacts the batch's fields[] into file-schema
    // order, not user-requested order. batchColIdx maps each output
    // column c to its slot within the compacted batch.
    std::vector<uint64_t> batchColIdx;
    batchColIdx.reserve(fmd.numCols);
    if (projected) {
        std::vector<uint64_t> sorted = fileColIdx;
        std::sort(sorted.begin(), sorted.end());
        for (uint64_t c = 0; c < fmd.numCols; ++c) {
            const auto it = std::find(sorted.begin(), sorted.end(), fileColIdx[c]);
            batchColIdx.push_back(static_cast<uint64_t>(std::distance(sorted.begin(), it)));
        }
    } else {
        for (uint64_t i = 0; i < fmd.numCols; ++i)
            batchColIdx.push_back(i);
    }

    auto rowReader = daphne_orc_detail::openRowReader(*reader, projNames);
    constexpr uint64_t kBatchSize = 1024;
    auto batch = rowReader->createRowBatch(kBatchSize);

    if (fmd.isSingleValueType) {
        const ValueTypeCode vt = fmd.schema.empty() ? ValueTypeCode::F64 : fmd.schema[0];

        // ----- DenseMatrix<double> -----
        if (vt == ValueTypeCode::F64) {
            for (uint64_t c = 0; c < fmd.numCols; ++c)
                daphne_orc_detail::expectColumnKind(root, fileColIdx[c], orc::DOUBLE, "F64");

            auto **out = reinterpret_cast<DenseMatrix<double> **>(res);
            DenseMatrix<double> *m = *out;
            double *vals = m->getValues();
            uint64_t rowOffset = 0;
            while (rowReader->next(*batch)) {
                auto *sb = dynamic_cast<orc::StructVectorBatch *>(batch.get());
                if (!sb)
                    throw std::runtime_error("ORC reader: top-level batch is not a struct");
                for (uint64_t c = 0; c < fmd.numCols; ++c) {
                    auto *col = sb->fields[batchColIdx[c]];
                    daphne_orc_detail::rejectNulls(col, c);
                    auto *dbl = dynamic_cast<orc::DoubleVectorBatch *>(col);
                    if (!dbl)
                        throw std::runtime_error("ORC reader: column " + std::to_string(c) +
                                                 " type mismatch — expected F64 (DoubleVectorBatch)");
                    for (uint64_t i = 0; i < batch->numElements; ++i)
                        vals[(rowOffset + i) * fmd.numCols + c] = dbl->data[i];
                }
                rowOffset += batch->numElements;
            }
            if (rowOffset != fmd.numRows)
                throw std::runtime_error("ORC reader: read " + std::to_string(rowOffset) + " rows; expected " +
                                         std::to_string(fmd.numRows));
            return;
        }

        // ----- DenseMatrix<int64_t> -----
        if (vt == ValueTypeCode::SI64) {
            for (uint64_t c = 0; c < fmd.numCols; ++c)
                daphne_orc_detail::expectColumnKind(root, fileColIdx[c], orc::LONG, "SI64");

            auto **out = reinterpret_cast<DenseMatrix<int64_t> **>(res);
            DenseMatrix<int64_t> *m = *out;
            int64_t *vals = m->getValues();
            uint64_t rowOffset = 0;
            while (rowReader->next(*batch)) {
                auto *sb = dynamic_cast<orc::StructVectorBatch *>(batch.get());
                if (!sb)
                    throw std::runtime_error("ORC reader: top-level batch is not a struct");
                for (uint64_t c = 0; c < fmd.numCols; ++c) {
                    auto *col = sb->fields[batchColIdx[c]];
                    daphne_orc_detail::rejectNulls(col, c);
                    auto *lng = dynamic_cast<orc::LongVectorBatch *>(col);
                    if (!lng)
                        throw std::runtime_error("ORC reader: column " + std::to_string(c) +
                                                 " type mismatch — expected SI64 (LongVectorBatch)");
                    for (uint64_t i = 0; i < batch->numElements; ++i)
                        vals[(rowOffset + i) * fmd.numCols + c] = lng->data[i];
                }
                rowOffset += batch->numElements;
            }
            if (rowOffset != fmd.numRows)
                throw std::runtime_error("ORC reader: read " + std::to_string(rowOffset) + " rows; expected " +
                                         std::to_string(fmd.numRows));
            return;
        }

        throw std::runtime_error("ORC reader: value type not supported by ORC reader (yet) — only F64 and SI64 are "
                                 "implemented");
    }

    // ----- Frame: per-column copy, type-dispatched per column -----
    {
        auto **outFrame = reinterpret_cast<Frame **>(res);
        Frame *f = *outFrame;

        // Pre-validate each output column's expected type against the file's TypeKind.
        for (uint64_t c = 0; c < fmd.numCols; ++c) {
            const ValueTypeCode vtc = fmd.schema[c];
            if (vtc == ValueTypeCode::F64)
                daphne_orc_detail::expectColumnKind(root, fileColIdx[c], orc::DOUBLE, "F64");
            else if (vtc == ValueTypeCode::SI64)
                daphne_orc_detail::expectColumnKind(root, fileColIdx[c], orc::LONG, "SI64");
            else if (vtc == ValueTypeCode::STR)
                daphne_orc_detail::expectColumnKind(root, fileColIdx[c], orc::STRING, "STR");
            else
                throw std::runtime_error("ORC reader: value type not supported by ORC reader (yet) — only F64, SI64, "
                                         "and STR are implemented (column " +
                                         std::to_string(c) + ")");
        }

        uint64_t rowOffset = 0;
        while (rowReader->next(*batch)) {
            auto *sb = dynamic_cast<orc::StructVectorBatch *>(batch.get());
            if (!sb)
                throw std::runtime_error("ORC reader: top-level batch is not a struct");
            for (uint64_t c = 0; c < fmd.numCols; ++c) {
                auto *col = sb->fields[batchColIdx[c]];
                daphne_orc_detail::rejectNulls(col, c);
                const ValueTypeCode vtc = fmd.schema[c];
                if (vtc == ValueTypeCode::F64) {
                    auto *dbl = dynamic_cast<orc::DoubleVectorBatch *>(col);
                    if (!dbl)
                        throw std::runtime_error("ORC reader: column " + std::to_string(c) +
                                                 " type mismatch — expected F64");
                    double *buf = f->getColumn<double>(c)->getValues();
                    for (uint64_t i = 0; i < batch->numElements; ++i)
                        buf[rowOffset + i] = dbl->data[i];
                } else if (vtc == ValueTypeCode::SI64) {
                    auto *lng = dynamic_cast<orc::LongVectorBatch *>(col);
                    if (!lng)
                        throw std::runtime_error("ORC reader: column " + std::to_string(c) +
                                                 " type mismatch — expected SI64");
                    int64_t *buf = f->getColumn<int64_t>(c)->getValues();
                    for (uint64_t i = 0; i < batch->numElements; ++i)
                        buf[rowOffset + i] = lng->data[i];
                } else { // STR (others rejected in pre-validation)
                    auto *sc = dynamic_cast<orc::StringVectorBatch *>(col);
                    if (!sc)
                        throw std::runtime_error("ORC reader: column " + std::to_string(c) +
                                                 " type mismatch — expected STR (StringVectorBatch)");
                    std::string *buf = f->getColumn<std::string>(c)->getValues();
                    for (uint64_t i = 0; i < batch->numElements; ++i)
                        buf[rowOffset + i] = (sc->length[i] > 0)
                                                 ? std::string(sc->data[i], static_cast<size_t>(sc->length[i]))
                                                 : std::string();
                }
            }
            rowOffset += batch->numElements;
        }
        if (rowOffset != fmd.numRows)
            throw std::runtime_error("ORC reader: read " + std::to_string(rowOffset) + " rows; expected " +
                                     std::to_string(fmd.numRows));
    }
}
