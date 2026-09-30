from pathlib import Path

import numpy as np
import pandas as pd


ROOT = Path(__file__).resolve().parents[2]

REFERENCE_PATH = (
    ROOT
    / "AI"
    / "dataset"
    / "metadata"
    / "09_integration_info"
    / "input_int8.npy"
)

STM32_PATH = (
    ROOT
    / "AI"
    / "dataset"
    / "metadata"
    / "09_integration_info"
    / "stm32_parity_output.csv"
)

VECTOR_INDEX = 0
EXPECTED_SIZE = 98 * 40


def load_reference() -> np.ndarray:
    data = np.load(REFERENCE_PATH)

    print(f"Reference shape: {data.shape}")
    print(f"Reference dtype: {data.dtype}")

    if data.dtype != np.int8:
        raise ValueError(
            f"Reference dtype must be int8, got {data.dtype}"
        )

    if data.ndim == 1:
        selected = data
    else:
        if VECTOR_INDEX >= data.shape[0]:
            raise ValueError(
                f"VECTOR_INDEX={VECTOR_INDEX} is out of range "
                f"for shape {data.shape}"
            )

        selected = np.asarray(
            data[VECTOR_INDEX]
        ).reshape(-1)

    if selected.size != EXPECTED_SIZE:
        raise ValueError(
            f"Expected {EXPECTED_SIZE} reference values, "
            f"got {selected.size}"
        )

    return selected.astype(
        np.int16,
        copy=False,
    )


def load_stm32() -> np.ndarray:
    df = pd.read_csv(STM32_PATH)

    required_columns = {
        "index",
        "value",
    }

    if not required_columns.issubset(df.columns):
        raise ValueError(
            f"CSV must contain columns {required_columns}, "
            f"got {list(df.columns)}"
        )

    if len(df) != EXPECTED_SIZE:
        raise ValueError(
            f"Expected {EXPECTED_SIZE} STM32 rows, "
            f"got {len(df)}"
        )

    expected_indices = np.arange(
        EXPECTED_SIZE,
        dtype=np.int64,
    )

    actual_indices = df["index"].to_numpy()

    if not np.array_equal(
        actual_indices,
        expected_indices,
    ):
        raise ValueError(
            "STM32 CSV indices are not exactly 0..3919"
        )

    values = df["value"].to_numpy()

    if np.any(values < -128) or np.any(values > 127):
        raise ValueError(
            "STM32 values are outside int8 range"
        )

    return values.astype(
        np.int16,
        copy=False,
    )


def main() -> None:
    reference = load_reference()
    stm32 = load_stm32()

    diff = stm32 - reference
    abs_diff = np.abs(diff)

    exact_matches = int(
        np.count_nonzero(diff == 0)
    )

    mismatch_count = (
        EXPECTED_SIZE - exact_matches
    )

    exact_match_rate = (
        exact_matches
        / EXPECTED_SIZE
        * 100.0
    )

    max_abs_diff = int(
        abs_diff.max()
    )

    mean_abs_diff = float(
        abs_diff.mean()
    )

    print("")
    print("=== AI_emb_06 parity result ===")
    print(
        f"Vector index       : {VECTOR_INDEX}"
    )
    print(
        f"Total values       : {EXPECTED_SIZE}"
    )
    print(
        f"Exact matches      : {exact_matches}"
    )
    print(
        f"Mismatches         : {mismatch_count}"
    )
    print(
        f"Exact match rate   : {exact_match_rate:.6f}%"
    )
    print(
        f"Max absolute diff  : {max_abs_diff}"
    )
    print(
        f"Mean absolute diff : {mean_abs_diff:.9f}"
    )

    if mismatch_count == 0:
        print("")
        print(
            "Result: PERFECT MATCH"
        )
        return

    print("")
    print("Difference histogram:")

    unique_diff, counts = np.unique(
        diff,
        return_counts=True,
    )

    for value, count in zip(
        unique_diff,
        counts,
    ):
        if value != 0:
            print(
                f"  diff {int(value):+d}: {int(count)}"
            )

    mismatch_indices = np.flatnonzero(
        diff != 0
    )

    print("")
    print("First mismatches:")

    for index in mismatch_indices[:20]:
        frame = int(
            index
            // 40
        )

        mel = int(
            index
            % 40
        )

        print(
            f"  index={index:4d} "
            f"frame={frame:2d} "
            f"mel={mel:2d} "
            f"PC={int(reference[index]):4d} "
            f"STM32={int(stm32[index]):4d} "
            f"diff={int(diff[index]):+d}"
        )


if __name__ == "__main__":
    main()