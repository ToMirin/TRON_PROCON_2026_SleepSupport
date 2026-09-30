#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "main.h"

/* 入力の取り込みはTIM8が行う。この周期は表示の更新間隔。 */
#define ENC_TEST_POLL_MS 10
#define ENC_TEST_COUNTS_PER_STEP 4

extern TIM_HandleTypeDef htim8;

/* 16ビットの周回を補正する。読み取り間の移動量は32768未満が前提。 */
static INT enc_test_delta(UINT current, UINT previous)
{
    UINT diff = (current - previous) & 0xffffU;
    return (diff < 0x8000U) ? (INT)diff : (INT)diff - 65536;
}

static void enc_test_task(INT stacd, void *exinf)
{
    UINT previous = 0U; /* 起動前にCNTを0にする。 */
    INT progress = 0;
    (void)stacd;
    (void)exinf;

    while (1) {
        UINT current = __HAL_TIM_GET_COUNTER(&htim8);
        progress += enc_test_delta(current, previous);
        previous = current;

        /* 途中の逆戻りは相殺。4エッジを1操作とし、端数を保持する。 */
        // A/Bそのものではなく、TIM8の現在値を読み、前回値との差分を加算する。TIM8は4エッジで1カウントする。
        while (progress >= ENC_TEST_COUNTS_PER_STEP ||
               progress <= -ENC_TEST_COUNTS_PER_STEP) {
            INT direction = (progress > 0) ? +1 : -1;
            progress -= direction * ENC_TEST_COUNTS_PER_STEP;
            HAL_GPIO_TogglePin(DBG_LED1_GPIO_Port, DBG_LED1_Pin);//LED1を点滅させる
            tm_printf((UB *)"ENC: %d\n", (int)direction);
        }
        tk_dly_tsk(ENC_TEST_POLL_MS);
    }
}

/* app_main.cから一度だけ呼ぶ。MX_TIM8_Initは完了済み。 */
static ER enc_test_start(void)
{
    T_CTSK ctsk = {
        .tskatr = TA_HLNG | TA_RNG3,
        .task = enc_test_task,
        .itskpri = 10,
        .stksz = 1024
    };
    ID enc_task_id = tk_cre_tsk(&ctsk);
    if (enc_task_id < E_OK) return (ER)enc_task_id;

    HAL_GPIO_WritePin(DBG_LED1_GPIO_Port, DBG_LED1_Pin, GPIO_PIN_RESET);
    __HAL_TIM_SET_COUNTER(&htim8, 0U);
    if (HAL_TIM_Encoder_Start(&htim8, TIM_CHANNEL_ALL) != HAL_OK) {
        (void)HAL_TIM_Encoder_Stop(&htim8, TIM_CHANNEL_ALL);
        (void)tk_del_tsk(enc_task_id);
        return E_SYS;
    }

    ER err = tk_sta_tsk(enc_task_id, 0);
    if (err < E_OK) {
        (void)HAL_TIM_Encoder_Stop(&htim8, TIM_CHANNEL_ALL);
        (void)tk_del_tsk(enc_task_id);
        return err;
    }
    return E_OK;
}

/*
 * 学習用：TIM8移行前のGPIOポーリング実装。
 * GPIOを一定間隔で読み、3回連続一致したABを遷移表で判定していた。
 * 上の現行実装では、入力の取り込みと方向判定をTIM8に任せている。
 * 以下は全行コメントのため、ビルド・実行には影響しない。
 */
// #include <tk/tkernel.h>
// #include <tm/tmonitor.h>
// #include "main.h"
//
// #define ENC_TEST_POLL_MS       1
// #define ENC_TEST_STABLE_READS  3U
//
//
// /*
//  * 確定したABを渡す。
//  * 戻り値: 一巡したら1、それ以外は0
//  * direction: +1=正方向、-1=逆方向、0=変化なし
//  */
// static INT enc_test_update(UINT ab)
// {
//     static UINT prev_ab = 0b100;// 初期値は!=ab
//     static INT progress = 0;    // 0=基準位置、+1=正方向1/4、+2=正方向1/2、+3=正方向3/4、+4=一巡
//     static UINT synced = 0U;    // ab == 0b11 かどうか
//     INT direction = 0;
//
//     // 正方向: 11→10→00→01→11 / 逆方向: 11→01→00→10→11
//     // { prev_ab, ab } の組み合わせで進行方向を判定
//     static const INT delta[4][4] = {
//         { 0, +1, -1,  0 },  /* 00 */
//         {-1,  0,  0, +1 },  /* 01 */
//         {+1,  0,  0, -1 },  /* 10 */
//         { 0, -1, +1,  0 }   /* 11 */
//     };
//
//     // 起動時と不正遷移後は、11を待つ
//     if (!synced) {
//         prev_ab = ab;
//         progress = 0;
//         synced = (ab == 0b11);
//         return 0;
//     }
//
//     if (ab == prev_ab) return 0;
//
//     //2ビット同時変化は破棄し、再同期
//     if ((prev_ab ^ ab) == 0b11) {
//         prev_ab = ab;
//         progress = 0;
//         synced = (ab == 0b11);
//         return 0;
//     }
//
//     progress += delta[prev_ab][ab];
//     prev_ab = ab;
//
//     // 11以外は破棄
//     if (ab != 0b11) return 0;
//
//     if (progress == +4)      direction = +1;
//     else if (progress == -4) direction = -1;
//
//     progress = 0;
//     return direction;
// }
//
// static void enc_test_task(INT stacd, void *exinf)
// {
//     static UINT candidate = 0U;
//     UINT match_count = 0U;
//
//     (void)stacd;
//     (void)exinf;
//
//     while (1) {
//         UINT a = (HAL_GPIO_ReadPin(ENC_A_GPIO_Port, ENC_A_Pin)
//                   == GPIO_PIN_SET);
//         UINT b = (HAL_GPIO_ReadPin(ENC_B_GPIO_Port, ENC_B_Pin)
//                   == GPIO_PIN_SET);
//         UINT ab = (a << 1) | b;
//
//
//         if (ab != candidate) {
//             candidate = ab;
//             match_count = 1U;
//         } else {
//             match_count++;
//         }
//
//         // 安定したABを3回連続で読めたら確定
//         if (match_count == ENC_TEST_STABLE_READS) {
//             match_count = 0U;
//
//             INT direction = enc_test_update(candidate);
//
//             INT val = 0;
//             if (direction != 0){
//                 if (direction > 0)      val = +1;
//                 else if (direction < 0) val = -1;
//                 HAL_GPIO_TogglePin(DBG_LED1_GPIO_Port, DBG_LED1_Pin);
//                 tm_printf((UB *)"ENC: %d\n", val);
//             }
//
//         }
//
//         tk_dly_tsk(ENC_TEST_POLL_MS);
//     }
// }
//
// static ID enc_task_id; // エンコーダータスクのID番号
// static ER enc_test_start()
// {
//     T_CTSK ctsk = {
//         .tskatr = TA_HLNG | TA_RNG3,
//         .task = enc_test_task,
//         .itskpri = 10, //! タスク優先度：10
//         .stksz = 1024
//     };
//     ER err;
//
//     enc_task_id = tk_cre_tsk(&ctsk);
//     if (enc_task_id < E_OK) {
//         return (ER)enc_task_id;
//     }
//
//     err = tk_sta_tsk(enc_task_id, 0);
//     if (err < E_OK) {
//         /* 起動できなかったタスクを削除する。 */
//         (void)tk_del_tsk(enc_task_id);
//         return err;
//     }
//
//     return E_OK;
// }
