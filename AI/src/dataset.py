"""
Common TensorFlow dataset pipeline.

This module loads AI-03 generated WAV clips,
applies AI-04 preprocessing and AI-05 normalization,
and creates tf.data.Dataset objects.

Input WAV:
    sample rate : 16000 Hz
    channels    : mono
    format      : signed int16 PCM
    samples     : 16000

Feature:
    normalized Log-Mel
    shape       : (98, 40, 1)
    dtype       : float32

Label:
    OTHER = 0
    SNORE = 1
    COUGH = 2
"""

from pathlib import Path

import numpy as np
import pandas as pd
import tensorflow as tf

from scipy.io import wavfile

from .preprocess import (
    preprocess_pcm16,
    NUM_FRAMES,
    NUM_MEL_BINS,
)


# ============================================================
# Constants
# ============================================================

SAMPLE_RATE = 16000
INPUT_SAMPLES = 16000

DEFAULT_BATCH_SIZE = 32
DEFAULT_RANDOM_SEED = 2026


# ============================================================
# Normalization statistics
# ============================================================

def load_normalization_stats(
    path: str | Path,
) -> tuple[np.ndarray, np.ndarray]:
    """
    Load TRAIN-only mean/std calculated in AI-05.

    Returns
    -------
    mean : np.ndarray
        shape = (40,), dtype = float32

    std : np.ndarray
        shape = (40,), dtype = float32
    """

    path = Path(path)

    if not path.exists():
        raise FileNotFoundError(
            f"Normalization stats not found: {path}"
        )

    with np.load(path) as stats:
        mean = stats["mean"].astype(
            np.float32
        )

        std = stats["std"].astype(
            np.float32
        )

    expected_shape = (
        NUM_MEL_BINS,
    )

    if mean.shape != expected_shape:
        raise ValueError(
            f"Invalid mean shape: {mean.shape}"
        )

    if std.shape != expected_shape:
        raise ValueError(
            f"Invalid std shape: {std.shape}"
        )

    if not np.isfinite(mean).all():
        raise ValueError(
            "mean contains NaN or Inf"
        )

    if not np.isfinite(std).all():
        raise ValueError(
            "std contains NaN or Inf"
        )

    if np.any(std <= 0):
        raise ValueError(
            "std contains zero or negative values"
        )

    return mean, std


# ============================================================
# Split manifest
# ============================================================

def load_split_manifest(
    path: str | Path,
) -> pd.DataFrame:
    """
    Load AI-05 split manifest.
    """

    path = Path(path)

    if not path.exists():
        raise FileNotFoundError(
            f"Split manifest not found: {path}"
        )

    manifest = pd.read_csv(path)

    required_columns = {
        "relative_path",
        "class_id",
        "class_name",
        "split",
    }

    missing = (
        required_columns
        - set(manifest.columns)
    )

    if missing:
        raise ValueError(
            "Missing manifest columns: "
            f"{sorted(missing)}"
        )

    valid_splits = {
        "TRAIN",
        "VALIDATION",
        "TEST",
    }

    invalid_splits = (
        set(manifest["split"].dropna())
        - valid_splits
    )

    if invalid_splits:
        raise ValueError(
            f"Invalid split values: "
            f"{sorted(invalid_splits)}"
        )

    return manifest


def select_split(
    manifest: pd.DataFrame,
    split: str,
) -> pd.DataFrame:
    """
    Extract one split while preserving manifest order.
    """

    split = split.upper()

    if split not in {
        "TRAIN",
        "VALIDATION",
        "TEST",
    }:
        raise ValueError(
            f"Invalid split: {split}"
        )

    result = (
        manifest[
            manifest["split"] == split
        ]
        .copy()
        .reset_index(drop=True)
    )

    return result


# ============================================================
# Single feature loading
# ============================================================

def load_feature(
    relative_path: str,
    *,
    clip_root: str | Path,
    mean: np.ndarray,
    std: np.ndarray,
) -> np.ndarray:
    """
    Load one AI-03 WAV clip and generate
    normalized Log-Mel features.

    Returns
    -------
    feature : np.ndarray
        shape = (98, 40, 1)
        dtype = float32
    """

    clip_root = Path(
        clip_root
    )

    path = (
        clip_root
        / relative_path
    )

    if not path.exists():
        raise FileNotFoundError(
            f"WAV not found: {path}"
        )

    sr, pcm = wavfile.read(
        path
    )

    if sr != SAMPLE_RATE:
        raise ValueError(
            f"Invalid sample rate: {sr} "
            f"({path})"
        )

    if pcm.dtype != np.int16:
        raise TypeError(
            f"Invalid WAV dtype: "
            f"{pcm.dtype} ({path})"
        )

    if pcm.shape != (
        INPUT_SAMPLES,
    ):
        raise ValueError(
            f"Invalid PCM shape: "
            f"{pcm.shape} ({path})"
        )

    feature = preprocess_pcm16(
        pcm,
        mean=mean,
        std=std,
    )

    expected_shape = (
        NUM_FRAMES,
        NUM_MEL_BINS,
    )

    if feature.shape != expected_shape:
        raise ValueError(
            f"Invalid feature shape: "
            f"{feature.shape}"
        )

    if not np.isfinite(
        feature
    ).all():
        raise ValueError(
            f"Feature contains NaN/Inf: "
            f"{path}"
        )

    # Add CNN channel dimension:
    # (98, 40) -> (98, 40, 1)
    feature = feature[
        :,
        :,
        np.newaxis,
    ]

    return feature.astype(
        np.float32,
        copy=False,
    )


# ============================================================
# Generator
# ============================================================

def feature_generator(
    dataframe: pd.DataFrame,
    *,
    clip_root: str | Path,
    mean: np.ndarray,
    std: np.ndarray,
):
    """
    Yield normalized feature / label pairs.

    feature:
        float32 [98, 40, 1]

    label:
        int32 scalar
    """

    for row in dataframe.itertuples(
        index=False
    ):

        feature = load_feature(
            row.relative_path,
            clip_root=clip_root,
            mean=mean,
            std=std,
        )

        label = np.int32(
            row.class_id
        )

        yield feature, label


# ============================================================
# TensorFlow Dataset
# ============================================================

OUTPUT_SIGNATURE = (
    tf.TensorSpec(
        shape=(
            NUM_FRAMES,
            NUM_MEL_BINS,
            1,
        ),
        dtype=tf.float32,
    ),
    tf.TensorSpec(
        shape=(),
        dtype=tf.int32,
    ),
)


def make_dataset(
    dataframe: pd.DataFrame,
    *,
    clip_root: str | Path,
    mean: np.ndarray,
    std: np.ndarray,
    training: bool,
    batch_size: int = DEFAULT_BATCH_SIZE,
    random_seed: int = DEFAULT_RANDOM_SEED,
    cache: bool = True,
    shuffle_buffer: int | None = None,
) -> tf.data.Dataset:
    """
    Create tf.data.Dataset.

    Pipeline:
        generator
            -> cache (optional)
            -> shuffle (TRAIN only)
            -> batch
            -> prefetch

    TEST / VALIDATION order is preserved when
    training=False.
    """

    if len(dataframe) == 0:
        raise ValueError(
            "Dataset dataframe is empty"
        )

    if batch_size <= 0:
        raise ValueError(
            "batch_size must be positive"
        )

    ds = tf.data.Dataset.from_generator(
        lambda: feature_generator(
            dataframe,
            clip_root=clip_root,
            mean=mean,
            std=std,
        ),
        output_signature=OUTPUT_SIGNATURE,
    )

    if cache:
        ds = ds.cache()

    if training:

        if shuffle_buffer is None:
            shuffle_buffer = len(
                dataframe
            )

        if shuffle_buffer <= 0:
            raise ValueError(
                "shuffle_buffer must be positive"
            )

        ds = ds.shuffle(
            buffer_size=shuffle_buffer,
            seed=random_seed,
            reshuffle_each_iteration=True,
        )

    ds = ds.batch(
        batch_size,
        drop_remainder=False,
    )

    ds = ds.prefetch(
        tf.data.AUTOTUNE
    )

    return ds