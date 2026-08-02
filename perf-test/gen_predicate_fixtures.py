#!/usr/bin/env python3
"""
gen_predicate_fixtures.py — generate ORC fixtures for the predicate push-down
benchmark.

Six fixtures across two sizes (1M and 10M rows) x two data-order variants
(sorted, shuffled) x one column layout (age int64 + value double). Small
stripe size (1 MiB) so each file has many stripes and stripe-level skipping
is observable.

The `age` values are uniformly random in [0, 100). The predicate we benchmark
is `age > 90`, matching ~10% of rows. On the sorted variant that predicate
skips ~90% of stripes at the SearchArgument layer; on the shuffled variant
it skips almost nothing.

Run on the VM inside the DAPHNE dev container:

    source /data/venv/bin/activate
    python3 /data/predicate-bench/gen_predicate_fixtures.py

Fixtures land in /data/predicate-bench/fixtures/ alongside their .meta files.
"""

import json
import os
import numpy as np
import pyarrow as pa
import pyarrow.orc as orc

OUT_DIR = "/data/predicate-bench/fixtures"
STRIPE_SIZE = 1024 * 1024  # 1 MiB
SEED = 42

os.makedirs(OUT_DIR, exist_ok=True)
rng = np.random.default_rng(SEED)


def write_fixture(nrows: int, variant: str) -> None:
    ages = rng.integers(0, 100, size=nrows, dtype=np.int64)
    if variant == "sorted":
        ages.sort()
    elif variant == "shuffled":
        pass  # already random
    else:
        raise ValueError(variant)
    values = rng.standard_normal(size=nrows)

    basename = f"predbench_{nrows}_{variant}.orc"
    path = os.path.join(OUT_DIR, basename)
    table = pa.table(
        {
            "age": pa.array(ages, type=pa.int64()),
            "value": pa.array(values, type=pa.float64()),
        }
    )
    orc.write_table(table, path, stripe_size=STRIPE_SIZE)
    print(f"  wrote {basename} ({nrows} rows, {variant})")

    meta = {
        "numRows": int(nrows),
        "numCols": 2,
        "schema": [
            {"label": "age", "valueType": "si64"},
            {"label": "value", "valueType": "f64"},
        ],
    }
    with open(path + ".meta", "w") as f:
        json.dump(meta, f, indent=4)
        f.write("\n")


for nrows in (1_000_000, 10_000_000):
    for variant in ("sorted", "shuffled"):
        write_fixture(nrows, variant)

print("\nAll predicate-bench fixtures generated in", OUT_DIR)
