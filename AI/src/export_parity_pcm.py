from pathlib import Path
import argparse

import numpy as np


ROOT = Path(__file__).resolve().parents[2]

INPUT_PATH = (
    ROOT
    / "AI"
    / "dataset"
    / "metadata"
    / "09_integration_info"
    / "pcm_int16.npy"
)

OUTPUT_PATH = (
    ROOT
    / "AI"
    / "generated"
    / "parity_pcm_int16.h"
)

EXPECTED_SAMPLES = 16000


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Export one AI-09 PCM test vector "
            "as a C int16_t array for STM32 parity testing."
        )
    )

    parser.add_argument(
        "--index",
        type=int,
        default=0,
        help="Test vector index to export (default: 0)",
    )

    return parser.parse_args()


def select_pcm_vector(
    pcm: np.ndarray,
    index: int,
) -> np.ndarray:
    """
    Select one 16000-sample PCM vector.

    Supported layouts include:

        (16000,)
        (N, 16000)
        (N, 16000, 1)

    The returned array always has shape (16000,).
    """

    if pcm.dtype != np.int16:
        raise ValueError(
            f"Expected int16 PCM, got {pcm.dtype}"
        )

    if pcm.ndim == 1:
        if pcm.size != EXPECTED_SAMPLES:
            raise ValueError(
                f"Expected {EXPECTED_SAMPLES} samples, "
                f"got {pcm.size}"
            )

        if index != 0:
            raise ValueError(
                "Input contains only one vector, "
                "so --index must be 0"
            )

        return np.ascontiguousarray(pcm)

    num_vectors = pcm.shape[0]

    if not 0 <= index < num_vectors:
        raise ValueError(
            f"--index {index} is out of range "
            f"(0 ... {num_vectors - 1})"
        )

    selected = np.asarray(
        pcm[index]
    ).reshape(-1)

    if selected.size != EXPECTED_SAMPLES:
        raise ValueError(
            f"Selected vector has {selected.size} samples; "
            f"expected {EXPECTED_SAMPLES}. "
            f"Original shape: {pcm.shape}"
        )

    return np.ascontiguousarray(
        selected,
        dtype=np.int16,
    )


def write_header(
    pcm: np.ndarray,
    vector_index: int,
) -> None:
    lines: list[str] = []

    lines.append(
        "/* Generated for AI_emb_06 parity test. Do not edit. */"
    )
    lines.append(
        f"/* AI-09 test vector index: {vector_index} */"
    )
    lines.append(
        "#ifndef TRON_AI_PARITY_PCM_INT16_H"
    )
    lines.append(
        "#define TRON_AI_PARITY_PCM_INT16_H"
    )
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append(
        f"#define TRON_PARITY_PCM_SAMPLES {EXPECTED_SAMPLES}"
    )
    lines.append(
        f"#define TRON_PARITY_VECTOR_INDEX {vector_index}"
    )
    lines.append("")
    lines.append(
        "static const int16_t "
        "tron_parity_pcm[TRON_PARITY_PCM_SAMPLES] = {"
    )

    values_per_line = 16

    for start in range(
        0,
        pcm.size,
        values_per_line,
    ):
        chunk = pcm[
            start:start + values_per_line
        ]

        values = ", ".join(
            str(int(value))
            for value in chunk
        )

        if (
            start + values_per_line
            < pcm.size
        ):
            values += ","

        lines.append(
            f"    {values}"
        )

    lines.append("};")
    lines.append("")
    lines.append(
        "#endif /* TRON_AI_PARITY_PCM_INT16_H */"
    )
    lines.append("")

    OUTPUT_PATH.write_text(
        "\n".join(lines),
        encoding="utf-8",
    )


def main() -> None:
    args = parse_args()

    pcm_all = np.load(INPUT_PATH)

    print(f"Input file : {INPUT_PATH}")
    print(f"Input shape: {pcm_all.shape}")
    print(f"Input dtype: {pcm_all.dtype}")

    pcm = select_pcm_vector(
        pcm_all,
        args.index,
    )

    write_header(
        pcm,
        args.index,
    )

    print("")
    print(
        f"Selected index : {args.index}"
    )
    print(
        f"Selected shape : {pcm.shape}"
    )
    print(
        f"Selected count : {pcm.size}"
    )
    print(
        f"Selected min   : {pcm.min()}"
    )
    print(
        f"Selected max   : {pcm.max()}"
    )
    print(
        f"Output         : {OUTPUT_PATH}"
    )


if __name__ == "__main__":
    main()