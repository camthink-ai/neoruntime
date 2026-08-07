#include "../inc/ota_module.h"

#include <stddef.h>

static void memclr(void *p, size_t n)
{
    uint8_t *b = (uint8_t *)p;
    for (size_t i = 0; i < n; i++) b[i] = 0;
}

/* Platform supplies these behaviors via ota_module_config_t. */
static ota_module_config_t g_cfg;
static int g_inited = 0;

/* Default record used when record area CRC/magic invalid. */
extern const ota_record_info_t ota_record_default_info;

typedef struct {
    int started;
    uint32_t target_index; /* index in ota_app_info[] */

    /* runtime download session state (not required to survive reset) */
    ota_package_header_t package_header_copy;
    uint32_t expected_app_size;
    uint32_t expected_total_size; /* app_offset + app_size */
    uint32_t expected_app_crc32;
    uint32_t downloaded_size;
    uint32_t downloaded_crc32;
    int crc_inited;
} ota_download_session_t;

static ota_download_session_t g_dl = {0};

/* Reduce stack usage: reuse a static buffer for incremental CRC reads. */
#ifndef OTA_MODULE_FLASH_COPY_BUF_SIZE
#define OTA_MODULE_FLASH_COPY_BUF_SIZE 256u
#endif
static uint8_t g_flash_copy_buf[OTA_MODULE_FLASH_COPY_BUF_SIZE];

/* Read OTA package header (if stored in flash) to calculate entry address. */
static ota_package_header_t g_pkg_header_buf;

/* ===== Helpers: CRC validation & record persistence ===== */
static uint32_t crc32_one_shot(uint32_t (*crc32_func)(uint8_t mode, const uint8_t *data, uint32_t size),
                                const uint8_t *data, uint32_t size)
{
    /* mode=0 means reset; callers pass complete data for one-shot checks. */
    return crc32_func(0, data, size);
}

static uint32_t record_crc32_without_info_crc32(const ota_record_info_t *rec)
{
    const uint8_t *bytes = (const uint8_t *)rec;
    size_t len = offsetof(ota_record_info_t, info_crc32);
    return crc32_one_shot(g_cfg.crc32_func, bytes, (uint32_t)len);
}

static uint32_t package_header_crc32_without_header_crc32(const ota_package_header_t *hdr)
{
    const uint8_t *bytes = (const uint8_t *)hdr;
    size_t len = offsetof(ota_package_header_t, header_crc32);
    return crc32_one_shot(g_cfg.crc32_func, bytes, (uint32_t)len);
}

static int record_load(ota_record_info_t *out)
{
    if (!out) return OTA_MODULE_ERR_INVALID_ARG;
    if (!g_inited) return OTA_MODULE_ERR_NOT_INIT;

    ota_record_info_t tmp;
    memclr(&tmp, sizeof(tmp));

    if (g_cfg.flash_read_func(g_cfg.record_flash_type, g_cfg.record_addr, (uint8_t *)&tmp, (uint32_t)sizeof(tmp)) != 0) {
        *out = ota_record_default_info;
        OTA_LOGE("record_load: flash read failed");
        return OTA_MODULE_ERR_RECORD_IO;
    }

    if (tmp.magic != OTA_INFO_MAGIC) {
        *out = ota_record_default_info;
        OTA_LOGW("record_load: magic invalid");
        return OTA_MODULE_ERR_RECORD_INVALID;
    }

    uint32_t crc = record_crc32_without_info_crc32(&tmp);
    if (crc != tmp.info_crc32) {
        *out = ota_record_default_info;
        OTA_LOGW("record_load: crc invalid");
        return OTA_MODULE_ERR_RECORD_INVALID;
    }

    *out = tmp;
    return OTA_MODULE_ERR_OK;
}

/* Forward declaration to allow record_store() to erase record area safely. */
static int flash_erase_slot(ota_flash_type_t type, uint32_t addr, uint32_t size);

static int record_store(const ota_record_info_t *rec)
{
    if (!rec) return OTA_MODULE_ERR_INVALID_ARG;
    if (!g_inited) return OTA_MODULE_ERR_NOT_INIT;

    ota_record_info_t tmp = *rec;
    tmp.magic = OTA_INFO_MAGIC;
    tmp.info_crc32 = record_crc32_without_info_crc32(&tmp);

    /* The record area may be updated multiple times and fields can move from
     * 0 back to 1. Erase the aligned record sector before each write so this
     * stays compatible with flash drivers that only allow 1 -> 0 writes. */
    if (flash_erase_slot(g_cfg.record_flash_type, g_cfg.record_addr, (uint32_t)sizeof(tmp)) != 0) {
        OTA_LOGE("record_store: flash erase failed");
        return OTA_MODULE_ERR_FLASH_OP;
    }

    if (g_cfg.flash_write_func(g_cfg.record_flash_type, g_cfg.record_addr, (const uint8_t *)&tmp, (uint32_t)sizeof(tmp)) != 0) {
        OTA_LOGE("record_store: flash write failed");
        return OTA_MODULE_ERR_FLASH_OP;
    }
    /* Ensure any platform-side pending double-word cache is committed. */
    (void)g_cfg.flash_write_func(g_cfg.record_flash_type, 0u, NULL, 0u);
    return OTA_MODULE_ERR_OK;
}

/* ===== Helpers: flash erase/copy ===== */
static uint32_t align_up(uint32_t v, uint32_t a)
{
    if (a == 0) return v;
    return (v + a - 1u) / a * a;
}

static uint32_t align_down(uint32_t v, uint32_t a)
{
    if (a == 0) return v;
    return (v / a) * a;
}

static int flash_erase_slot(ota_flash_type_t type, uint32_t addr, uint32_t size)
{
    uint32_t sector = g_cfg.flash_sector_size[type];
    if (sector == 0) return OTA_MODULE_ERR_INVALID_ARG;
    uint32_t erase_addr = align_down(addr, sector);
    /* Expand the erase range to cover the original [addr, addr+size) after aligning. */
    uint32_t erase_size = align_up((addr + size) - erase_addr, sector);
    if (g_cfg.flash_erase_func(type, erase_addr, erase_size) != 0) {
        OTA_LOGE("flash_erase_slot: erase failed type=%lu addr=%lu size=%lu",
                 (unsigned long)type, (unsigned long)erase_addr, (unsigned long)erase_size);
        return OTA_MODULE_ERR_FLASH_OP;
    }
    return OTA_MODULE_ERR_OK;
}

static int choose_target_ota_slot(const ota_record_info_t *rec, uint32_t *out_index)
{
    if (!rec || !out_index) return OTA_MODULE_ERR_INVALID_ARG;

    const uint32_t count = (uint32_t)(OTA_IS_DUAL_APP ? 2u : 1u);

    /* 1) Prefer DOWNLOADING so interrupted power loss can be overwritten on next OTA. */
    for (uint32_t i = 0; i < count; i++) {
        if (rec->ota_app_info[i].status == OTA_APP_STATUS_DOWNLOADING) {
            *out_index = i;
            return 0;
        }
    }
    /* 2) Then INVALID (abort can set INVALID; also skipable after boot). */
    for (uint32_t i = 0; i < count; i++) {
        if (rec->ota_app_info[i].status == OTA_APP_STATUS_INVALID) {
            *out_index = i;
            return 0;
        }
    }
    /* 3) Then BACKUP */
    for (uint32_t i = 0; i < count; i++) {
        if (rec->ota_app_info[i].status == OTA_APP_STATUS_BACKUP) {
            *out_index = i;
            return 0;
        }
    }

    /* Single-slot fallback:
     * If there is only one OTA slot, allow overwriting it even if it's ACTIVE/UNVERIFIED.
     * This enables "update in bootloader" deployments where no spare slot exists.
     * (Overwriting an executing image from within the running app is unsafe; integrator must avoid that.)
     */
    if (count == 1u) {
        *out_index = 0u;
        return 0;
    }
    return OTA_MODULE_ERR_SLOT_UNAVAILABLE;
}

static int choose_boot_candidate_ota_slot(const ota_record_info_t *rec, uint32_t *out_index)
{
    if (!rec || !out_index) return OTA_MODULE_ERR_INVALID_ARG;
    const uint32_t count = (uint32_t)(OTA_IS_DUAL_APP ? 2u : 1u);

    /* Skip DOWNLOADING/INVALID for safety. Prefer UNVERIFIED to validate new firmware. */
    for (uint32_t i = 0; i < count; i++) {
        if (rec->ota_app_info[i].status == OTA_APP_STATUS_UNVERIFIED) {
            *out_index = i;
            return 0;
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        if (rec->ota_app_info[i].status == OTA_APP_STATUS_ACTIVE) {
            *out_index = i;
            return 0;
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        if (rec->ota_app_info[i].status == OTA_APP_STATUS_BACKUP) {
            *out_index = i;
            return 0;
        }
    }
    return OTA_MODULE_ERR_NO_BOOT_CANDIDATE;
}

static uint32_t ota_calc_entry_addr_from_header(const ota_app_info_t *app_info)
{
    uint32_t base = 0;
    if (!app_info) return base;
    base = app_info->flash_addr;

    /* If package header is stored at the partition start, entry address is base + app_offset. */
    if (g_cfg.flash_read_func == NULL) return base;
    if (app_info->flash_size < sizeof(ota_package_header_t)) return base;

    memclr(&g_pkg_header_buf, sizeof(g_pkg_header_buf));
    if (g_cfg.flash_read_func(app_info->flash_type, app_info->flash_addr, (uint8_t *)&g_pkg_header_buf,
                              (uint32_t)sizeof(g_pkg_header_buf)) != 0) {
        OTA_LOGD("ota_calc_entry_addr: header read failed");
        return base;
    }

    if (g_pkg_header_buf.magic != OTA_PACKAGE_MAGIC) {
        OTA_LOGD("ota_calc_entry_addr: header magic mismatch");
        return base;
    }

    /* CRC check: only when magic and header CRC are both valid,
     * return base + app_offset. Otherwise fallback to base. */
    if (g_pkg_header_buf.header_crc32 != package_header_crc32_without_header_crc32(&g_pkg_header_buf)) {
        OTA_LOGD("ota_calc_entry_addr: header crc mismatch");
        return base;
    }

    if (g_pkg_header_buf.app_offset >= app_info->flash_size) {
        OTA_LOGD("ota_calc_entry_addr: app_offset out of range");
        return base;
    }

    OTA_LOGD("ota_calc_entry_addr: base=%lu app_offset=%lu",
             (unsigned long)base, (unsigned long)g_pkg_header_buf.app_offset);
    return base + g_pkg_header_buf.app_offset;
}

static int flash_crc32_app_body(const ota_app_info_t *app_info,
                                 const ota_package_header_t *hdr,
                                 uint32_t *out_crc)
{
    uint32_t crc = 0;
    uint32_t remaining = 0;
    uint32_t addr = 0;
    uint32_t want = 0;
    uint8_t first = 1;

    if (!app_info || !hdr || !out_crc) return OTA_MODULE_ERR_INVALID_ARG;
    if ((hdr->app_offset + hdr->app_size) > app_info->flash_size) return OTA_MODULE_ERR_FLASH_SIZE;

    remaining = hdr->app_size;
    addr = app_info->flash_addr + hdr->app_offset;

    while (remaining > 0) {
        want = remaining;
        if (want > OTA_MODULE_FLASH_COPY_BUF_SIZE) want = OTA_MODULE_FLASH_COPY_BUF_SIZE;

        if (g_cfg.flash_read_func(app_info->flash_type, addr, g_flash_copy_buf, want) != 0) {
            return OTA_MODULE_ERR_FLASH_OP;
        }

        if (first) {
            crc = g_cfg.crc32_func(0, g_flash_copy_buf, want);
            first = 0;
        } else {
            crc = g_cfg.crc32_func(1, g_flash_copy_buf, want);
        }

        addr += want;
        remaining -= want;
    }

    *out_crc = crc;
    return OTA_MODULE_ERR_OK;
}

/* ===== Public APIs ===== */
int ota_module_init(const ota_module_config_t *config)
{
    if (!config) return OTA_MODULE_ERR_INVALID_ARG;

    if (!config->flash_read_func || !config->flash_write_func || !config->flash_erase_func ||
        !config->crc32_func || !config->jump_func) {
        OTA_LOGE("ota_module_init: missing config function pointers");
        return OTA_MODULE_ERR_INVALID_ARG;
    }

    g_cfg = *config;
    g_inited = 1;
    memclr(&g_dl, sizeof(g_dl));

    /* Ensure record area becomes valid once at init (if platform provides correct default record). */
    ota_record_info_t rec;
    int rc = record_load(&rec);
    if (rc != 0) {
        rec = ota_record_default_info;
        rec.magic = OTA_INFO_MAGIC;
        rc = record_store(&rec);
        if (rc != 0) return rc;
    }

    OTA_LOGI("ota_module_init: ok");
    return OTA_MODULE_ERR_OK;
}

int ota_module_boot_preprocess(void)
{
    ota_record_info_t rec;
    int rc = record_load(&rec);
    (void)rc;

    /* 1) UNVERIFIED: execute in place from that OTA slot (retry budget via verify_count). */
    uint32_t unverified_idx = 0;
    uint32_t count = (uint32_t)(OTA_IS_DUAL_APP ? 2u : 1u);
    int has_unverified = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (rec.ota_app_info[i].status == OTA_APP_STATUS_UNVERIFIED) {
            unverified_idx = i;
            has_unverified = 1;
            break;
        }
    }

    if (has_unverified) {
        /* Persist retry count BEFORE jumping so repeated resets consume the budget. */
        ota_app_info_t *uv = &rec.ota_app_info[unverified_idx];
        if (uv->verify_count + 1u >= OTA_APP_MAX_VERIFY_COUNT) {
            uv->status = OTA_APP_STATUS_INVALID;
            uv->verify_count = 0;
            rc = record_store(&rec);
            if (rc != 0) return rc;
            /* Fall through to ACTIVE/BACKUP/factory selection. */
        } else {
            uv->verify_count += 1u;
            rc = record_store(&rec);
            if (rc != 0) return rc;
            rc = g_cfg.jump_func(ota_calc_entry_addr_from_header(uv));
            if (rc != 0) return OTA_MODULE_ERR_JUMP_FAILED;
            return OTA_MODULE_ERR_OK;
        }
    }

    /* 2) Otherwise: choose an OTA slot (skip DOWNLOADING/INVALID) and jump in place. */
    uint32_t chosen_idx = 0;
    int chosen = choose_boot_candidate_ota_slot(&rec, &chosen_idx);
    if (chosen == 0) {
        rc = g_cfg.jump_func(ota_calc_entry_addr_from_header(&rec.ota_app_info[chosen_idx]));
        if (rc != 0) return OTA_MODULE_ERR_JUMP_FAILED;
        return OTA_MODULE_ERR_OK;
    }

#if OTA_IS_HAVE_FACTORY_APP
    if (rec.factory_app_info.status != OTA_APP_STATUS_INVALID &&
        rec.factory_app_info.status != OTA_APP_STATUS_DOWNLOADING) {
        rc = g_cfg.jump_func(ota_calc_entry_addr_from_header(&rec.factory_app_info));
        if (rc != 0) return OTA_MODULE_ERR_JUMP_FAILED;
        return OTA_MODULE_ERR_OK;
    }
#endif

    OTA_LOGE("ota_module_boot_preprocess: no boot candidate");
    return OTA_MODULE_ERR_NO_BOOT_CANDIDATE;
}

int ota_module_has_active_app_region(void)
{
    if (!g_inited) {
        return 0;
    }
    ota_record_info_t rec;
    if (record_load(&rec) != 0) {
        return 0;
    }
    const uint32_t count = (uint32_t)(OTA_IS_DUAL_APP ? 2u : 1u);
    for (uint32_t i = 0; i < count; i++) {
        if (rec.ota_app_info[i].status == OTA_APP_STATUS_ACTIVE) {
            return 1;
        }
    }
#if OTA_IS_HAVE_FACTORY_APP
    if (rec.factory_app_info.status == OTA_APP_STATUS_ACTIVE) {
        return 1;
    }
#endif
    return 0;
}

int ota_module_get_record_info(ota_record_info_t *info)
{
    if (!info) return OTA_MODULE_ERR_INVALID_ARG;
    ota_record_info_t rec;
    int rc = record_load(&rec);
    (void)rc;
    *info = rec;
    return rc;
}

int ota_module_ota_download_start(const ota_package_header_t *package_header)
{
    if (!package_header) return OTA_MODULE_ERR_INVALID_ARG;
    if (!g_inited) return OTA_MODULE_ERR_NOT_INIT;
    if (g_dl.started) {
        OTA_LOGW("ota_download_start: already in a session");
        return OTA_MODULE_ERR_ALREADY_IN_PROGRESS;
    }

    /* Validate package header CRC32. */
    if (package_header->magic != OTA_PACKAGE_MAGIC) {
        OTA_LOGE("ota_download_start: package magic invalid");
        return OTA_MODULE_ERR_PACKAGE_MAGIC;
    }
    uint32_t expected_hdr_crc = package_header_crc32_without_header_crc32(package_header);
    if (expected_hdr_crc != package_header->header_crc32) {
        OTA_LOGE("ota_download_start: header crc invalid");
        return OTA_MODULE_ERR_PACKAGE_HEADER_CRC;
    }

    ota_record_info_t rec;
    int rc = record_load(&rec);
    (void)rc;

    uint32_t target_idx = 0;
    if (choose_target_ota_slot(&rec, &target_idx) != 0) {
        OTA_LOGE("ota_download_start: no target OTA slot available");
        return OTA_MODULE_ERR_SLOT_UNAVAILABLE;
    }

    ota_app_info_t *target = &rec.ota_app_info[target_idx];
    /* app_offset is relative to OTA package start (download offset=0). */
    if (package_header->app_offset >= target->flash_size) {
        OTA_LOGE("ota_download_start: app_offset out of range");
        return OTA_MODULE_ERR_FLASH_SIZE;
    }
    if ((package_header->app_offset + package_header->app_size) > target->flash_size) {
        OTA_LOGE("ota_download_start: app_offset+app_size exceeds slot");
        return OTA_MODULE_ERR_FLASH_SIZE;
    }
    if (package_header->app_size > target->flash_size) {
        OTA_LOGE("ota_download_start: app_size too large");
        return OTA_MODULE_ERR_FLASH_SIZE;
    }

    /* Erase up to (app_offset + app_size): header + padding + app will be rewritten by download(). */
    uint32_t erase_len = package_header->app_offset + package_header->app_size;
    if (erase_len > target->flash_size) {
        OTA_LOGE("ota_download_start: erase_len exceeds slot");
        return OTA_MODULE_ERR_FLASH_SIZE;
    }
    if (flash_erase_slot(target->flash_type, target->flash_addr, erase_len) != 0) {
        OTA_LOGE("ota_download_start: flash erase failed");
        return OTA_MODULE_ERR_FLASH_OP;
    }

    /* Update record: mark as DOWNLOADING so boot skips it and next OTA can reuse it. */
    target->status = OTA_APP_STATUS_DOWNLOADING;
    target->verify_count = 0;
    rec.info_crc32 = 0; /* updated by record_store() */
    rc = record_store(&rec);
    if (rc != 0) {
        OTA_LOGE("ota_download_start: record_store failed");
        /* Best-effort mark invalid to avoid boot using this slot. */
        target->status = OTA_APP_STATUS_INVALID;
        (void)record_store(&rec);
        return rc;
    }

    OTA_LOGI("ota_download_start: target_idx=%lu app_size=%lu",
             (unsigned long)target_idx,
             (unsigned long)package_header->app_size);

    /* Runtime session context. */
    memclr(&g_dl, sizeof(g_dl));
    g_dl.started = 1;
    g_dl.target_index = target_idx;
    g_dl.package_header_copy = *package_header;
    g_dl.expected_app_size = package_header->app_size;
    g_dl.expected_total_size = package_header->app_offset + package_header->app_size;
    g_dl.expected_app_crc32 = package_header->app_crc32;
    g_dl.downloaded_size = 0;
    g_dl.downloaded_crc32 = 0;
    g_dl.crc_inited = 0;

    return OTA_MODULE_ERR_OK;
}

int ota_module_ota_download(uint32_t offset, const uint8_t *data, uint32_t size)
{
    if (!data && size != 0) return OTA_MODULE_ERR_INVALID_ARG;
    if (!g_inited) return OTA_MODULE_ERR_NOT_INIT;
    if (!g_dl.started) return OTA_MODULE_ERR_SESSION_MISMATCH;

    ota_record_info_t rec;
    int rc = record_load(&rec);
    if (rc != 0) {
        OTA_LOGE("ota_download: record_load failed rc=%d", rc);
        return rc;
    }

    const uint32_t target_idx = g_dl.target_index;
    const ota_app_info_t *target = &rec.ota_app_info[target_idx];
    if (target->status != OTA_APP_STATUS_DOWNLOADING) {
        /* Session mismatch after reset/abort. */
        OTA_LOGE("ota_download: target status mismatch=%d", (int)target->status);
        return OTA_MODULE_ERR_SESSION_MISMATCH;
    }

    if (offset != g_dl.downloaded_size) {
        OTA_LOGE("ota_download: offset mismatch exp=%lu got=%lu",
                 (unsigned long)g_dl.downloaded_size, (unsigned long)offset);
        return OTA_MODULE_ERR_OFFSET_MISMATCH;
    }
    if (offset + size > g_dl.expected_total_size) {
        OTA_LOGE("ota_download: size mismatch offset=%lu size=%lu total=%lu",
                 (unsigned long)offset,
                 (unsigned long)size,
                 (unsigned long)g_dl.expected_total_size);
        return OTA_MODULE_ERR_SIZE_MISMATCH;
    }
    if (size == 0) return OTA_MODULE_ERR_OK;

    /* Write to partition start + offset (offset is relative to package start). */
    uint32_t dst_addr = target->flash_addr + offset;
    if ((dst_addr + size) > (target->flash_addr + target->flash_size)) {
        OTA_LOGE("ota_download: dst addr out of slot range");
        return OTA_MODULE_ERR_FLASH_SIZE;
    }

    if (g_cfg.flash_write_func(target->flash_type, dst_addr, data, size) != 0) {
        OTA_LOGE("ota_download: flash write failed");
        return OTA_MODULE_ERR_FLASH_OP;
    }

    g_dl.downloaded_size += size;
    /* download CRC is intentionally not computed; final CRC is checked in ota_download_finish(). */
    return OTA_MODULE_ERR_OK;
}

int ota_module_ota_get_download_info(ota_download_info_t *info)
{
    if (!info) return OTA_MODULE_ERR_INVALID_ARG;
    if (!g_inited) return OTA_MODULE_ERR_NOT_INIT;

    if (!g_dl.started) {
        memclr(info, sizeof(*info));
        info->status = OTA_DOWNLOAD_STATUS_INVALID;
        return OTA_MODULE_ERR_SESSION_MISMATCH;
    }

    ota_record_info_t rec;
    int rc = record_load(&rec);
    if (rc != 0) return rc;

    const uint32_t target_idx = g_dl.target_index;
    const ota_app_info_t *target = &rec.ota_app_info[target_idx];

    memclr(info, sizeof(*info));
    info->status = OTA_DOWNLOAD_STATUS_DOWNLOADING;
    info->ota_app_info = *target;
    info->package_header = g_dl.package_header_copy;
    info->download_size = g_dl.downloaded_size;
    info->download_crc32 = 0;
    return OTA_MODULE_ERR_OK;
}

int ota_module_ota_download_finish(void)
{
    if (!g_inited) return OTA_MODULE_ERR_NOT_INIT;
    if (!g_dl.started) return OTA_MODULE_ERR_SESSION_MISMATCH;

    /* Load record and update target status. */
    ota_record_info_t rec;
    int rc = record_load(&rec);
    if (rc != 0) return rc;

    const uint32_t target_idx = g_dl.target_index;
    ota_app_info_t *target = &rec.ota_app_info[target_idx];
    if (target->status != OTA_APP_STATUS_DOWNLOADING) {
        OTA_LOGE("ota_download_finish: target status mismatch=%d", (int)target->status);
        return OTA_MODULE_ERR_APP_STATUS;
    }

    if (g_dl.downloaded_size != g_dl.expected_total_size) {
        OTA_LOGE("ota_download_finish: size mismatch downloaded=%lu expected=%lu",
                 (unsigned long)g_dl.downloaded_size,
                 (unsigned long)g_dl.expected_total_size);
        return OTA_MODULE_ERR_SIZE_MISMATCH;
    }

    /* Ensure any platform-side pending double-word cache is committed before CRC. */
    (void)g_cfg.flash_write_func(target->flash_type, 0u, NULL, 0u);

    /* Strict CRC check:
     * - compute CRC32 from app_offset
     * - header + padding bytes are naturally skipped (read starts at app_offset)
     */
    uint32_t flash_crc = 0;
    rc = flash_crc32_app_body(target, &g_dl.package_header_copy, &flash_crc);
    if (rc != OTA_MODULE_ERR_OK) return rc;
    if (flash_crc != g_dl.expected_app_crc32) {
        OTA_LOGE("ota_download_finish: crc mismatch flash=%lu expected=%lu",
                 (unsigned long)flash_crc, (unsigned long)g_dl.expected_app_crc32);
        target->status = OTA_APP_STATUS_INVALID;
        target->verify_count = 0;
        rc = record_store(&rec);
        if (rc != 0) return rc;
        memclr(&g_dl, sizeof(g_dl));
        return OTA_MODULE_ERR_CRC_MISMATCH;
    }

    /* CRC OK: mark as UNVERIFIED for next boot verification. */
    target->status = OTA_APP_STATUS_UNVERIFIED;
    target->verify_count = 0;
    rc = record_store(&rec);
    if (rc != 0) return rc;

    memclr(&g_dl, sizeof(g_dl));
    return OTA_MODULE_ERR_OK;
}

int ota_module_ota_download_abort(void)
{
    if (!g_inited) return OTA_MODULE_ERR_NOT_INIT;

    ota_record_info_t rec;
    int rc = record_load(&rec);
    if (rc != 0) {
        /* record_load already filled default; still allow marking invalid for safety. */
        OTA_LOGW("ota_download_abort: record_load rc=%d", rc);
    }

    int32_t target_idx = -1;

    if (g_dl.started) {
        target_idx = (int32_t)g_dl.target_index;
    } else {
        /* Find a DOWNLOADING slot to abort. */
        const uint32_t count = (uint32_t)(OTA_IS_DUAL_APP ? 2u : 1u);
        for (uint32_t i = 0; i < count; i++) {
            if (rec.ota_app_info[i].status == OTA_APP_STATUS_DOWNLOADING) {
                target_idx = (int32_t)i;
                break;
            }
        }
    }

    if (target_idx < 0) {
        OTA_LOGE("ota_download_abort: no downloading slot found");
        return OTA_MODULE_ERR_SLOT_UNAVAILABLE;
    }

    ota_app_info_t *target = &rec.ota_app_info[(uint32_t)target_idx];
    target->status = OTA_APP_STATUS_INVALID; /* your constraint: abort -> invalid/non-bootable */
    target->verify_count = 0;
    rc = record_store(&rec);
    if (rc != 0) return rc;

    memclr(&g_dl, sizeof(g_dl));
    OTA_LOGE("ota_download_abort: target_idx=%lu", (unsigned long)(uint32_t)target_idx);
    return OTA_MODULE_ERR_OK;
}

/* Execute-in-place: promote first UNVERIFIED OTA slot to ACTIVE and demote others.
 * Returns: 0 = record should be stored; 1 = idempotent (no change); -1 = error */
static int mark_verified_promote_unverified_ota_slot(ota_record_info_t *rec)
{
    const uint32_t count = (uint32_t)(OTA_IS_DUAL_APP ? 2u : 1u);
    uint32_t uv = count;
    for (uint32_t i = 0; i < count; i++) {
        if (rec->ota_app_info[i].status == OTA_APP_STATUS_UNVERIFIED) {
            uv = i;
            break;
        }
    }
    if (uv >= count) {
        for (uint32_t i = 0; i < count; i++) {
            if (rec->ota_app_info[i].status == OTA_APP_STATUS_ACTIVE) {
                OTA_LOGD("ota_app_mark_verified: already ACTIVE (OTA slot)");
                return 1;
            }
        }
        OTA_LOGE("ota_app_mark_verified: no UNVERIFIED ota slot");
        return -1;
    }

    rec->ota_app_info[uv].status = OTA_APP_STATUS_ACTIVE;
    rec->ota_app_info[uv].verify_count = 0;

    for (uint32_t i = 0; i < count; i++) {
        if (i == uv) {
            continue;
        }
        if (rec->ota_app_info[i].status == OTA_APP_STATUS_ACTIVE ||
            rec->ota_app_info[i].status == OTA_APP_STATUS_UNVERIFIED) {
            rec->ota_app_info[i].status = OTA_APP_STATUS_BACKUP;
            rec->ota_app_info[i].verify_count = 0;
        }
    }

#if OTA_IS_HAVE_FACTORY_APP
    if (rec->factory_app_info.status == OTA_APP_STATUS_ACTIVE ||
        rec->factory_app_info.status == OTA_APP_STATUS_UNVERIFIED) {
        rec->factory_app_info.status = OTA_APP_STATUS_BACKUP;
        rec->factory_app_info.verify_count = 0;
    }
#endif
    return 0;
}

int ota_module_app_mark_verified(void)
{
    if (!g_inited) return OTA_MODULE_ERR_NOT_INIT;

    ota_record_info_t rec;
    int rc = record_load(&rec);
    if (rc != 0) return rc;

    int mv = mark_verified_promote_unverified_ota_slot(&rec);
    if (mv == 1) {
        return OTA_MODULE_ERR_OK;
    }
    if (mv < 0) {
        return OTA_MODULE_ERR_APP_STATUS;
    }

    if (record_store(&rec) != 0) {
        OTA_LOGE("ota_app_mark_verified: record_store failed");
        return OTA_MODULE_ERR_FLASH_OP;
    }
    OTA_LOGI("ota_app_mark_verified: ok");
    return OTA_MODULE_ERR_OK;
}

int ota_module_set_boot_ymodem_flag(uint32_t flag)
{
    if (!g_inited) return OTA_MODULE_ERR_NOT_INIT;

    ota_record_info_t rec;
    int rc = record_load(&rec);
    if (rc != 0) {
        /* record_load already filled default; still allow update. */
        OTA_LOGW("ota_set_boot_ymodem_flag: record_load rc=%d", rc);
    }

    rec.boot_ymodem_flag = flag;
    rc = record_store(&rec);
    if (rc != 0) return rc;
    return OTA_MODULE_ERR_OK;
}

uint32_t ota_module_get_boot_ymodem_flag(void)
{
    if (!g_inited) return 0u;

    ota_record_info_t rec;
    int rc = record_load(&rec);
    if (rc != 0) {
        /* record_load already filled default; treat as flag=0. */
        OTA_LOGW("ota_get_boot_ymodem_flag: record_load rc=%d", rc);
    }
    return rec.boot_ymodem_flag;
}

