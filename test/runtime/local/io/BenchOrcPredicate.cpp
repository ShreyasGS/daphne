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

// Benchmark cases for ORC predicate push-down. All tests are tagged [bench]
// and are excluded from the normal [io] runs. Each case prints one CSV line
// on stdout of the form:
//   BENCH,<fixture>,<mode>,<wall_ms>,<rows>
// The harness in perf-test/predicate_harness.sh invokes each case with
// /usr/bin/time -v to also capture peak RSS.

#include <runtime/local/datastructures/DataObjectFactory.h>
#include <runtime/local/datastructures/Frame.h>
#include <runtime/local/datastructures/ValueTypeCode.h>
#include <runtime/local/io/FileMetaData.h>
#include <runtime/local/io/ReadOrc.h>

#include <tags.h>

#include <catch.hpp>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

FileMetaData makeFmd(uint64_t nrows) {
    std::vector<ValueTypeCode> schema{ValueTypeCode::SI64, ValueTypeCode::F64};
    std::vector<std::string> labels{"age", "value"};
    return FileMetaData(nrows, 2, false, schema, labels);
}

void benchOne(const char *fixtureLabel, uint64_t nrows, const char *path, bool withPredicate) {
    FileMetaData fmd = makeFmd(nrows);
    std::map<std::string, std::string> opts;
    if (withPredicate)
        opts["predicate"] = "age > 90";

    Frame *f = nullptr;
    if (!withPredicate) {
        std::vector<ValueTypeCode> schema{ValueTypeCode::SI64, ValueTypeCode::F64};
        std::vector<std::string> labels{"age", "value"};
        f = DataObjectFactory::create<Frame>(nrows, 2, schema.data(), labels.data(), false);
    }
    const auto t0 = std::chrono::steady_clock::now();
    readOrc(reinterpret_cast<void *>(&f), fmd, path, opts, nullptr);
    const auto t1 = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    const uint64_t rows = f ? f->getNumRows() : 0;
    std::cout << "BENCH," << fixtureLabel << "," << (withPredicate ? "predicate" : "baseline") << "," << ms << ","
              << rows << std::endl;
    if (f)
        DataObjectFactory::destroy(f);
}

} // namespace

// ---------------------------------------------------------------------------
// 1M row fixtures
// ---------------------------------------------------------------------------

TEST_CASE("bench, 1M sorted, baseline", "[bench]") {
    benchOne("1M_sorted", 1'000'000, "/data/predicate-bench/fixtures/predbench_1000000_sorted.orc", false);
}
TEST_CASE("bench, 1M sorted, predicate", "[bench]") {
    benchOne("1M_sorted", 1'000'000, "/data/predicate-bench/fixtures/predbench_1000000_sorted.orc", true);
}
TEST_CASE("bench, 1M shuffled, baseline", "[bench]") {
    benchOne("1M_shuffled", 1'000'000, "/data/predicate-bench/fixtures/predbench_1000000_shuffled.orc", false);
}
TEST_CASE("bench, 1M shuffled, predicate", "[bench]") {
    benchOne("1M_shuffled", 1'000'000, "/data/predicate-bench/fixtures/predbench_1000000_shuffled.orc", true);
}

// ---------------------------------------------------------------------------
// 10M row fixtures
// ---------------------------------------------------------------------------

TEST_CASE("bench, 10M sorted, baseline", "[bench]") {
    benchOne("10M_sorted", 10'000'000, "/data/predicate-bench/fixtures/predbench_10000000_sorted.orc", false);
}
TEST_CASE("bench, 10M sorted, predicate", "[bench]") {
    benchOne("10M_sorted", 10'000'000, "/data/predicate-bench/fixtures/predbench_10000000_sorted.orc", true);
}
TEST_CASE("bench, 10M shuffled, baseline", "[bench]") {
    benchOne("10M_shuffled", 10'000'000, "/data/predicate-bench/fixtures/predbench_10000000_shuffled.orc", false);
}
TEST_CASE("bench, 10M shuffled, predicate", "[bench]") {
    benchOne("10M_shuffled", 10'000'000, "/data/predicate-bench/fixtures/predbench_10000000_shuffled.orc", true);
}
