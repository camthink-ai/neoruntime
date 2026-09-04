/**
 * @file hal_factory_format.c
 * @brief Storage-agnostic CTFB v1 A/B factory-identity format engine.
 *
 * Implements the CRC, dual-slot selection and field encode/decode. All storage
 * access goes through @ref HalFactoryIo so the same code drives I2C EEPROM,
 * SPI flash, MTD, a raw file, or a test fixture.
 *
 * Bit-for-bit compatible with the platform `factory-eeprom.sh` tool.
 */
#include "common/hal_factory_format.h"

#include <stdio.h>
#include <string.h>

/* ---- Slot header field offsets ---- */
enum {
    K_OFF_MAGIC    = 0,
    K_OFF_VERSION  = 4,
    K_OFF_FLAGS    = 5,
    K_OFF_CRC      = 6,
    K_OFF_SEQ      = 8,
    K_OFF_RESERVED = 12, /* 4 bytes, 0xFF */
    K_OFF_MAC      = 16, /* 6 raw bytes */
    K_OFF_SN       = 24, /* 32 ASCII */
    K_OFF_PN       = 56, /* 16 ASCII */
    K_OFF_BATCH    = 72, /* 8 ASCII */
    K_OFF_HWREV    = 80, /* 8 ASCII */
    K_PAYLOAD_LEN  = 112, /* CRC region: bytes [16..128) */
};

enum {
    K_FLAG_PROGRAMMED = 0x1,
    K_FLAG_MAC_VALID  = 0x2,
};

static const uint8_t k_magic[4] = {'C', 'T', 'F', 'B'};

typedef struct {
    uint32_t off;
    uint32_t len;
    bool     mac;
} field_meta_t;

static bool field_meta(HalFactoryField field, field_meta_t *m)
{
    if (m == NULL) {
        return false;
    }
    switch (field) {
        case HAL_FACTORY_FIELD_MAC:   *m = (field_meta_t){K_OFF_MAC,   6,  true}; return true;
        case HAL_FACTORY_FIELD_SN:    *m = (field_meta_t){K_OFF_SN,   32, false}; return true;
        case HAL_FACTORY_FIELD_PN:    *m = (field_meta_t){K_OFF_PN,   16, false}; return true;
        case HAL_FACTORY_FIELD_BATCH: *m = (field_meta_t){K_OFF_BATCH, 8, false}; return true;
        case HAL_FACTORY_FIELD_HWREV: *m = (field_meta_t){K_OFF_HWREV, 8, false}; return true;
        default: return false;
    }
}

/* ---- Little-endian primitives (match the shell script od -tu1 parsing) ---- */
static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0]) | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

/* CRC16-CCITT-FALSE: init 0xFFFF, poly 0x1021. */
static uint16_t crc16_ccitt_false(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    size_t i;
    for (i = 0; i < len; i++) {
        int b;
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static bool slot_valid(const uint8_t s[HAL_FACTORY_SLOT_SIZE])
{
    if (memcmp(s, k_magic, 4) != 0) {
        return false;
    }
    if (s[K_OFF_VERSION] != 1) {
        return false;
    }
    return rd16(s + K_OFF_CRC) == crc16_ccitt_false(s + K_OFF_MAC, K_PAYLOAD_LEN);
}

static uint32_t slot_seq(const uint8_t s[HAL_FACTORY_SLOT_SIZE])
{
    return rd32(s + K_OFF_SEQ);
}

typedef struct {
    uint8_t  data[HAL_FACTORY_SLOT_SIZE];
    char     letter; /* 'A', 'B', or '\0' */
    uint32_t base;
    uint32_t seq;
} active_slot_t;

static int read_active(const HalFactoryIo *io, active_slot_t *out)
{
    uint8_t a[HAL_FACTORY_SLOT_SIZE];
    uint8_t b[HAL_FACTORY_SLOT_SIZE];
    int rca = io->read(io->user, HAL_FACTORY_SLOT_A_BASE, a, HAL_FACTORY_SLOT_SIZE);
    int rcb = io->read(io->user, HAL_FACTORY_SLOT_B_BASE, b, HAL_FACTORY_SLOT_SIZE);

    if (rca != 0 && rcb != 0) {
        return HAL_ERR_NOT_READY;
    }

    bool va = (rca == 0) && slot_valid(a);
    bool vb = (rcb == 0) && slot_valid(b);

    if (va && vb) {
        const uint8_t *pick;
        if (slot_seq(a) >= slot_seq(b)) {
            pick = a; out->letter = 'A'; out->base = HAL_FACTORY_SLOT_A_BASE;
        } else {
            pick = b; out->letter = 'B'; out->base = HAL_FACTORY_SLOT_B_BASE;
        }
        memcpy(out->data, pick, HAL_FACTORY_SLOT_SIZE);
        out->seq = slot_seq(pick);
        return 0;
    }
    if (va) {
        memcpy(out->data, a, HAL_FACTORY_SLOT_SIZE);
        out->letter = 'A'; out->base = HAL_FACTORY_SLOT_A_BASE; out->seq = slot_seq(a);
        return 0;
    }
    if (vb) {
        memcpy(out->data, b, HAL_FACTORY_SLOT_SIZE);
        out->letter = 'B'; out->base = HAL_FACTORY_SLOT_B_BASE; out->seq = slot_seq(b);
        return 0;
    }
    return HAL_ERR_NOT_FOUND;
}

static void decode_mac(const uint8_t raw[6], char out[18])
{
    (void)snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
                   raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
}

static void decode_ascii(const uint8_t *raw, size_t len, char *out, size_t out_size)
{
    size_t n = len;
    while (n > 0 && (raw[n - 1] == 0xFF || raw[n - 1] == 0x00)) {
        n--;
    }
    size_t copy = (n < out_size - 1) ? n : (out_size - 1);
    memcpy(out, raw, copy);
    out[copy] = '\0';
}

static bool parse_mac(const char *s, uint8_t out[6])
{
    int b[6] = {0};
    if (s == NULL || strlen(s) != 17) {
        return false;
    }
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        if (b[i] < 0 || b[i] > 255) {
            return false;
        }
        out[i] = (uint8_t)b[i];
    }
    return true;
}

int hal_factory_format_read_all(const HalFactoryIo *io, HalFactoryInfo *out)
{
    if (io == NULL || out == NULL) {
        return HAL_ERR_INVALID_ARG;
    }
    active_slot_t a;
    int rc = read_active(io, &a);
    memset(out, 0, sizeof(*out));

    if (rc == HAL_ERR_NOT_FOUND) {
        out->valid = false;
        out->active_slot = '\0';
        return HAL_ERR_NOT_FOUND;
    }
    if (rc != 0) {
        return rc;
    }

    out->valid = true;
    out->active_slot = a.letter;
    out->seq = a.seq;
    decode_mac(a.data + K_OFF_MAC, out->mac_address);
    decode_ascii(a.data + K_OFF_SN, 32, out->serial_number, sizeof(out->serial_number));
    decode_ascii(a.data + K_OFF_PN, 16, out->product_number, sizeof(out->product_number));
    decode_ascii(a.data + K_OFF_BATCH, 8, out->batch, sizeof(out->batch));
    decode_ascii(a.data + K_OFF_HWREV, 8, out->hardware_revision, sizeof(out->hardware_revision));
    return 0;
}

int hal_factory_format_get(const HalFactoryIo *io, HalFactoryField field,
                           char *value, uint32_t value_size)
{
    field_meta_t m;
    if (io == NULL || value == NULL || value_size == 0 || !field_meta(field, &m)) {
        return HAL_ERR_INVALID_ARG;
    }
    active_slot_t a;
    int rc = read_active(io, &a);
    if (rc != 0) {
        value[0] = '\0';
        return rc;
    }

    if (m.mac) {
        char buf[18];
        decode_mac(a.data + m.off, buf);
        (void)snprintf(value, value_size, "%s", buf);
    } else {
        decode_ascii(a.data + m.off, m.len, value, value_size);
    }
    return 0;
}

int hal_factory_format_set(const HalFactoryIo *io, HalFactoryField field, const char *value)
{
    field_meta_t m;
    if (io == NULL || value == NULL || !field_meta(field, &m)) {
        return HAL_ERR_INVALID_ARG;
    }

    uint8_t mac[6] = {0};
    const char *ascii_val = value;
    size_t ascii_len = strlen(value);

    if (m.mac) {
        if (!parse_mac(value, mac)) {
            return HAL_ERR_INVALID_ARG;
        }
    } else if (ascii_len > m.len) {
        return HAL_ERR_INVALID_SIZE;
    }

    active_slot_t a;
    uint8_t out[HAL_FACTORY_SLOT_SIZE];
    uint32_t target_base = 0;
    uint32_t seq = 0;

    int rc = read_active(io, &a);
    if (rc == HAL_ERR_NOT_FOUND) {
        /* First programming: write slot A as a blank record with seq = 1. */
        memset(out, 0xFF, HAL_FACTORY_SLOT_SIZE);
        target_base = HAL_FACTORY_SLOT_A_BASE;
        seq = 1;
    } else if (rc == 0) {
        /* Preserve all fields from the active slot, write the inactive one. */
        memcpy(out, a.data, HAL_FACTORY_SLOT_SIZE);
        target_base = (a.letter == 'A') ? HAL_FACTORY_SLOT_B_BASE : HAL_FACTORY_SLOT_A_BASE;
        seq = a.seq + 1;
    } else {
        return rc;
    }

    if (m.mac) {
        memcpy(out + m.off, mac, 6);
    } else {
        memcpy(out + m.off, ascii_val, ascii_len);
        memset(out + m.off + ascii_len, 0xFF, m.len - ascii_len);
    }

    /* Header: magic, version, flags, CRC, seq, reserved. */
    memcpy(out + K_OFF_MAGIC, k_magic, 4);
    out[K_OFF_VERSION] = 1;
    uint8_t flags = K_FLAG_PROGRAMMED;
    if (out[K_OFF_MAC] != 0xFF) {
        flags |= K_FLAG_MAC_VALID;
    }
    out[K_OFF_FLAGS] = flags;
    wr16(out + K_OFF_CRC, crc16_ccitt_false(out + K_OFF_MAC, K_PAYLOAD_LEN));
    wr32(out + K_OFF_SEQ, seq);
    memset(out + K_OFF_RESERVED, 0xFF, 4);

    rc = io->write(io->user, target_base, out, HAL_FACTORY_SLOT_SIZE);
    if (rc != 0) {
        return rc;
    }

    /* Verify read-back (power-fail safety). */
    uint8_t ver[HAL_FACTORY_SLOT_SIZE];
    rc = io->read(io->user, target_base, ver, HAL_FACTORY_SLOT_SIZE);
    if (rc != 0) {
        return rc;
    }
    if (!slot_valid(ver)) {
        return HAL_ERR_CHECK;
    }
    return 0;
}

int hal_factory_format_erase(const HalFactoryIo *io)
{
    if (io == NULL) {
        return HAL_ERR_INVALID_ARG;
    }
    uint8_t ff[HAL_FACTORY_EEPROM_SIZE];
    memset(ff, 0xFF, sizeof(ff));
    return io->write(io->user, 0, ff, HAL_FACTORY_EEPROM_SIZE);
}
