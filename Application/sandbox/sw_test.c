/*
 * スイッチ・LED動作確認用タスク
 *
 * app_main.cから、SANDBOXとSW_TESTが有効な場合だけ取り込む。
 * このファイル自体は、独立したビルド対象にしない。
 *
 * 対応：
 *   SW1_USER (PC13) → DBG_LED1 (PC10)
 *   SW2      (PC2)  → DBG_LED2 (PC11)
 *   SW3      (PC3)  → DBG_LED3 (PC12)
 *
 * スイッチを押している間は点灯し、離すと消灯する。
 * LEDはGPIOがHighで点灯する配線を前提とする。
 */

// #if defined(SANDBOX) && defined(SW_TEST)

#include <tk/tkernel.h>
#include "main.h"

/* 入力の読み取り間隔と、状態確定に必要な連続一致回数。 */
#define SW_TEST_POLL_MS       10
#define SW_TEST_STABLE_READS  3U

#define NUM_OF_SW 3 // スイッチの数


/* スイッチとLEDの接続、およびスイッチの押下極性。 */
typedef struct {
    GPIO_TypeDef *sw_port; // GPIO周辺レジスタを表すポートへのポインタ
    uint16_t sw_pin;
    GPIO_PinState pressed_level;

    GPIO_TypeDef *led_port;
    uint16_t led_pin;
} SwTestIo_t;

/* チャタリング除去用の状態。スイッチごとに独立して保持する。 */
typedef struct {
    UINT candidate;      /* 現在、確定待ちの押下状態 */
    UINT stable;         /* 確定済みの押下状態 */
    UINT match_count;    /* candidateが連続して観測された回数 */
} SwTestState_t;


static ID sw_task_id; // スイッチ・LEDタスクのID番号
/* 各スイッチで状態を保持する。 */ 
static const SwTestIo_t sw_test_io[] = {
    {
        SW1_USER_GPIO_Port, SW1_USER_Pin, GPIO_PIN_SET,
        DBG_LED1_GPIO_Port, DBG_LED1_Pin
    },
    {
        SW2_GPIO_Port, SW2_Pin, GPIO_PIN_RESET,
        DBG_LED2_GPIO_Port, DBG_LED2_Pin
    },
    {
        SW3_GPIO_Port, SW3_Pin, GPIO_PIN_RESET,
        DBG_LED3_GPIO_Port, DBG_LED3_Pin
    }
};



static void sw_test_task(INT stacd, void *exinf)
{
    SwTestState_t state[NUM_OF_SW] = {0};

    /* このテストではタスク起動引数を使用しない。 */
    (void)stacd;
    (void)exinf;

    while (1) {
        for (UINT i = 0; i < NUM_OF_SW; ++i) {
            const SwTestIo_t *io = &sw_test_io[i];
            SwTestState_t *s = &state[i];

            /*
             * ボード搭載SWはHighで押下、外付けSWはLowで押下。
             * ここで「押下=1、非押下=0」に統一する。
             */
            //! HAL_GPIO_ReadPin: PinがSETなら1、RESETなら0を返す
            UINT pressed =
                (HAL_GPIO_ReadPin(io->sw_port, io->sw_pin)
                 == io->pressed_level);
        
            if (pressed != s->candidate) {
                /* 入力が変わったので、連続一致回数を数え直す。 */
                s->candidate = pressed;
                s->match_count = 1U;
            } else if (s->match_count < SW_TEST_STABLE_READS) {
                ++s->match_count;
            }

            /*
             * 同じ状態を3回連続で読めたら状態を確定する。
             * 押したときと離したときの両方に適用する。
             */
            if ((s->match_count == SW_TEST_STABLE_READS) &&
                (s->stable != s->candidate)) {
                s->stable = s->candidate;

                //! HAL_GPIO_WritePin: PinがSETなら1、RESETなら0を出力する
                HAL_GPIO_WritePin(
                    io->led_port,
                    io->led_pin,
                    s->stable ? GPIO_PIN_SET : GPIO_PIN_RESET
                );
            }
        }

        /* CPUを占有せず、他のタスクへ実行機会を渡す。 */
        tk_dly_tsk(SW_TEST_POLL_MS);
    }
}

/*
 * usermainから一度だけ呼び出す。
 * GPIOの初期化は、CubeMX生成コードで完了している前提。
 */
static ER sw_test_start(void)
{
    T_CTSK ctsk = {
        .tskatr = TA_HLNG | TA_RNG3,
        .task = sw_test_task,
        .itskpri = 10, //! タスク優先度：10
        .stksz = 1024
    };
    ER err;

    /* 起動時は3個とも消灯する。 */
    for (UINT i = 0; i < NUM_OF_SW; ++i) {
        HAL_GPIO_WritePin(
            sw_test_io[i].led_port,
            sw_test_io[i].led_pin,
            GPIO_PIN_RESET
        );
    }

    sw_task_id = tk_cre_tsk(&ctsk);
    if (sw_task_id < E_OK) {
        return (ER)sw_task_id;
    }

    err = tk_sta_tsk(sw_task_id, 0);
    if (err < E_OK) {
        /* 起動できなかったタスクを削除する。 */
        (void)tk_del_tsk(sw_task_id);
        return err;
    }

    return E_OK;
}

// #endif /* SANDBOX && SW_TEST */