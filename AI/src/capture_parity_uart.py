import argparse
from pathlib import Path

import serial


ROOT = Path(__file__).resolve().parents[2]

OUTPUT_PATH = (
    ROOT
    / "AI"
    / "dataset"
    / "metadata"
    / "09_integration_info"
    / "stm32_parity_output.csv"
)

BEGIN_MARKER = "AI_PREPROCESS_PARITY_BEGIN"
END_MARKER = "AI_PREPROCESS_PARITY_END"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()

    parser.add_argument(
        "--port",
        required=True,
        help="Serial port, e.g. /dev/cu.usbmodem1103",
    )

    parser.add_argument(
        "--baud",
        type=int,
        default=115200,
    )

    return parser.parse_args()


def main() -> None:
    args = parse_args()

    print(f"Opening: {args.port}")
    print(f"Baud   : {args.baud}")
    print("")
    print("Waiting for parity output...")
    print("Reset the NUCLEO board now.")

    rows: list[str] = []

    with serial.Serial(
        args.port,
        args.baud,
        timeout=10,
    ) as ser:

        capturing = False

        while True:
            raw = ser.readline()

            if not raw:
                continue

            line = raw.decode(
                "utf-8",
                errors="replace",
            ).strip()

            if line == BEGIN_MARKER:
                capturing = True
                print("BEGIN detected")
                continue

            if line == END_MARKER:
                print("END detected")
                break

            if capturing:
                if line == "index,value":
                    continue

                rows.append(line)

    if len(rows) != 3920:
        raise RuntimeError(
            f"Expected 3920 rows, got {len(rows)}"
        )

    OUTPUT_PATH.write_text(
        "index,value\n"
        + "\n".join(rows)
        + "\n",
        encoding="utf-8",
    )

    print("")
    print(f"Captured rows: {len(rows)}")
    print(f"Saved to     : {OUTPUT_PATH}")


if __name__ == "__main__":
    main()