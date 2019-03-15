
#include "uf2.h"

#include "nrf_log.h"
#include "nrf_nvmc.h"
#include "nrf_sdh.h"
#include "nrf_dfu_settings.h"

#include <string.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) sizeof(arr) / sizeof(arr[0])
#endif

typedef struct {
    uint8_t JumpInstruction[3];
    uint8_t OEMInfo[8];
    uint16_t SectorSize;
    uint8_t SectorsPerCluster;
    uint16_t ReservedSectors;
    uint8_t FATCopies;
    uint16_t RootDirectoryEntries;
    uint16_t TotalSectors16;
    uint8_t MediaDescriptor;
    uint16_t SectorsPerFAT;
    uint16_t SectorsPerTrack;
    uint16_t Heads;
    uint32_t HiddenSectors;
    uint32_t TotalSectors32;
    uint8_t PhysicalDriveNum;
    uint8_t Reserved;
    uint8_t ExtendedBootSig;
    uint32_t VolumeSerialNumber;
    uint8_t VolumeLabel[11];
    uint8_t FilesystemIdentifier[8];
} __attribute__((packed)) FAT_BootBlock;

typedef struct {
    char name[8];
    char ext[3];
    uint8_t attrs;
    uint8_t reserved;
    uint8_t createTimeFine;
    uint16_t createTime;
    uint16_t createDate;
    uint16_t lastAccessDate;
    uint16_t highStartCluster;
    uint16_t updateTime;
    uint16_t updateDate;
    uint16_t startCluster;
    uint32_t size;
} __attribute__((packed)) DirEntry;

STATIC_ASSERT(sizeof(DirEntry) == 32);

struct TextFile {
    const char name[11];
    const char *content;
};

#define NUM_FAT_BLOCKS UF2_NUM_BLOCKS

#define STR0(x) #x
#define STR(x) STR0(x)
const char infoUf2File[] = //
    "UF2 Bootloader " UF2_VERSION "\r\n"
    "Model: " PRODUCT_NAME "\r\n"
    "Board-ID: " BOARD_ID "\r\n";

const char indexFile[] = //
    "<!doctype html>\n"
    "<html>"
    "<body>"
    "<script>\n"
    "location.replace(\"" INDEX_URL "\");\n"
    "</script>"
    "</body>"
    "</html>\n";

static const struct TextFile info[] = {
    {.name = "INFO_UF2TXT", .content = infoUf2File},
    {.name = "INDEX   HTM", .content = indexFile},
    {.name = "CURRENT UF2"},
};
// WARNING -- code presumes each non-UF2 file content fits in single sector
//            Cannot programmatically statically assert .content length
//            for each element above.
STATIC_ASSERT(ARRAY_SIZE(infoUf2File) < 512);
STATIC_ASSERT(ARRAY_SIZE(indexFile) < 512);
#define NUM_FILES (ARRAY_SIZE(info))
#define NUM_DIRENTRIES (NUM_FILES + 1) // Code adds volume label as first root directory entry

#define UF2_SIZE (FLASH_SIZE * 2)
#define UF2_SECTORS (UF2_SIZE / 512)
#define UF2_FIRST_SECTOR (NUM_FILES + 1) // WARNING -- code presumes each non-UF2 file content fits in a single sector
#define UF2_LAST_SECTOR (UF2_FIRST_SECTOR + UF2_SECTORS - 1)

#define RESERVED_SECTORS 1
#define ROOT_DIR_SECTORS 4
#define SECTORS_PER_FAT ((NUM_FAT_BLOCKS * 2 + 511) / 512)

#define START_FAT0 RESERVED_SECTORS
#define START_FAT1 (START_FAT0 + SECTORS_PER_FAT)
#define START_ROOTDIR (START_FAT1 + SECTORS_PER_FAT)
#define START_CLUSTERS (START_ROOTDIR + ROOT_DIR_SECTORS)

// all directory entries must fit in a single sector
// because otherwise current code overflows buffer
#define DIRENTRIES_PER_SECTOR (512/sizeof(DirEntry))
STATIC_ASSERT(NUM_DIRENTRIES < DIRENTRIES_PER_SECTOR );

static const FAT_BootBlock BootBlock = {
    .JumpInstruction = {0xeb, 0x3c, 0x90},
    .OEMInfo = "UF2 UF2 ",
    .SectorSize = 512,
    .SectorsPerCluster = 1,
    .ReservedSectors = RESERVED_SECTORS,
    .FATCopies = 2,
    .RootDirectoryEntries = (ROOT_DIR_SECTORS * 512 / DIRENTRIES_PER_SECTOR),
    .TotalSectors16 = NUM_FAT_BLOCKS - 2,
    .MediaDescriptor = 0xF8,
    .SectorsPerFAT = SECTORS_PER_FAT,
    .SectorsPerTrack = 1,
    .Heads = 1,
    .PhysicalDriveNum = 0x80, // to match MediaDescriptor of 0xF8
    .ExtendedBootSig = 0x29,
    .VolumeSerialNumber = 0x00420042,
    .VolumeLabel = VOLUME_LABEL,
    .FilesystemIdentifier = "FAT16   ",
};

#define NO_CACHE 0xffffffff

uint32_t flashAddr = NO_CACHE;
uint8_t flashBuf[FLASH_PAGE_SIZE] __attribute__((aligned(4)));
bool firstFlush = true;
bool hadWrite = false;

void flushFlash() {
    if (flashAddr == NO_CACHE)
        return;

    if (firstFlush) {
        if (sdRunning) {
            // disable SD - we need sync access to flash, and we might be also overwriting the SD
            nrf_sdh_disable_request();
            nrf_dfu_settings_init(false);
        }

        firstFlush = false;

        s_dfu_settings.write_offset = 0;
        s_dfu_settings.sd_size = 0;
        s_dfu_settings.bank_layout = NRF_DFU_BANK_LAYOUT_DUAL;
        s_dfu_settings.bank_current = NRF_DFU_CURRENT_BANK_0;

        memset(&s_dfu_settings.bank_0, 0, sizeof(s_dfu_settings.bank_0));
        memset(&s_dfu_settings.bank_1, 0, sizeof(s_dfu_settings.bank_1));

        nrf_dfu_settings_write(NULL);
    }

    int32_t sz = flashAddr + FLASH_PAGE_SIZE;
    if (s_dfu_settings.bank_0.image_size < sz)
        s_dfu_settings.bank_0.image_size = sz;

    NRF_LOG_DEBUG("Flush at %x", flashAddr);
    if (memcmp(flashBuf, (void *)flashAddr, FLASH_PAGE_SIZE) != 0) {
        NRF_LOG_DEBUG("Write flush at %x", flashAddr);
        nrf_nvmc_page_erase(flashAddr);
        nrf_nvmc_write_words(flashAddr, (uint32_t *)flashBuf, FLASH_PAGE_SIZE / sizeof(uint32_t));
    }

    flashAddr = NO_CACHE;
}

void flash_write(uint32_t dst, const uint8_t *src, int len) {
    uint32_t newAddr = dst & ~(FLASH_PAGE_SIZE - 1);

    hadWrite = true;

    if (newAddr != flashAddr) {
        flushFlash();
        flashAddr = newAddr;
        memcpy(flashBuf, (void *)newAddr, FLASH_PAGE_SIZE);
    }
    memcpy(flashBuf + (dst & (FLASH_PAGE_SIZE - 1)), src, len);
}

void uf2_timer(void *p_context) {
    UNUSED_PARAMETER(p_context);
    if (hadWrite) {
        flushFlash();
        s_dfu_settings.bank_0.bank_code = NRF_DFU_BANK_VALID_APP;
        int32_t start = SD_MAGIC_OK() ? MAIN_APPLICATION_START_ADDR : MBR_SIZE;
        int32_t sz = s_dfu_settings.bank_0.image_size - start;
        if (sz > 0)
            s_dfu_settings.bank_0.image_size = sz;
        nrf_dfu_settings_write(NULL);
    }
    NVIC_SystemReset();
}

void uf2_timer_start(int ms);

void padded_memcpy(char *dst, const char *src, int len) {
    for (int i = 0; i < len; ++i) {
        if (*src)
            *dst = *src++;
        else
            *dst = ' ';
        dst++;
    }
}

void read_block(uint32_t block_no, uint8_t *data) {
    memset(data, 0, 512);
    uint32_t sectionIdx = block_no;

    if (block_no == 0) { // Requested boot block
        memcpy(data, &BootBlock, sizeof(BootBlock));
        data[510] = 0x55;
        data[511] = 0xaa;
        // logval("data[0]", data[0]);
    } else if (block_no < START_ROOTDIR) { // Requested a FAT table sector
        sectionIdx -= START_FAT0;
        // logval("sidx", sectionIdx);
        if (sectionIdx >= SECTORS_PER_FAT)
            sectionIdx -= SECTORS_PER_FAT; // Two copies of FATs, both are identical
        if (sectionIdx == 0) { // generate the FAT chains for single-sector files
            data[0] = 0xf0;
            for (int i = 1; i < NUM_FILES * 2 + 4; ++i) {
                // WARNING -- code presumes only one NULL .content for the .UF2 file
                //            and all non-NULL .content fit in one sector
                //            and requires the .UF2 file to be the last element of the array
                data[i] = 0xff;
            }
        }
        for (int i = 0; i < 256; ++i) { // generate the FAT chain for the .UF2 file
            uint32_t v = sectionIdx * 256 + i;
            if (UF2_FIRST_SECTOR <= v && v <= UF2_LAST_SECTOR)
                ((uint16_t *)(void *)data)[i] = v == UF2_LAST_SECTOR ? 0xffff : v + 1;
        }
    } else if (block_no < START_CLUSTERS) { // Requested one of the root directory sectors
        sectionIdx -= START_ROOTDIR;
        if (sectionIdx == 0) { // STATIC_ASSERT() above ensures only one sector has directory entries
            DirEntry *d = (void *)data;
            padded_memcpy(d->name, (const char *)BootBlock.VolumeLabel, 11);
            d->attrs = 0x28;
            for (int i = 0; i < NUM_FILES; ++i) {
                d++;
                const struct TextFile *inf = &info[i];
                // WARNING -- code presumes only one NULL .content for .UF2 file
                //            and requires it be the last element of the array
                d->size = inf->content ? strlen(inf->content) : UF2_SIZE;
                d->startCluster = i + 2;
                padded_memcpy(d->name, inf->name, 11);
                // FAT specification REQUIRES the creation date
                // (if this is not filled in, cannot list files in CMD / DOS)
                d->createDate = 0x4d99; // valid date
                d->updateDate = 0x4d99; // valid date
            }
        }
    } else { // Requested a sector that could contain user data (files, UF2)
        sectionIdx -= START_CLUSTERS;
        // WARNING -- code presumes each file (except last) fits in a single sector
        if (sectionIdx < NUM_FILES - 1) {
            memcpy(data, info[sectionIdx].content, strlen(info[sectionIdx].content));
        } else {
            sectionIdx -= NUM_FILES - 1;
            uint32_t addr = sectionIdx * 256;
            if (addr < FLASH_SIZE) {
                UF2_Block *bl = (void *)data;
                bl->magicStart0 = UF2_MAGIC_START0;
                bl->magicStart1 = UF2_MAGIC_START1;
                bl->magicEnd = UF2_MAGIC_END;
                bl->blockNo = sectionIdx;
                bl->numBlocks = FLASH_SIZE / 256;
                bl->targetAddr = addr;
                bl->payloadSize = 256;
                memcpy(bl->data, (void *)addr, bl->payloadSize);
            }
        }
    }
}

void write_block(uint32_t block_no, uint8_t *data, bool quiet, WriteState *state) {
    UF2_Block *bl = (void *)data;

    // NRF_LOG_DEBUG("Write magic: %x", bl->magicStart0);

    if (!is_uf2_block(bl)) {
        return;
    }
    // TODO -- Check if should ignore block with different family ID specified
    // if ( UF2_FAMILY_ID && !((bl->flags & UF2_FLAG_FAMILYID) && (bl->familyID == UF2_FAMILY_ID)) ) {
    //  return -1;
    //}

    if ((bl->flags & UF2_FLAG_NOFLASH) || bl->payloadSize > 256 || (bl->targetAddr & 0xff) ||
        bl->targetAddr < USER_FLASH_START || bl->targetAddr + bl->payloadSize > USER_FLASH_END) {
#if USE_DBG_MSC
        if (!quiet)
            logval("invalid target addr", bl->targetAddr);
#endif
        NRF_LOG_WARNING("Skip block at %x", bl->targetAddr);
        // this happens when we're trying to re-flash CURRENT.UF2 file previously
        // copied from a device; we still want to count these blocks to reset properly
    } else {
        // logval("write block at", bl->targetAddr);
        NRF_LOG_DEBUG("Write block at %x", bl->targetAddr);
        flash_write(bl->targetAddr, bl->data, bl->payloadSize);
    }

    bool isSet = false;

    if (state && bl->numBlocks) {
        if (state->numBlocks != bl->numBlocks) {
            if (bl->numBlocks >= MAX_BLOCKS || state->numBlocks)
                state->numBlocks = 0xffffffff;
            else
                state->numBlocks = bl->numBlocks;
        }
        if (bl->blockNo < MAX_BLOCKS) {
            uint8_t mask = 1 << (bl->blockNo % 8);
            uint32_t pos = bl->blockNo / 8;
            if (!(state->writtenMask[pos] & mask)) {
                // logval("incr", state->numWritten);
                state->writtenMask[pos] |= mask;
                state->numWritten++;
            }
            if (state->numWritten >= state->numBlocks) {
                // wait a little bit before resetting, to avoid Windows transmit error
                // https://github.com/Microsoft/uf2-samd21/issues/11
                if (!quiet) {
                    uf2_timer_start(30);
                    isSet = true;
                }
            }
        }
        NRF_LOG_DEBUG("wr %d=%d (of %d)", state->numWritten, bl->blockNo, bl->numBlocks);
    }

    if (!isSet && !quiet) {
        // uf2_timer_start(500);
    }
}
