#include "test.h"
#include "tagart.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 1x1 red JPEG (standard minimal SOF). */
static const uint8_t kJpeg[] = {
    0xFF,0xD8,0xFF,0xE0,0x00,0x10,'J','F','I','F',0x00,0x01,0x01,0x00,0x00,0x01,
    0x00,0x01,0x00,0x00,0xFF,0xDB,0x00,0x43,0x00,0x08,0x06,0x06,0x07,0x06,0x05,0x08,
    0x07,0x07,0x07,0x09,0x09,0x08,0x0A,0x0C,0x14,0x0D,0x0C,0x0B,0x0B,0x0C,0x19,0x12,
    0x13,0x0F,0x14,0x1D,0x1A,0x1F,0x1E,0x1D,0x1A,0x1C,0x1C,0x20,0x24,0x2E,0x27,0x20,
    0x22,0x2C,0x23,0x1C,0x1C,0x28,0x37,0x29,0x2C,0x30,0x31,0x34,0x34,0x34,0x1F,0x27,
    0x39,0x3D,0x38,0x32,0x3C,0x2E,0x33,0x34,0x32,0xFF,0xC0,0x00,0x0B,0x08,0x00,0x01,
    0x00,0x01,0x01,0x01,0x11,0x00,0xFF,0xC4,0x00,0x14,0x00,0x01,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,0xFF,0xC4,0x00,0x14,
    0x10,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0xFF,0xDA,0x00,0x08,0x01,0x01,0x00,0x00,0x3F,0x00,0x7F,0xFF,0xD9
};

static void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static void synchsafe(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 21) & 0x7f);
    p[1] = (uint8_t)((v >> 14) & 0x7f);
    p[2] = (uint8_t)((v >> 7) & 0x7f);
    p[3] = (uint8_t)(v & 0x7f);
}

TEST(id3_apic) {
    const char *path = "/tmp/vita-tagart-id3.mp3";
    /* APIC body: enc=0, "image/jpeg\0", type=3, desc="\0", jpeg */
    uint8_t body[64 + sizeof kJpeg];
    memset(body, 0, sizeof body);
    int p = 0;
    body[p++] = 0;
    memcpy(body + p, "image/jpeg", 11); p += 11; /* includes NUL */
    body[p++] = 3;
    body[p++] = 0; /* empty desc */
    memcpy(body + p, kJpeg, sizeof kJpeg);
    p += (int)sizeof kJpeg;
    int body_len = p;

    uint8_t hdr[10 + 10];
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, "ID3", 3);
    hdr[3] = 4; /* v2.4 */
    int tag = 10 + body_len; /* one frame header + body */
    synchsafe(hdr + 6, (uint32_t)tag);
    memcpy(hdr + 10, "APIC", 4);
    synchsafe(hdr + 14, (uint32_t)body_len);

    FILE *f = fopen(path, "wb");
    CHECK(f);
    fwrite(hdr, 1, 20, f);
    fwrite(body, 1, (size_t)body_len, f);
    /* dummy audio padding */
    uint8_t pad[16] = {0xFF, 0xFB};
    fwrite(pad, 1, sizeof pad, f);
    fclose(f);

    uint8_t *art = NULL;
    size_t n = 0;
    CHECK(tagart_extract(path, &art, &n) == 0);
    CHECK(art);
    CHECK(n == sizeof kJpeg);
    CHECK(art[0] == 0xFF && art[1] == 0xD8);
    free(art);
    unlink(path);
}

TEST(mp4_covr) {
    const char *path = "/tmp/vita-tagart-mp4.m4b";
    /* ftyp + moov/udta/meta/ilst/covr/data + mdat (dummy) */
    /* We'll build innermost first and wrap. */
    /* data box: size + 'data' + 8 bytes type/locale + jpeg */
    uint32_t data_sz = 8 + 8 + (uint32_t)sizeof kJpeg;
    uint32_t covr_sz = 8 + data_sz;
    uint32_t ilst_sz = 8 + covr_sz;
    uint32_t meta_sz = 8 + 4 + ilst_sz; /* version/flags */
    uint32_t udta_sz = 8 + meta_sz;
    uint32_t moov_sz = 8 + udta_sz;
    uint32_t ftyp_sz = 24;
    uint32_t mdat_sz = 16;

    size_t total = ftyp_sz + moov_sz + mdat_sz;
    uint8_t *buf = calloc(1, total);
    CHECK(buf);
    size_t o = 0;
    be32(buf + o, ftyp_sz); memcpy(buf + o + 4, "ftyp", 4);
    memcpy(buf + o + 8, "M4B ", 4);
    be32(buf + o + 12, 0); memcpy(buf + o + 16, "M4B ", 4); memcpy(buf + o + 20, "mp42", 4);
    o += ftyp_sz;

    be32(buf + o, moov_sz); memcpy(buf + o + 4, "moov", 4); o += 8;
    be32(buf + o, udta_sz); memcpy(buf + o + 4, "udta", 4); o += 8;
    be32(buf + o, meta_sz); memcpy(buf + o + 4, "meta", 4); o += 8;
    be32(buf + o, 0); o += 4; /* version/flags */
    be32(buf + o, ilst_sz); memcpy(buf + o + 4, "ilst", 4); o += 8;
    be32(buf + o, covr_sz); memcpy(buf + o + 4, "covr", 4); o += 8;
    be32(buf + o, data_sz); memcpy(buf + o + 4, "data", 4); o += 8;
    be32(buf + o, 0x0000000D); o += 4; /* JPEG type in data box flags */
    be32(buf + o, 0); o += 4;
    memcpy(buf + o, kJpeg, sizeof kJpeg); o += sizeof kJpeg;

    be32(buf + o, mdat_sz); memcpy(buf + o + 4, "mdat", 4); o += 8;
    memset(buf + o, 0xAA, 8); o += 8;
    CHECK_EQ_INT((int)o, (int)total);

    FILE *f = fopen(path, "wb");
    CHECK(f);
    fwrite(buf, 1, total, f);
    fclose(f);
    free(buf);

    int64_t moff = 0, msz = 0;
    CHECK(mp4_locate_box(path, "moov", &moff, &msz) == 0);
    CHECK(moff == (int64_t)ftyp_sz);
    CHECK(msz == (int64_t)moov_sz);

    uint8_t *art = NULL;
    size_t n = 0;
    CHECK(tagart_extract(path, &art, &n) == 0);
    CHECK(art);
    CHECK_EQ_INT((int)n, (int)sizeof kJpeg);
    CHECK(art[0] == 0xFF && art[1] == 0xD8);
    free(art);
    unlink(path);
}

TEST(no_art) {
    const char *path = "/tmp/vita-tagart-empty.bin";
    FILE *f = fopen(path, "wb");
    fwrite("not a media file!!!!", 1, 20, f);
    fclose(f);
    uint8_t *art = NULL;
    size_t n = 0;
    CHECK(tagart_extract(path, &art, &n) != 0);
    CHECK(art == NULL);
    unlink(path);
}

void suite_tagart(void) {
    RUN(id3_apic);
    RUN(mp4_covr);
    RUN(no_art);
}
