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

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

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

inline std::unique_ptr<orc::RowReader> openRowReader(orc::Reader &reader) {
    orc::RowReaderOptions rowOpts;
    return reader.createRowReader(rowOpts);
}

// Validate top-level shape (rows, cols, struct-rooted schema).
inline void validateShape(const orc::Reader &reader, const FileMetaData &fmd) {
    if (reader.getNumberOfRows() != fmd.numRows)
        throw std::runtime_error("ORC reader: row count mismatch — meta says " + std::to_string(fmd.numRows) +
                                 ", file has " + std::to_string(reader.getNumberOfRows()));
    const orc::Type &root = reader.getType();
    if (root.getKind() != orc::STRUCT)
        throw std::runtime_error("ORC reader: expected top-level struct in ORC file");
    if (root.getSubtypeCount() != fmd.numCols)
        throw std::runtime_error("ORC reader: column count mismatch — meta says " + std::to_string(fmd.numCols) +
                                 ", file has " + std::to_string(root.getSubtypeCount()));
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
    (void)options; // reserved for future push-down hints
    (void)ctx;

    auto reader = daphne_orc_detail::openOrcReader(filename);
    daphne_orc_detail::validateShape(*reader, fmd);
    const orc::Type &root = reader->getType();

    auto rowReader = daphne_orc_detail::openRowReader(*reader);
    constexpr uint64_t kBatchSize = 1024;
    auto batch = rowReader->createRowBatch(kBatchSize);

    if (fmd.isSingleValueType) {
        const ValueTypeCode vt = fmd.schema.empty() ? ValueTypeCode::F64 : fmd.schema[0];

        // ----- DenseMatrix<double> -----
        if (vt == ValueTypeCode::F64) {
            for (uint64_t c = 0; c < fmd.numCols; ++c)
                daphne_orc_detail::expectColumnKind(root, c, orc::DOUBLE, "F64");

            auto **out = reinterpret_cast<DenseMatrix<double> **>(res);
            DenseMatrix<double> *m = *out;
            double *vals = m->getValues();
            uint64_t rowOffset = 0;
            while (rowReader->next(*batch)) {
                auto *sb = dynamic_cast<orc::StructVectorBatch *>(batch.get());
                if (!sb)
                    throw std::runtime_error("ORC reader: top-level batch is not a struct");
                for (uint64_t c = 0; c < fmd.numCols; ++c) {
                    auto *col = sb->fields[c];
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
                daphne_orc_detail::expectColumnKind(root, c, orc::LONG, "SI64");

            auto **out = reinterpret_cast<DenseMatrix<int64_t> **>(res);
            DenseMatrix<int64_t> *m = *out;
            int64_t *vals = m->getValues();
            uint64_t rowOffset = 0;
            while (rowReader->next(*batch)) {
                auto *sb = dynamic_cast<orc::StructVectorBatch *>(batch.get());
                if (!sb)
                    throw std::runtime_error("ORC reader: top-level batch is not a struct");
                for (uint64_t c = 0; c < fmd.numCols; ++c) {
                    auto *col = sb->fields[c];
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

        // Pre-validate each column's expected type against the ORC schema.
        for (uint64_t c = 0; c < fmd.numCols; ++c) {
            const ValueTypeCode vtc = fmd.schema[c];
            if (vtc == ValueTypeCode::F64)
                daphne_orc_detail::expectColumnKind(root, c, orc::DOUBLE, "F64");
            else if (vtc == ValueTypeCode::SI64)
                daphne_orc_detail::expectColumnKind(root, c, orc::LONG, "SI64");
            else if (vtc == ValueTypeCode::STR)
                daphne_orc_detail::expectColumnKind(root, c, orc::STRING, "STR");
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
                auto *col = sb->fields[c];
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
