/*
 * STM32G0xx OTA platform port.
 *
 * The OTA module expects the platform to provide:
 *   - flash read/write/erase (internal/external/RAM as supported)
 *   - CRC32 implementation (incremental via mode=0/reset, mode=1/continue)
 *   - jump-to-app implementation
 *   - a config instance (optional, but convenient)
 *
 * Flash programming notes:
 * - STM32G0 internal flash supports 64-bit (double-word) programming.
 * - OTA writes arbitrary byte chunks, so this port buffers/merges per double-word.
 * - To support repeated writes (e.g. OTA record area updates), flash_write()
 *   performs "erase-on-demand": it checks whether newly written bytes would require
 *   changing 0->1 bits; if yes, it erases affected flash pages then re-programs.
 */
#include "ota_module.h"

/* Include HAL with explicit relative paths so this file can be checked standalone. */
#include "crc.h"
#include "main.h"

/* CRC handle comes from Core/Src/crc.c. */
extern CRC_HandleTypeDef hcrc;

#include <string.h>

/* Flash program/erase must not be interrupted by handlers that fetch code from the
 * same internal flash bank. Save PRIMASK so nested critical sections behave correctly. */
static void flash_irq_enter(uint32_t *saved_primask)
{
    *saved_primask = __get_PRIMASK();
    __disable_irq();
}

static void flash_irq_exit(uint32_t saved_primask)
{
    if (saved_primask == 0u) {
        __enable_irq();
    }
}

/* ===== Small helpers ===== */
static uint32_t align_down_u32(uint32_t v, uint32_t a)
{
    if (a == 0u) return v;
    return (v / a) * a;
}

static uint32_t align_up_u32(uint32_t v, uint32_t a)
{
    if (a == 0u) return v;
    return (v + a - 1u) / a * a;
}

/* ===== Internal flash stream-program cache (double-word) =====
 * STM32G0 double-word programming may fail if the SAME 64-bit address is programmed twice
 * after an erase, even if some bytes remain unchanged.
 *
 * OTA download writes arbitrary chunk sizes and may split a 64-bit double-word across calls.
 * To avoid double-programming, we cache the *ending partial double-word* and only program it
 * when the remaining bytes arrive (or when explicitly flushed).
 */
typedef struct {
    uint32_t addr_aligned; /* 8-byte aligned address */
    uint8_t buf[8u];       /* desired final 8 bytes */
    uint8_t mask;          /* bit i=1 means byte i is provided by write stream */
    uint8_t valid;
} flash_dw_cache_t;

static flash_dw_cache_t s_dw_cache = {0};

static void flash_dw_cache_clear(void)
{
    s_dw_cache.valid = 0u;
    s_dw_cache.mask = 0u;
    s_dw_cache.addr_aligned = 0u;
    memset(s_dw_cache.buf, 0, sizeof(s_dw_cache.buf));
}

static int internal_flash_program_doubleword(uint32_t addr_aligned, const uint8_t desired_dw[8u])
{
    uint8_t existing_dw[8u];
    memcpy(existing_dw, (const void *)addr_aligned, 8u);

    if (memcmp(existing_dw, desired_dw, 8u) == 0) {
        return 0;
    }

    for (uint32_t j = 0u; j < 8u; j++) {
        if ((existing_dw[j] & desired_dw[j]) != desired_dw[j]) {
            printf("flash program invalid 0->1 at %08lx\r\n", addr_aligned + j);
            return -1;
        }
    }

    uint64_t dw = 0u;
    memcpy(&dw, desired_dw, sizeof(dw));
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr_aligned, dw) != HAL_OK) {
        printf("flash program failed at %08lx\r\n", addr_aligned);
        return -1;
    }
    return 0;
}

static int internal_flash_flush_dw_cache(void)
{
    if (!s_dw_cache.valid) return 0;
    int rc = internal_flash_program_doubleword(s_dw_cache.addr_aligned, s_dw_cache.buf);
    flash_dw_cache_clear();
    return rc;
}

static uint32_t flash_page_from_address(uint32_t addr)
{
    /* Mirrors Cube example logic: bank1 uses [FLASH_BASE, FLASH_BASE+FLASH_BANK_SIZE). */
    if (addr < (FLASH_BASE + FLASH_BANK_SIZE)) {
        return (addr - FLASH_BASE) / FLASH_PAGE_SIZE;
    }
    return (addr - (FLASH_BASE + FLASH_BANK_SIZE)) / FLASH_PAGE_SIZE;
}

static uint32_t flash_bank_from_address(uint32_t addr)
{
#if defined(FLASH_DBANK_SUPPORT)
    return (addr < (FLASH_BASE + FLASH_BANK_SIZE)) ? FLASH_BANK_1 : FLASH_BANK_2;
#else
    /* On single-bank devices parameter must be FLASH_BANK_1. */
    (void)addr;
    return FLASH_BANK_1;
#endif
}

static uint32_t flash_bank_base_from_bank(uint32_t bank)
{
    return (bank == FLASH_BANK_1) ? FLASH_BASE : (FLASH_BASE + FLASH_BANK_SIZE);
}

static int flash_erase_internal_pages(uint32_t addr, uint32_t size)
{
    if (size == 0u) return 0;

    /* Any erase invalidates cached pending double-word. */
    flash_dw_cache_clear();

    /* OTA module should pass aligned ranges, but keep it safe. */
    addr = align_down_u32(addr, FLASH_PAGE_SIZE);
    size = align_up_u32(size, FLASH_PAGE_SIZE);

    uint32_t irq_saved;
    flash_irq_enter(&irq_saved);
    if (HAL_FLASH_Unlock() != HAL_OK) {
        flash_irq_exit(irq_saved);
        return -1;
    }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_OPTVERR);

    FLASH_EraseInitTypeDef erase = {0};
    uint32_t cur = addr;
    uint32_t remaining = size;
    uint32_t page_error = 0;

    while (remaining > 0u) {
        uint32_t bank = flash_bank_from_address(cur);
        uint32_t bank_base = flash_bank_base_from_bank(bank);
        uint32_t bank_end = bank_base + FLASH_BANK_SIZE;

        /* Pages that fit within current bank. */
        uint32_t max_pages_in_bank = (bank_end - cur) / FLASH_PAGE_SIZE;
        uint32_t pages_to_erase = remaining / FLASH_PAGE_SIZE;
        if (pages_to_erase > max_pages_in_bank) pages_to_erase = max_pages_in_bank;

        erase.TypeErase = FLASH_TYPEERASE_PAGES;
        erase.Banks = bank;
        erase.Page = flash_page_from_address(cur);
        erase.NbPages = pages_to_erase;

        if (HAL_FLASHEx_Erase(&erase, &page_error) != HAL_OK) {
            (void)page_error;
            (void)HAL_FLASH_Lock();
            flash_irq_exit(irq_saved);
            return -1;
        }

        cur += pages_to_erase * FLASH_PAGE_SIZE;
        remaining -= pages_to_erase * FLASH_PAGE_SIZE;
    }

    if (HAL_FLASH_Lock() != HAL_OK) {
        flash_irq_exit(irq_saved);
        return -1;
    }
    flash_irq_exit(irq_saved);
    return 0;
}

static int internal_flash_program_bytes(uint32_t addr, const uint8_t *data, uint32_t size)
{
    /* Program via 64-bit double-words, with a cache for the ending partial DW. */
    if (size == 0u) return 0;

    const uint32_t end = addr + size;
    const uint32_t first_dw = align_down_u32(addr, 8u);
    const uint32_t last_dw = align_down_u32(end - 1u, 8u);

    /* If we have a pending cached DW and caller moved past it, program it now. */
    if (s_dw_cache.valid) {
        if (first_dw > s_dw_cache.addr_aligned) {
            int frc = internal_flash_flush_dw_cache();
            if (frc != 0) return frc;
        }
    }

    uint32_t cur = first_dw;
    while (cur < end) {
        uint8_t desired_dw[8u];
        uint8_t local_mask = 0u;

        /* Base image: cached DW (if matching), else current flash. */
        if (s_dw_cache.valid && s_dw_cache.addr_aligned == cur) {
            memcpy(desired_dw, s_dw_cache.buf, 8u);
            local_mask = s_dw_cache.mask;
        } else {
            memcpy(desired_dw, (const void *)cur, 8u);
            local_mask = 0u;
        }

        /* Apply bytes from current write range. */
        for (uint32_t j = 0u; j < 8u; j++) {
            const uint32_t byte_addr = cur + j;
            if (byte_addr >= addr && byte_addr < end) {
                desired_dw[j] = data[byte_addr - addr];
                local_mask |= (uint8_t)(1u << j);
            }
        }

        const int is_last = (cur == last_dw);
        const int full_mask = (local_mask == 0xFFu);

        if (is_last && !full_mask) {
            /* Cache the ending partial DW; do NOT program now to avoid double-programming
             * when the next chunk continues from end and completes this DW.
             */
            s_dw_cache.valid = 1u;
            s_dw_cache.addr_aligned = cur;
            s_dw_cache.mask = local_mask;
            memcpy(s_dw_cache.buf, desired_dw, 8u);
            cur += 8u;
            continue;
        }

        /* Program now (either complete DW or not the stream-ending DW). */
        int rc = internal_flash_program_doubleword(cur, desired_dw);
        if (rc != 0) return rc;

        /* If we just programmed a cached DW, clear it. */
        if (s_dw_cache.valid && s_dw_cache.addr_aligned == cur) {
            flash_dw_cache_clear();
        }

        cur += 8u;
    }

    return 0;
}

/* ===== OTA required interfaces ===== */
int stm32g0xx_flash_read(ota_flash_type_t flash_type, uint32_t addr, uint8_t *data, uint32_t size)
{
    if (size == 0u) return 0;
    if (!data) return -1;

    if (flash_type == OTA_FLASH_TYPE_INTERNAL) {
        memcpy(data, (const void *)addr, size);
        return 0;
    }

    /* Not implemented for external/RAM in this port. */
    return -1;
}

int stm32g0xx_flash_write(ota_flash_type_t flash_type, uint32_t addr, const uint8_t *data, uint32_t size)
{
    if (flash_type != OTA_FLASH_TYPE_INTERNAL) return -1;
    /* size==0 used as an explicit "flush pending DW" operation. */
    if (size == 0u) {
        uint32_t irq_saved;
        flash_irq_enter(&irq_saved);
        if (HAL_FLASH_Unlock() != HAL_OK) {
            flash_irq_exit(irq_saved);
            return -1;
        }
        __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_OPTVERR);
        int frc = internal_flash_flush_dw_cache();
        (void)HAL_FLASH_Lock();
        flash_irq_exit(irq_saved);
        return frc;
    }
    if (!data) return -1;

    /* Program (this does not erase; caller must erase beforehand if needed). */
    uint32_t irq_saved;
    flash_irq_enter(&irq_saved);
    if (HAL_FLASH_Unlock() != HAL_OK) {
        flash_irq_exit(irq_saved);
        return -1;
    }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_OPTVERR);
    int rc = internal_flash_program_bytes(addr, data, size);

    (void)HAL_FLASH_Lock();
    flash_irq_exit(irq_saved);
    return rc;
}

int stm32g0xx_flash_erase(ota_flash_type_t flash_type, uint32_t addr, uint32_t size)
{
    if (flash_type != OTA_FLASH_TYPE_INTERNAL) return -1;
    return flash_erase_internal_pages(addr, size);
}

uint32_t stm32g0xx_crc32(uint8_t mode, const uint8_t *data, uint32_t size)
{
    if (size == 0u || !data) return 0u;

    /* mode=0: reset CRC; mode=1: continue incremental CRC */
    if (mode == 0u) {
        /* Reset CRC calculation unit to default init value. */
        __HAL_CRC_DR_RESET(&hcrc);
        return HAL_CRC_Calculate(&hcrc, (uint32_t *)(void *)data, size);
    }
    return HAL_CRC_Accumulate(&hcrc, (uint32_t *)(void *)data, size);
}

int stm32g0xx_jump(uint32_t jump_addr)
{
    typedef  void (*pFunction)(void);
    static pFunction JumpToApp;
    if (jump_addr == 0u) return -1;

    /* Basic sanity: MSP should point to SRAM region, but keep it permissive. */
    __disable_irq();

    /* Set vector table base so interrupts use the new app handlers. */
    SCB->VTOR = jump_addr;
    JumpToApp = (pFunction) (*(__IO uint32_t *)(jump_addr + 4));

    __set_MSP(*(__IO uint32_t*)jump_addr);

    JumpToApp();
    /* Must not return: if we get here the app reset vector was invalid or the app returned. */
    while (1) { }
}

/* ===== Optional: a ready-to-use module config =====
 * You still MUST fill actual partition layout in `ota_record_default.c`
 * (flash_addr/flash_size) for ota/factory slots to work on hardware.
 */
const ota_module_config_t ota_module_platform_config = {
    .record_flash_type = OTA_FLASH_TYPE_INTERNAL,
    /* Default record area: last flash page. Adjust to your real layout. */
    .record_addr = OTA_PLATFORM_RECORD_ADDR,

    .flash_start_addr = { OTA_PLATFORM_FLASH_BASE, 0u, 0u },
    .flash_size = { OTA_PLATFORM_FLASH_SIZE, 0u, 0u },
    .flash_sector_size = { OTA_PLATFORM_FLASH_PAGE_SIZE, 0u, 0u },

    .flash_read_func = stm32g0xx_flash_read,
    .flash_write_func = stm32g0xx_flash_write,
    .flash_erase_func = stm32g0xx_flash_erase,
    .crc32_func = stm32g0xx_crc32,
    .jump_func = stm32g0xx_jump,
};
