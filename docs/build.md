# ビルドと書き込み

[READMEに戻る](../README.md)

## 準備する環境

- NUCLEO-H533REとUSBデータケーブル
- STM32CubeIDE（STM32H5とボード搭載ST-LINKを扱える環境）
- 本リポジトリ一式
- [ハードウェア資料](hardware.md)の配線済み装置

本リポジトリにはCubeIDEプロジェクトの `.project`・`.cproject` と、CubeMX設定 `mtk3bsp2_stm32h533.ioc` が含まれます。プロジェクト名は `mtk3bsp2_stm32h533` です。

現在のプロジェクト設定はSTM32Cube FW_H5 V1.6.0を基準にしています。通常のビルドには同梱ソースと生成済みAIコードを使い、AIの再学習やモデル変換を最初から行う必要はありません。

## 1. 既存プロジェクトを取り込む

リポジトリを取得し、STM32CubeIDEの既存プロジェクトのインポート機能で、ルートの `.project` を含むフォルダを選択します。新しい空のSTM32プロジェクトを作成するのではなく、付属のビルド設定を読み込みます。

## 2. 通常動作の設定を確認する

[Application/sleep_app_config.h](../Application/sleep_app_config.h)で、次の設定を使用します。

```c
#define SLEEP_APP_SANDBOX 0
#define SLEEP_LCD_HEADLESS 0
```

`SLEEP_APP_SANDBOX=0` は通常システム、`SLEEP_LCD_HEADLESS=0` は実LCDを使用する設定です。既定値はこの組み合わせです。LCDなしで開発確認する場合だけ `SLEEP_LCD_HEADLESS=1` を使い、画面状態をシリアル出力で確認できます。

## 3. Debug構成でビルドする

ビルド構成は **Debug** を選択します。現在の `.cproject` では、この構成にアプリケーション、AI生成コード、μT-Kernel、AIランタイムの設定が含まれます。Release構成は同じ統合設定になっていないため、この手順では使用しません。

プロジェクトを更新し、Clean後にBuildを実行します。主な同梱資材は以下です。

| 資材 | 場所 |
|---|---|
| 通常アプリケーション | `Application/` |
| AI前処理と呼び出し窓口 | `Core/Src/ai_preprocess.c`・`ai_pipeline.c`・`ai_classifier.c` |
| 生成済みモデルコード | `AI_Model/` |
| AI用生成パラメータ | `AI/generated/` |
| ST Edge AIランタイム | `Middlewares/ST/AI/Lib/NetworkRuntime1201_CM33_GCC.a` |
| RTOS・BSP | `mtk3_bsp2/` |
| リンカ設定 | `STM32H533RETX_FLASH.ld` |

ビルド成果物は `Debug/` 以下に生成されます。ライブラリが見つからない場合は、上記のランタイムの存在とDebug構成のライブラリ検索先を確認してください。

## 4. ボードへ書き込む

ボード搭載ST-LINK側のUSB端子をPCへ接続します。CubeIDEで対象を `mtk3bsp2_stm32h533`、デバッグ接続をST-LINK／SWDとしてRunまたはDebug設定を作成し、ビルド済みプログラムを書き込みます。Debugで停止した場合は実行を再開します。

LCDの起動画面から時刻設定へ進むことを確認し、[利用ガイド](usage.md)に沿って設定・開始・停止・保存を行ってください。

診断ログを確認する場合はST-LINKの仮想COMポートを開き、115200 bps、8 data bits、パリティなし、1 stop bit、フロー制御なしに設定します。

## 周辺機能を再生成する場合

通常のビルドのためにCubeMXで再生成する必要はありません。設定を変更して再生成する場合は、生成結果と既存ソースの差分を確認してください。

特にCPU 250 MHz、I2S2の16 kHz・24 bit・右詰め、受信用DMA、SPI1のPLL3設定、アプリケーションのビルド対象が維持されている必要があります。SPI1用PLL3の入力レンジは、M分周後8 MHzに対応する `RCC_PLL3_VCIRANGE_3` を使用しています。

## 学習・量子化を再実行する場合

ファームウェアのビルドとは別の工程です。`AI/requirements.txt` と `AI/environment-freeze.txt` を参照してPython環境を準備し、[学習データセット](datasets.md)に従って音声を用意します。

主なNotebookは `06_train_baseline.ipynb`、`07_eval_model.ipynb`、`08_quant_model.ipynb`、`09_integration_check.ipynb` です。モデルを変更した場合は、正規化・量子化パラメータとST Edge AI生成コードも対応するものへ更新します。
