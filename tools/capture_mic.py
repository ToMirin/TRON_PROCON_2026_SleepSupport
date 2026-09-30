#!/usr/bin/env python3
"""dma_test.cの2秒録音を受信し、検証済みの16kHz/mono/16bit WAVを保存する。

macOS/Linux、Python標準ライブラリのみ。シリアルモニタは先に閉じる。
例: python3 tools/capture_mic.py --port /dev/tty.usbmodem1102 --output /tmp/mic.wav
受信開始後、ボードのSW1_USERを押して話す。再録音時はボードをリセット。
--inputは保存したテキストログの変換・受信処理の検証用。
"""

import argparse
import os
from pathlib import Path
import select
import struct
import time
import wave


def decode_recording(lines):
    """順序、長さ、FNV-1aを検査する。途中切れ・欠落時にはWAVを作らない。"""
    pcm = None
    for line in lines:
        fields = line.strip().split()
        if not fields:
            continue
        if fields[0] == "PCM_ABORT:":
            raise ValueError("ボードが録音を中止しました。リセットして再試行してください。")
        if fields[0] == "PCM_BEGIN":
            if pcm is not None or fields != ["PCM_BEGIN", "16000", "32000"]:
                raise ValueError("録音ヘッダーが不正、または録音が途中で再開始されました。")
            pcm = bytearray()
        elif fields[0] == "PCM" and pcm is not None:
            if len(fields) != 3 or int(fields[1]) != len(pcm) // 2:
                raise ValueError("PCM行の順序不正・欠落を検出しました。")
            payload = fields[2]
            if len(payload) != 256 or any(c not in "0123456789abcdefABCDEF" for c in payload):
                raise ValueError("PCMの16進データが不正です。")
            for offset in range(0, len(payload), 4):
                pcm.extend(struct.pack("<H", int(payload[offset:offset + 4], 16)))
            if len(pcm) > 64000:
                raise ValueError("PCMが規定の長さを超えています。")
        elif fields[0] == "PCM_END" and pcm is not None:
            if len(fields) != 2 or len(pcm) != 64000:
                raise ValueError("録音データが不足しています。")
            checksum = 2166136261
            for byte in pcm:
                checksum = ((checksum ^ byte) * 16777619) & 0xFFFFFFFF
            if checksum != int(fields[1], 16):
                raise ValueError("チェックサム不一致：転送データが破損しています。")
            return bytes(pcm)
    raise ValueError("録音完了まで受信できませんでした。")


def serial_lines(port, timeout):
    import termios
    import tty

    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    original = None
    try:
        original = termios.tcgetattr(fd)
        tty.setraw(fd, termios.TCSANOW)
        settings = termios.tcgetattr(fd)
        settings[4] = settings[5] = termios.B115200
        settings[2] |= termios.CLOCAL | termios.CREAD
        settings[2] &= ~getattr(termios, "CRTSCTS", 0)
        termios.tcsetattr(fd, termios.TCSANOW, settings)
        print("受信待機中。SW1_USERを押して2秒間話してください。その後、転送を待ちます。", flush=True)
        deadline = time.monotonic() + timeout
        pending = bytearray()
        while time.monotonic() < deadline:
            ready, _, _ = select.select([fd], [], [], min(1.0, max(0.0, deadline - time.monotonic())))
            if not ready:
                continue
            chunk = os.read(fd, 4096)
            if not chunk:
                raise ValueError("シリアルポートが切断されました。")
            pending.extend(chunk)
            while b"\n" in pending:
                line, _, pending = pending.partition(b"\n")
                yield line.decode("ascii", errors="replace")
            if len(pending) > 4096:
                raise ValueError("シリアルデータ形式が不正です。ボーレートを確認してください。")
        raise ValueError("受信タイムアウト。ボードをリセットし、受信開始後にSW1_USERを押してください。")
    finally:
        try:
            if original is not None:
                termios.tcsetattr(fd, termios.TCSANOW, original)
        finally:
            os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--port")
    source.add_argument("--input", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=120)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("出力先が既に存在します。別のファイル名を指定してください。")
    try:
        if args.input:
            with args.input.open() as stream:
                pcm = decode_recording(stream)
        else:
            lines = serial_lines(args.port, args.timeout)
            try:
                pcm = decode_recording(lines)
            finally:
                lines.close()
        with args.output.open("xb") as stream:
            with wave.open(stream, "wb") as wav:
                wav.setnchannels(1)
                wav.setsampwidth(2)
                wav.setframerate(16000)
                wav.writeframes(pcm)
        samples = struct.unpack("<32000h", pcm)
        print(f"保存完了: {args.output.resolve()} (2秒 / 16kHz / mono / 16bit)")
        print(f"min={min(samples)} max={max(samples)}。PC側で追加のゲインは適用していません（STM32側の設定を確認してください）。")
    except (OSError, ValueError) as error:
        parser.exit(1, f"エラー: {error}\n")


if __name__ == "__main__":
    main()
