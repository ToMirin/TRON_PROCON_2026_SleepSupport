"""
audio preprocessing

Input:
    mono int16 PCM
    sample rate: 16000 Hz
    samples:     16000 (1.0 s)

Output:
    Log-Mel feature
    shape: (98, 40)

Specification:
    PCM scaling     : int16 / 32768.0
    STFT window     : periodic Hann
    frame length    : 400 samples (25 ms)
    frame step      : 160 samples (10 ms)
    FFT length      : 512
    padding         : none
    center          : false
    spectrum        : power spectrum |X|^2
    Mel bins        : 40
    Mel scale       : HTK
    fmin            : 60 Hz
    fmax            : 8000 Hz
    Log-Mel         : 10 * log10(max(mel_power, 1e-10))
"""

import numpy as np

from scipy.fft import rfft
from scipy.signal.windows import hann


# ============================================================
# Constants
# ============================================================

SAMPLE_RATE = 16000
INPUT_SAMPLES = 16000

FRAME_LENGTH = 400
FRAME_STEP = 160
FFT_LENGTH = 512

NUM_FRAMES = (
    (INPUT_SAMPLES - FRAME_LENGTH)
    // FRAME_STEP
    + 1
)

NUM_FFT_BINS = FFT_LENGTH // 2 + 1

NUM_MEL_BINS = 40

FMIN = 60.0
FMAX = 8000.0

LOG_FLOOR = 1e-10

STD_EPS = 1e-8


# ============================================================
# PCM
# ============================================================

def pcm16_to_float32(pcm: np.ndarray) -> np.ndarray:
    """
    Convert signed int16 PCM to float32.

    Mapping:
        -32768 -> -1.0
         32767 -> 32767 / 32768

    Peak normalization is NOT performed.
    """

    pcm = np.asarray(pcm)

    if pcm.dtype != np.int16:
        raise TypeError(
            f"Expected int16 PCM, got {pcm.dtype}"
        )

    if pcm.ndim != 1:
        raise ValueError(
            f"Expected mono 1-D PCM, got shape {pcm.shape}"
        )

    if len(pcm) != INPUT_SAMPLES:
        raise ValueError(
            f"Expected {INPUT_SAMPLES} samples, "
            f"got {len(pcm)}"
        )

    return (
        pcm.astype(np.float32)
        / np.float32(32768.0)
    )


# ============================================================
# Framing
# ============================================================

def frame_signal(
    x: np.ndarray,
) -> np.ndarray:
    """
    Split a 16000-sample waveform into
    98 frames of 400 samples with 160-sample hop.

    No padding is applied.
    """

    x = np.asarray(
        x,
        dtype=np.float32,
    )

    if x.ndim != 1:
        raise ValueError(
            f"Expected 1-D signal, got shape {x.shape}"
        )

    if len(x) != INPUT_SAMPLES:
        raise ValueError(
            f"Expected {INPUT_SAMPLES} samples, "
            f"got {len(x)}"
        )

    frames = np.empty(
        (NUM_FRAMES, FRAME_LENGTH),
        dtype=np.float32,
    )

    for i in range(NUM_FRAMES):

        start = i * FRAME_STEP
        end = start + FRAME_LENGTH

        frames[i] = x[start:end]

    return frames


# ============================================================
# STFT
# ============================================================

def periodic_hann_window() -> np.ndarray:
    """
    Return periodic Hann window.

    scipy.signal.windows.hann(..., sym=False)
    corresponds to a periodic Hann window.
    """

    return hann(
        FRAME_LENGTH,
        sym=False,
    ).astype(np.float32)


def stft_power(
    x: np.ndarray,
) -> np.ndarray:
    """
    Calculate STFT power spectrum.

    Returns:
        shape = (98, 257)
    """

    frames = frame_signal(x)

    window = periodic_hann_window()

    windowed = (
        frames
        * window[np.newaxis, :]
    )

    spectrum = rfft(
        windowed,
        n=FFT_LENGTH,
        axis=1,
    )

    power = (
        spectrum.real * spectrum.real
        + spectrum.imag * spectrum.imag
    )

    return power.astype(
        np.float32,
        copy=False,
    )


# ============================================================
# HTK Mel scale
# ============================================================

def hz_to_mel_htk(
    hz,
):
    """
    HTK Mel conversion.

    mel = 2595 * log10(1 + f / 700)
    """

    hz = np.asarray(
        hz,
        dtype=np.float64,
    )

    return (
        2595.0
        * np.log10(
            1.0 + hz / 700.0
        )
    )


def mel_to_hz_htk(
    mel,
):
    """
    Inverse HTK Mel conversion.
    """

    mel = np.asarray(
        mel,
        dtype=np.float64,
    )

    return (
        700.0
        * (
            np.power(
                10.0,
                mel / 2595.0,
            )
            - 1.0
        )
    )


# ============================================================
# Mel filter bank
# ============================================================

def create_mel_filterbank() -> np.ndarray:
    """
    Create fixed HTK Mel triangular filter bank.

    Shape:
        (257, 40)

    No Slaney / area normalization is applied.

    Each filter has:
        left point
        center point
        right point
    equally spaced on the HTK Mel scale.
    """

    mel_min = float(
        hz_to_mel_htk(FMIN)
    )

    mel_max = float(
        hz_to_mel_htk(FMAX)
    )

    # +2 for left/right endpoints
    mel_points = np.linspace(
        mel_min,
        mel_max,
        NUM_MEL_BINS + 2,
        dtype=np.float64,
    )

    hz_points = mel_to_hz_htk(
        mel_points
    )

    fft_freqs = np.linspace(
        0.0,
        SAMPLE_RATE / 2.0,
        NUM_FFT_BINS,
        dtype=np.float64,
    )

    filterbank = np.zeros(
        (NUM_FFT_BINS, NUM_MEL_BINS),
        dtype=np.float32,
    )

    for m in range(NUM_MEL_BINS):

        left = hz_points[m]
        center = hz_points[m + 1]
        right = hz_points[m + 2]

        # Rising slope
        rising = (
            (fft_freqs - left)
            / (center - left)
        )

        # Falling slope
        falling = (
            (right - fft_freqs)
            / (right - center)
        )

        weights = np.maximum(
            0.0,
            np.minimum(
                rising,
                falling,
            ),
        )

        filterbank[:, m] = (
            weights.astype(np.float32)
        )

    return filterbank


# Fixed reference filterbank
MEL_FILTERBANK = create_mel_filterbank()


# ============================================================
# Mel Power
# ============================================================

def mel_power(
    power_spectrum: np.ndarray,
) -> np.ndarray:
    """
    Convert STFT power spectrum to Mel power.

    Input:
        (98, 257)

    Output:
        (98, 40)
    """

    power_spectrum = np.asarray(
        power_spectrum,
        dtype=np.float32,
    )

    expected_shape = (
        NUM_FRAMES,
        NUM_FFT_BINS,
    )

    if (
        power_spectrum.shape
        != expected_shape
    ):
        raise ValueError(
            "Expected power spectrum shape "
            f"{expected_shape}, "
            f"got {power_spectrum.shape}"
        )

    mel = (
        power_spectrum
        @ MEL_FILTERBANK
    )

    return mel.astype(
        np.float32,
        copy=False,
    )


# ============================================================
# Log-Mel
# ============================================================

def log_mel(
    mel_power_spectrum: np.ndarray,
) -> np.ndarray:
    """
    Calculate Log-Mel in dB-like power scale.

    log_mel =
        10 * log10(max(mel_power, 1e-10))

    Output:
        shape = (98, 40)
    """

    mel_power_spectrum = np.asarray(
        mel_power_spectrum,
        dtype=np.float32,
    )

    expected_shape = (
        NUM_FRAMES,
        NUM_MEL_BINS,
    )

    if (
        mel_power_spectrum.shape
        != expected_shape
    ):
        raise ValueError(
            "Expected Mel shape "
            f"{expected_shape}, "
            f"got {mel_power_spectrum.shape}"
        )

    floored = np.maximum(
        mel_power_spectrum,
        np.float32(LOG_FLOOR),
    )

    result = (
        np.float32(10.0)
        * np.log10(floored)
    )

    return result.astype(
        np.float32,
        copy=False,
    )


# ============================================================
# Standardization
# ============================================================

def normalize_log_mel(
    features: np.ndarray,
    mean: np.ndarray,
    std: np.ndarray,
) -> np.ndarray:
    """
    Normalize each Mel bin using training-set statistics.

    mean/std shape:
        (40,)

    Actual mean/std values are calculated in AI-05
    using TRAINING DATA ONLY.
    """

    features = np.asarray(
        features,
        dtype=np.float32,
    )

    mean = np.asarray(
        mean,
        dtype=np.float32,
    )

    std = np.asarray(
        std,
        dtype=np.float32,
    )

    if features.shape != (
        NUM_FRAMES,
        NUM_MEL_BINS,
    ):
        raise ValueError(
            f"Invalid feature shape: {features.shape}"
        )

    if mean.shape != (
        NUM_MEL_BINS,
    ):
        raise ValueError(
            f"Invalid mean shape: {mean.shape}"
        )

    if std.shape != (
        NUM_MEL_BINS,
    ):
        raise ValueError(
            f"Invalid std shape: {std.shape}"
        )

    if np.any(std < STD_EPS):
        raise ValueError(
            "std contains zero or near-zero values"
        )

    normalized = (
        (features - mean[np.newaxis, :])
        / std[np.newaxis, :]
    )

    return normalized.astype(
        np.float32,
        copy=False,
    )


# ============================================================
# Full preprocessing
# ============================================================

def preprocess_pcm16(
    pcm: np.ndarray,
    mean: np.ndarray | None = None,
    std: np.ndarray | None = None,
) -> np.ndarray:
    """
    Full preprocessing pipeline.

    Without normalization:
        int16[16000]
            ->
        Log-Mel float32[98, 40]

    With mean/std:
        int16[16000]
            ->
        normalized float32[98, 40]
    """

    x = pcm16_to_float32(pcm)

    power = stft_power(x)

    mel = mel_power(power)

    features = log_mel(mel)

    if (
        mean is None
        and std is None
    ):
        return features

    if (
        mean is None
        or std is None
    ):
        raise ValueError(
            "mean and std must both be provided"
        )

    return normalize_log_mel(
        features,
        mean,
        std,
    )


def to_model_input(
    features: np.ndarray,
) -> np.ndarray:
    """
    Convert [98, 40] feature to CNN input:

        [1, 98, 40, 1]
    """

    features = np.asarray(
        features,
        dtype=np.float32,
    )

    if features.shape != (
        NUM_FRAMES,
        NUM_MEL_BINS,
    ):
        raise ValueError(
            f"Invalid feature shape: {features.shape}"
        )

    return features[
        np.newaxis,
        :,
        :,
        np.newaxis,
    ]