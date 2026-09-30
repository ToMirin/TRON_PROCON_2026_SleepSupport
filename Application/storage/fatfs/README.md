# FatFs R0.15

Official source: https://elm-chan.org/fsw/ff/arc/ff15.zip
Archive SHA-256: e0d76654d877e6c74be5ea3c395808794d495169514e98cbf6046168b8f4f070

Vendored source/ff.c, source/ff.h, source/diskio.h, source/ffconf.h, source/ffunicode.c.
Original copyright and redistribution terms are retained in the source headers.

Configuration: write enabled; FAT12/16/32 and exFAT; one 512-byte-sector volume;
LFN enabled with static working buffers (FF_USE_LFN=1); code page 437; tiny shared sector buffer; fixed file timestamp;
no mkfs or concurrent access. Only input_task calls the filesystem.
The SPI disk I/O implementation is ../sd_spi.c.
