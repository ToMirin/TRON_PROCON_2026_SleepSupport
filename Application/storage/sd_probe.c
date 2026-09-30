#include "sd_probe.h"
#include "fatfs/ff.h"
#include "fatfs/diskio.h"
#include <tm/tmonitor.h>
#include <string.h>

/* input_taskだけが使用。FatFsの作業領域をタスクスタックへ置かない。 */
static FATFS filesystem;
static FIL file;

/* 認識失敗時だけ管理セクタを読む。カードへの書き込みは行わない。 */
static BYTE diagnostic_sector[512];
static BYTE diagnostic_again[512];
static DWORD read_le32(const BYTE *p)
{
    return (DWORD)p[0] | ((DWORD)p[1] << 8) |
           ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
}

static void dump_boot(DWORD sector)
{
    DRESULT r = disk_read(0, diagnostic_again, sector, 1);
    tm_printf((UB *)"SD diag: LBA=%lu read=%d\n", (unsigned long)sector, (int)r);
    if (r != RES_OK) return;
    tm_printf((UB *)"SD diag: same_as_MBR=%d signature=%02x%02x first64=\n",
              memcmp(diagnostic_sector, diagnostic_again, 512) == 0,
              (unsigned)diagnostic_again[510], (unsigned)diagnostic_again[511]);
    for (unsigned i = 0; i < 64; ++i) {
        tm_printf((UB *)"%02x ", (unsigned)diagnostic_again[i]);
        if (i % 16 == 15) tm_putstring((UB *)"\n");
    }
}

static void diagnose_mount(void)
{
    DRESULT result = disk_read(0, diagnostic_sector, 0, 1);
    tm_printf((UB *)"SD diag: sector0 read=%d\n", (int)result);
    if (result != RES_OK) return;
    result = disk_read(0, diagnostic_again, 0, 1);
    tm_printf((UB *)"SD diag: reread=%d identical=%d signature=%02x%02x\n",
              (int)result, result == RES_OK &&
              memcmp(diagnostic_sector, diagnostic_again, 512) == 0,
              (unsigned)diagnostic_sector[510], (unsigned)diagnostic_sector[511]);
    tm_putstring((UB *)"SD diag: sector0 first16=");
    for (unsigned i = 0; i < 16; ++i)
        tm_printf((UB *)"%02x ", (unsigned)diagnostic_sector[i]);
    tm_putstring((UB *)"\n");
    /* EXFATのブートセクタならここはパーティション表ではない。 */
    if (memcmp(diagnostic_sector + 3, "EXFAT   ", 8) == 0) {
        tm_putstring((UB *)"SD diag: exFAT boot sector at LBA0\n");
        return;
    }
    for (unsigned i = 0; i < 4; ++i) {
        const BYTE *entry = diagnostic_sector + 446 + i * 16;
        DWORD first = read_le32(entry + 8);
        tm_printf((UB *)"SD diag: partition%u type=%02x start=%lu\n",
                  i + 1, (unsigned)entry[4], (unsigned long)first);
        if (entry[4] == 0xee) {
            tm_putstring((UB *)"SD diag: GPT detected (requires FF_LBA64=1)\n");
        } else if (entry[4] == 0x07 && first != 0) {
            result = disk_read(0, diagnostic_again, first, 1);
            tm_printf((UB *)"SD diag: partition boot read=%d exFAT=%d\n",
                      (int)result, result == RES_OK &&
                      memcmp(diagnostic_again + 3, "EXFAT   ", 8) == 0);
            dump_boot(first);
            if (first <= 0xfffffff3UL) dump_boot(first + 12U); /* exFAT backup boot */
        }
    }
}

ER storage_sd_probe(void)
{
    static const char text[] = "stop monitoring\r\n";
    char readback[sizeof(text)];
    UINT count;
    const char *stage = "initialize";
    FRESULT result = FR_NOT_READY;
    int opened = 0;
    tm_printf((UB *)"SD probe v5: exFAT=%d LFN=%d LBA64=%d\n",
              FF_FS_EXFAT, FF_USE_LFN, FF_LBA64);
    if (disk_initialize(0) != 0) goto done;
    stage = "mount";
    result = f_mount(&filesystem, "0:", 1);
    if (result != FR_OK) {
        diagnose_mount();
        goto done;
    }
    stage = "open";
    result = f_open(&file, "0:/test.txt", FA_CREATE_ALWAYS | FA_WRITE);
    if (result != FR_OK) goto done;
    opened = 1;
    stage = "write";
    result = f_write(&file, text, sizeof(text) - 1U, &count);
    if (result == FR_OK && count != sizeof(text) - 1U) result = FR_DISK_ERR;
    if (result != FR_OK) goto done;
    stage = "close";
    result = f_close(&file); /* データとディレクトリエントリを同期 */
    opened = 0;
    if (result != FR_OK) goto done;
    /* 再マウントし、キャッシュだけでなくカードから読み戻す。 */
    f_mount(NULL, "0:", 0);
    stage = "remount";
    result = f_mount(&filesystem, "0:", 1);
    if (result != FR_OK) goto done;
    stage = "verify open";
    result = f_open(&file, "0:/test.txt", FA_READ);
    if (result != FR_OK) goto done;
    opened = 1;
    stage = "verify read";
    result = f_read(&file, readback, sizeof(readback), &count);
    if (result == FR_OK && (count != sizeof(text) - 1U ||
        memcmp(readback, text, sizeof(text) - 1U) != 0)) result = FR_DISK_ERR;
done:
    if (opened) {
        FRESULT close_result = f_close(&file);
        if (result == FR_OK) { result = close_result; stage = "verify close"; }
    }
    f_mount(NULL, "0:", 0);
    if (result != FR_OK) {
        tm_printf((UB *)"SD test: %s failed (FatFs=%d)\n", stage, (int)result);
        return E_IO;
    }
    tm_putstring((UB *)"SD test: test.txt written and verified.\n");
    return E_OK;
}
