# AIの仕組みと評価

[READMEに戻る](../README.md)

## 分類するもの

1秒間の音声から、次の3クラスのうち1つを選びます。

| ID | クラス | 内容 | イベント保存 |
|---|---|---|---|
| 0 | OTHER | いびき・咳以外 | しない |
| 1 | SNORE | いびき | する |
| 2 | COUGH | 咳 | する |

入力は16 kHz、モノラル、符号付き16 bit PCM、16,000サンプルです。マイクから取得した信号の前処理と推論をSTM32上で行います。分類は最大の出力値を持つクラスを選ぶ方式で、現在の保存処理には分類スコアによる追加の足切り閾値を設けていません。

## 前処理

```text
1秒PCM → Log-Mel特徴量 → 学習データ由来の平均・標準偏差で正規化
       → INT8量子化 → CNN → 3クラスの出力
```

Log-Melは、時間ごとの周波数成分を、人の聴覚に対応したMel尺度でまとめて対数化した特徴量です。

| 項目 | 設定 |
|---|---|
| PCMの数値変換 | `pcm / 32768.0` |
| 窓関数 | periodic Hann、400サンプル（25 ms） |
| フレーム間隔 | 160サンプル（10 ms） |
| FFT | 512点 |
| フレームの中央合わせ・端の埋め合わせ | なし |
| スペクトル | パワースペクトル |
| Melフィルタ | HTK方式、40 bins、60〜8,000 Hz |
| 対数化 | `10 * log10(max(mel_power, 1e-10))` |
| 正規化 | TRAINのみで計算したMel binごとの平均・標準偏差 |
| 特徴量の形状 | 98フレーム × 40 bins |
| モデル入力 | `[1, 98, 40, 1]`、符号付きINT8、3,920要素 |

実機前処理は [ai_preprocess.c](../Core/Src/ai_preprocess.c)、窓・フィルタ・正規化・量子化の定数は `AI/generated/` にあります。

## モデルと学習

小型の2次元CNNを使用しています。畳み込みの出力チャンネル数を8→16→32と増やし、最後に全体平均を取って3クラスへ分類します。

```text
入力 98×40×1
  → Conv2D(8, 3×3)  → BatchNorm → ReLU → MaxPool(2×2)
  → Conv2D(16, 3×3) → BatchNorm → ReLU → MaxPool(2×2)
  → Conv2D(32, 3×3) → BatchNorm → ReLU
  → GlobalAveragePooling → Dense(3) → Softmax
```

総パラメータ数は6,155（学習対象6,043、非学習112）です。

| 学習条件 | 設定 |
|---|---|
| フレームワーク | TensorFlow / Keras |
| 最適化 | Adam、初期学習率0.001 |
| 損失関数 | SparseCategoricalCrossentropy |
| バッチサイズ | 32 |
| 最大epoch数 | 50 |
| クラス不均衡への対応 | TRAIN件数に基づくクラス重み |
| データ拡張 | なし |
| モデル選択 | VALIDATION lossが最小のチェックポイント |
| 学習停止 | EarlyStopping、patience 8 |
| 学習率調整 | VALIDATION lossに応じて0.5倍、patience 3 |

最小VALIDATION lossは0.171507（epoch index 20）です。TESTは学習やチェックポイント選択には使用していません。データの構成は[学習データセット](datasets.md)を参照してください。

## 実機向けINT8量子化

float32モデルを学習後量子化（PTQ）でINT8モデルに変換しました。量子化の代表データはTRAINの5,425件だけを使用し、VALIDATION・TESTは含めていません。

| 項目 | 入力 | 出力 |
|---|---|---|
| 形状 | `[1, 98, 40, 1]` | `[1, 3]` |
| 型 | int8 | int8 |
| scale | 0.021447716280817986 | 1/256 |
| zero point | 2 | -128 |

実機採用モデルは [baseline_int8.tflite](../AI/model/baseline_int8.tflite) で、保存ファイルの容量は11,616 Bです。これはモデルファイル単体のサイズであり、ランタイムや前処理を含むファームウェアの容量ではありません。STM32側ではST Edge AIの生成コードとランタイムを使用します。

## 静的テストの条件

学習・モデル選択から分離したTESTの1,162クリップを使用しています。各クリップは1秒の音声で、両モデルに同じ前処理・TRAIN由来の正規化を適用します。float32版はKeras、INT8版はPC上のTFLite Interpreterで評価した結果です。

| クラス | TEST件数 |
|---|---:|
| OTHER | 228 |
| SNORE | 166 |
| COUGH | 768 |
| **合計** | **1,162** |

以下は保存済みの評価結果を掲載したもので、ドキュメント作成時に再学習・再推論したものではありません。

## 全体の分類性能

| 指標 | float32 | INT8（実機採用） |
|---|---:|---:|
| Accuracy | 95.0086% | 94.9225% |
| Macro-F1 | 0.923411 | 0.922490 |
| 正解件数 | 1,104 / 1,162 | 1,103 / 1,162 |

量子化によるAccuracyの変化は約−0.0861パーセントポイント、Macro-F1の変化は約−0.000921です。予測クラスが変わったのは5件で、正解から誤りが3件、誤りから正解が2件でした。

## クラス別の性能

| クラス | float32 Precision | INT8 Precision | float32 Recall | INT8 Recall | float32 F1 | INT8 F1 |
|---|---:|---:|---:|---:|---:|---:|
| OTHER | 0.879310 | 0.869198 | 0.894737 | 0.903509 | 0.886957 | 0.886022 |
| SNORE | 0.898810 | 0.908537 | 0.909639 | 0.897590 | 0.904192 | 0.903030 |
| COUGH | 0.982940 | 0.982917 | 0.975260 | 0.973958 | 0.979085 | 0.978417 |

Precision（適合率）は、そのクラスと予測したもののうち正解だった割合です。Recall（再現率）は、そのクラスの音を正しく拾えた割合です。F1は両者の調和平均、Macro-F1は3クラスのF1を同じ重みで平均した値です。

量子化後のSNOREはPrecisionが上がる一方、Recallは約1.20パーセントポイント下がっています。全体指標に加え、クラスごとの違いも示しています。

## 混同行列

行が正解、列が予測です。

### float32

| 正解 ＼ 予測 | OTHER | SNORE | COUGH |
|---|---:|---:|---:|
| OTHER | 204 | 15 | 9 |
| SNORE | 11 | 151 | 4 |
| COUGH | 17 | 2 | 749 |

### INT8

| 正解 ＼ 予測 | OTHER | SNORE | COUGH |
|---|---:|---:|---:|
| OTHER | 206 | 13 | 9 |
| SNORE | 13 | 149 | 4 |
| COUGH | 18 | 2 | 748 |

この評価は、切り出し済み音声に対する分類性能です。クラス数に偏りがあり、OTHERはESC-50、SNOREは主にAudioSet Strong、COUGHは主にCoswaraに由来します。音の種類と収録条件の違いが重なっている点も、数値を読む際の前提です。

## 組込みインタフェース

上位処理は [ai_pipeline.h](../Core/Inc/ai_pipeline.h) の `ai_pipeline_init()` と `ai_pipeline_predict()` を使用します。前処理と推論をまとめた窓口で、入力PCMの所有権は呼び出し側にあります。

出力はクラスID、3クラス分のINT8値、選択クラスの `confidence_q8` です。`confidence_q8 = raw_output[class_id] + 128` として0〜255へ変換し、CSVへ保存します。おおよそのSoftmax出力は `confidence_q8 / 256` ですが、個々の判定が正しい確率を保証する値ではありません。

## 評価結果の参照先

- [float32クラス別指標](../AI/dataset/metadata/07_eval_info/classification_metrics.csv)
- [float32混同行列](../AI/dataset/metadata/07_eval_info/confusion_matrix.csv)
- [INT8クラス別指標](../AI/dataset/metadata/08_quant_info/int8_classification_metrics.csv)
- [INT8混同行列](../AI/dataset/metadata/08_quant_info/int8_confusion_matrix.csv)
- [量子化比較指標](../AI/dataset/metadata/08_quant_info/quantization_metrics.csv)

これらの数値表と上記の説明により、開発用の作業記録を参照せずに評価条件と結果を確認できます。
