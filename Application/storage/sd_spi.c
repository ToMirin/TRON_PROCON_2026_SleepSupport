/* SD v2 SPI block device. Single caller: input_task after audio stops. */
#include "main.h"
#include "fatfs/ff.h"
#include "fatfs/diskio.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <string.h>

extern SPI_HandleTypeDef hspi1;
static DSTATUS status = STA_NOINIT;
static int block_addressed, transfer_failed;

static int transfer(const BYTE *tx, BYTE *rx, UINT count)
{
    if (transfer_failed) return 0;
    if (HAL_SPI_TransmitReceive(&hspi1, (BYTE *)tx, rx, (uint16_t)count, 1000) != HAL_OK) {
        transfer_failed = 1;
        status = STA_NOINIT;
        return 0;
    }
    return 1;
}

static BYTE exchange(BYTE value)
{
    BYTE result = 0xff;
    transfer(&value, &result, 1);
    return result;
}

static void deselect(void)
{
    HAL_GPIO_WritePin(SD_CS_GPIO_Port, SD_CS_Pin, GPIO_PIN_SET);
    exchange(0xff); /* カードがMISOを解放するためのクロック */
}

static int ready(uint32_t timeout)
{
    uint32_t start = HAL_GetTick();
    do {
        if (exchange(0xff) == 0xff && !transfer_failed) return 1;
        if (transfer_failed) break;
        tk_dly_tsk(1);
    } while ((uint32_t)(HAL_GetTick() - start) < timeout);
    return 0;
}

static BYTE command(BYTE cmd, DWORD arg)
{
    deselect();
    HAL_GPIO_WritePin(SD_CS_GPIO_Port, SD_CS_Pin, GPIO_PIN_RESET);
    if (cmd != 0U && !ready(500)) return 0xff;
    exchange((BYTE)(0x40 | cmd));
    exchange((BYTE)(arg >> 24)); exchange((BYTE)(arg >> 16));
    exchange((BYTE)(arg >> 8)); exchange((BYTE)arg);
    exchange(cmd == 0 ? 0x95 : cmd == 8 ? 0x87 : 0x01);
    for (unsigned i = 0; i < 10; ++i) {
        BYTE response = exchange(0xff);
        if (!(response & 0x80)) return transfer_failed ? 0xff : response;
    }
    return 0xff;
}

static int read_data(BYTE *buffer, UINT length)
{
    uint32_t start = HAL_GetTick();
    BYTE token;
    do {
        token = exchange(0xff);
        if (token != 0xff || transfer_failed) break;
        tk_dly_tsk(1);
    } while ((uint32_t)(HAL_GetTick() - start) < 500);
    if (token != 0xfe || transfer_failed) return 0;
    BYTE dummy[32];
    memset(dummy, 0xff, sizeof(dummy));
    while (length) {
        UINT n = length > sizeof(dummy) ? sizeof(dummy) : length;
        if (!transfer(dummy, buffer, n)) return 0;
        buffer += n; length -= n;
    }
    exchange(0xff); exchange(0xff); /* CRC: SPI CRC checking disabled */
    return !transfer_failed;
}

DSTATUS disk_initialize(BYTE drive)
{
    if (drive != 0) return STA_NOINIT;
    status = STA_NOINIT;
    transfer_failed = 0;
    block_addressed = 0;
    HAL_GPIO_WritePin(SD_CS_GPIO_Port, SD_CS_Pin, GPIO_PIN_SET);
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_256;
    /* HALは各転送の終わりにSPIを無効化する。CSがLowのままでも
     * SCK/MOSIを保持し、端子の解放による余分なクロックを防ぐ。
     * CubeMX再生成後もSD初期化時に必ず適用する。 */
    hspi1.Init.MasterKeepIOState = SPI_MASTER_KEEP_IO_STATE_ENABLE;
    hspi1.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
    if (HAL_SPI_Init(&hspi1) != HAL_OK) return status;
    /*
     * Some cards do not answer the first CMD0 after the MCU or SPI peripheral
     * has been reset while the card remains powered.  The failed attempt still
     * supplies clocks and leaves the card ready for the next attempt, which
     * previously made a second save operation succeed.  Complete that recovery
     * here instead of requiring the user to retry the save.
     */
    tk_dly_tsk(100);
    for (unsigned i = 0; i < 20; ++i) exchange(0xff); /* 160 clocks, CS high */
    uint32_t cmd0_start = HAL_GetTick();
    BYTE response = 0xff;
    unsigned cmd0_attempts = 0;
    do {
        ++cmd0_attempts;
        response = command(0, 0);
        if (response == 1 || transfer_failed) break;
        deselect();
        tk_dly_tsk(10);
    } while ((uint32_t)(HAL_GetTick() - cmd0_start) < 1000U);
    if (response != 1) {
        tm_printf((UB *)"SD init: CMD0 failed response=%02x attempts=%u\n",
                  (unsigned)response, cmd0_attempts);
        goto done;
    }
    if (cmd0_attempts > 1U)
        tm_printf((UB *)"SD init: CMD0 recovered after %u attempts\n",
                  cmd0_attempts);
    /* この初期確認ではSD v2（SDHCを含む）のみ対応。 */
    if (command(8, 0x1aa) != 1) goto done;
    BYTE r7[4];
    for (unsigned i = 0; i < 4; ++i) r7[i] = exchange(0xff);
    if (r7[2] != 1 || r7[3] != 0xaa) goto done;
    uint32_t start = HAL_GetTick();
    do {
        if (command(55, 0) > 1) goto done;
        response = command(41, 0x40000000);
        if (response == 0) break;
        if (response != 1 || transfer_failed) goto done;
        deselect();
        tk_dly_tsk(10);
    } while ((uint32_t)(HAL_GetTick() - start) < 2000);
    if (response != 0 || command(58, 0) != 0) goto done;
    BYTE ocr[4];
    for (unsigned i = 0; i < 4; ++i) ocr[i] = exchange(0xff);
    if (!(ocr[0] & 0x80) || !(ocr[1] & 0x30)) goto done;
    block_addressed = (ocr[0] & 0x40) != 0;
    tm_printf((UB *)"SD init: OCR=%02x %02x %02x %02x block_addressed=%d\n",
              (unsigned)ocr[0], (unsigned)ocr[1], (unsigned)ocr[2],
              (unsigned)ocr[3], block_addressed);
    if (!block_addressed && command(16, 512) != 0) goto done;
    if (!transfer_failed) status = 0;
done:
    deselect();
    /* 接続確認は250 kHzのまま。高速化は実機で読み書きできてから行う。 */
    return status;
}

DSTATUS disk_status(BYTE drive) { return drive == 0 ? status : STA_NOINIT; }

DRESULT disk_read(BYTE drive, BYTE *buffer, LBA_t sector, UINT count)
{
    if (drive || !buffer || !count) return RES_PARERR;
    if (status) return RES_NOTRDY;
    for (UINT i = 0; i < count; ++i) {
        if (!block_addressed && sector > 0x7fffffU) return RES_PARERR;
        if (command(17, block_addressed ? sector : sector * 512U) != 0 ||
            !read_data(buffer, 512)) {
            deselect(); status = STA_NOINIT; return RES_ERROR;
        }
        deselect();
        if (transfer_failed) return RES_ERROR;
        buffer += 512; ++sector;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE drive, const BYTE *buffer, LBA_t sector, UINT count)
{
    if (drive || !buffer || !count) return RES_PARERR;
    if (status) return RES_NOTRDY;
    for (UINT i = 0; i < count; ++i) {
        if (!block_addressed && sector > 0x7fffffU) return RES_PARERR;
        if (command(24, block_addressed ? sector : sector * 512U) != 0) goto failed;
        exchange(0xff); exchange(0xfe);
        BYTE discard[32];
        for (UINT offset = 0; offset < 512; offset += sizeof(discard))
            if (!transfer(buffer + offset, discard, sizeof(discard))) goto failed;
        exchange(0xff); exchange(0xff);
        if ((exchange(0xff) & 0x1f) != 0x05 || !ready(2000)) goto failed;
        /* CMD13 R2 reports errors that can follow an accepted data response. */
        if (command(13, 0) != 0 || exchange(0xff) != 0 || transfer_failed) goto failed;
        deselect();
        if (transfer_failed) return RES_ERROR;
        buffer += 512; ++sector;
    }
    return RES_OK;
failed:
    deselect(); status = STA_NOINIT; return RES_ERROR;
}

DRESULT disk_ioctl(BYTE drive, BYTE cmd, void *buffer)
{
    (void)buffer;
    if (drive) return RES_PARERR;
    if (status) return RES_NOTRDY;
    if (cmd != CTRL_SYNC) return RES_PARERR; /* mkfs/trim are disabled */
    HAL_GPIO_WritePin(SD_CS_GPIO_Port, SD_CS_Pin, GPIO_PIN_RESET);
    int ok = ready(2000);
    deselect();
    if (!ok || transfer_failed) { status = STA_NOINIT; return RES_ERROR; }
    return RES_OK;
}
