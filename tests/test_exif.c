#include "test_common.h"
#include "exif.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

static void write_file(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(data, 1, size, f);
        fclose(f);
    }
}

static void test_exif_null_inputs(void) {
    ExifData data;
    TEST_ASSERT(!exif_read(NULL, NULL));
    TEST_ASSERT(!exif_read(NULL, &data));
    TEST_ASSERT(!exif_read("/tmp/any", NULL));
}

static void test_exif_nonexistent_and_small(void) {
    ExifData data;
    TEST_ASSERT(!exif_read("/tmp/civ_test_nonexistent_12345.jpg", &data));

    const char *tmp_empty = "/tmp/civ_test_empty.jpg";
    write_file(tmp_empty, "", 0);
    TEST_ASSERT(exif_read(tmp_empty, &data));
    TEST_ASSERT(!data.has_exif);
    TEST_ASSERT_INT_EQ(data.orientation, 1);
    unlink(tmp_empty);

    const char *tmp_short = "/tmp/civ_test_short.jpg";
    uint8_t short_bytes[] = { 0xFF, 0xD8, 0xFF };
    write_file(tmp_short, short_bytes, sizeof(short_bytes));
    TEST_ASSERT(exif_read(tmp_short, &data));
    TEST_ASSERT(!data.has_exif);
    unlink(tmp_short);

    const char *tmp_nonjpeg = "/tmp/civ_test_nonjpeg.jpg";
    uint8_t nonjpeg_bytes[] = { 'N', 'O', 'T', 'J', 'P', 'E', 'G', '1', '2', '3', '4' };
    write_file(tmp_nonjpeg, nonjpeg_bytes, sizeof(nonjpeg_bytes));
    TEST_ASSERT(exif_read(tmp_nonjpeg, &data));
    TEST_ASSERT(!data.has_exif);
    unlink(tmp_nonjpeg);
}

static void test_exif_synthetic_le(void) {
    const char *tmp = "/tmp/civ_test_synth_le.jpg";
    uint8_t buf[1024];
    memset(buf, 0, sizeof(buf));

    // JPEG SOI
    buf[0] = 0xFF; buf[1] = 0xD8;
    // APP1 marker
    buf[2] = 0xFF; buf[3] = 0xE1;
    // Length: 400 bytes
    uint16_t seg_len = 400;
    buf[4] = (uint8_t)(seg_len >> 8);
    buf[5] = (uint8_t)(seg_len & 0xFF);
    // Exif header
    memcpy(buf + 6, "Exif\0\0", 6);

    // TIFF starts at buf + 12
    uint8_t *tiff = buf + 12;
    // Byte order: II (Little-Endian)
    tiff[0] = 'I'; tiff[1] = 'I';
    // Magic: 42
    tiff[2] = 0x2A; tiff[3] = 0x00;
    // IFD0 offset: 8
    tiff[4] = 0x08; tiff[5] = 0x00; tiff[6] = 0x00; tiff[7] = 0x00;

    // IFD0 at tiff + 8
    // Number of tags: 4
    tiff[8] = 0x04; tiff[9] = 0x00;

    // Tag 1: Orientation (0x0112), SHORT, count=1, value=6 (inline)
    size_t e0 = 10;
    tiff[e0 + 0] = 0x12; tiff[e0 + 1] = 0x01; // Tag
    tiff[e0 + 2] = 0x03; tiff[e0 + 3] = 0x00; // SHORT
    tiff[e0 + 4] = 0x01; tiff[e0 + 5] = 0x00; tiff[e0 + 6] = 0x00; tiff[e0 + 7] = 0x00; // count=1
    tiff[e0 + 8] = 0x06; tiff[e0 + 9] = 0x00; tiff[e0 + 10] = 0x00; tiff[e0 + 11] = 0x00; // value=6

    // Tag 2: Make (0x010F), ASCII, count=6 ("Canon\0"), offset=120
    size_t e1 = e0 + 12;
    tiff[e1 + 0] = 0x0F; tiff[e1 + 1] = 0x01;
    tiff[e1 + 2] = 0x02; tiff[e1 + 3] = 0x00; // ASCII
    tiff[e1 + 4] = 0x06; tiff[e1 + 5] = 0x00; tiff[e1 + 6] = 0x00; tiff[e1 + 7] = 0x00; // count=6
    tiff[e1 + 8] = 120;  tiff[e1 + 9] = 0;    tiff[e1 + 10] = 0;   tiff[e1 + 11] = 0;   // offset=120
    memcpy(tiff + 120, "Canon\0", 6);

    // Tag 3: Model (0x0110), ASCII, count=4 ("EOS\0"), inline in offset field
    size_t e2 = e1 + 12;
    tiff[e2 + 0] = 0x10; tiff[e2 + 1] = 0x01;
    tiff[e2 + 2] = 0x02; tiff[e2 + 3] = 0x00; // ASCII
    tiff[e2 + 4] = 0x04; tiff[e2 + 5] = 0x00; tiff[e2 + 6] = 0x00; tiff[e2 + 7] = 0x00; // count=4
    memcpy(tiff + e2 + 8, "EOS\0", 4); // inline

    // Tag 4: Exif IFD Pointer (0x8769), LONG, count=1, offset=150
    size_t e3 = e2 + 12;
    tiff[e3 + 0] = 0x69; tiff[e3 + 1] = 0x87;
    tiff[e3 + 2] = 0x04; tiff[e3 + 3] = 0x00; // LONG
    tiff[e3 + 4] = 0x01; tiff[e3 + 5] = 0x00; tiff[e3 + 6] = 0x00; tiff[e3 + 7] = 0x00;
    tiff[e3 + 8] = 150;  tiff[e3 + 9] = 0;    tiff[e3 + 10] = 0;   tiff[e3 + 11] = 0;

    // Next IFD = 0
    size_t next_ifd = e3 + 12;
    tiff[next_ifd] = 0; tiff[next_ifd + 1] = 0; tiff[next_ifd + 2] = 0; tiff[next_ifd + 3] = 0;

    // Exif Sub-IFD at offset 150
    // num_entries = 3
    tiff[150] = 0x03; tiff[151] = 0x00;
    // Sub-Tag 1: ISO (0x8827), SHORT, count=1, value=800
    size_t se0 = 152;
    tiff[se0 + 0] = 0x27; tiff[se0 + 1] = 0x88;
    tiff[se0 + 2] = 0x03; tiff[se0 + 3] = 0x00;
    tiff[se0 + 4] = 0x01; tiff[se0 + 5] = 0x00; tiff[se0 + 6] = 0x00; tiff[se0 + 7] = 0x00;
    tiff[se0 + 8] = (uint8_t)(800 & 0xFF); tiff[se0 + 9] = (uint8_t)(800 >> 8);

    // Sub-Tag 2: Exposure Time (0x829A), RATIONAL, count=1, offset=220
    size_t se1 = se0 + 12;
    tiff[se1 + 0] = 0x9A; tiff[se1 + 1] = 0x82;
    tiff[se1 + 2] = 0x05; tiff[se1 + 3] = 0x00; // RATIONAL
    tiff[se1 + 4] = 0x01; tiff[se1 + 5] = 0x00; tiff[se1 + 6] = 0x00; tiff[se1 + 7] = 0x00;
    tiff[se1 + 8] = 220;  tiff[se1 + 9] = 0;    tiff[se1 + 10] = 0;   tiff[se1 + 11] = 0;
    // 1 / 500 at 220
    uint32_t num = 1, den = 500;
    memcpy(tiff + 220, &num, 4);
    memcpy(tiff + 224, &den, 4);

    // Sub-Tag 3: PixelXDimension (0xA002), LONG, count=1, value=3840
    size_t se2 = se1 + 12;
    tiff[se2 + 0] = 0x02; tiff[se2 + 1] = 0xA0;
    tiff[se2 + 2] = 0x04; tiff[se2 + 3] = 0x00;
    tiff[se2 + 4] = 0x01; tiff[se2 + 5] = 0x00; tiff[se2 + 6] = 0x00; tiff[se2 + 7] = 0x00;
    uint32_t px = 3840;
    memcpy(tiff + se2 + 8, &px, 4);

    write_file(tmp, buf, 600);

    ExifData data;
    TEST_ASSERT(exif_read(tmp, &data));
    TEST_ASSERT(data.has_exif);
    TEST_ASSERT_INT_EQ(data.orientation, 6);
    TEST_ASSERT_STR_EQ(data.make, "Canon");
    TEST_ASSERT_STR_EQ(data.model, "EOS");
    TEST_ASSERT_INT_EQ(data.iso, 800);
    TEST_ASSERT_STR_EQ(data.exposure, "1/500");
    TEST_ASSERT_INT_EQ(data.exif_width, 3840);

    unlink(tmp);
}

static void test_exif_malicious_bounds(void) {
    const char *tmp = "/tmp/civ_test_malicious.jpg";
    uint8_t buf[512];
    memset(buf, 0, sizeof(buf));

    buf[0] = 0xFF; buf[1] = 0xD8;
    buf[2] = 0xFF; buf[3] = 0xE1;
    uint16_t seg_len = 200;
    buf[4] = (uint8_t)(seg_len >> 8); buf[5] = (uint8_t)(seg_len & 0xFF);
    memcpy(buf + 6, "Exif\0\0", 6);

    uint8_t *tiff = buf + 12;
    tiff[0] = 'I'; tiff[1] = 'I';
    tiff[2] = 0x2A; tiff[3] = 0x00;
    // IFD0 offset points to 8
    tiff[4] = 0x08;

    // IFD0: 2 tags
    tiff[8] = 0x02; tiff[9] = 0x00;

    // Tag 1: Make with huge offset that would overflow if not checked (0xFFFFFFF0)
    size_t e0 = 10;
    tiff[e0 + 0] = 0x0F; tiff[e0 + 1] = 0x01; // Make
    tiff[e0 + 2] = 0x02; tiff[e0 + 3] = 0x00; // ASCII
    // count = 20
    tiff[e0 + 4] = 0x14; tiff[e0 + 5] = 0x00; tiff[e0 + 6] = 0x00; tiff[e0 + 7] = 0x00;
    // offset = 0xFFFFFFF0
    tiff[e0 + 8] = 0xF0; tiff[e0 + 9] = 0xFF; tiff[e0 + 10] = 0xFF; tiff[e0 + 11] = 0xFF;

    // Tag 2: Rational with denominator = 0
    size_t e1 = e0 + 12;
    tiff[e1 + 0] = 0x9A; tiff[e1 + 1] = 0x82; // Exposure
    tiff[e1 + 2] = 0x05; tiff[e1 + 3] = 0x00; // Rational
    tiff[e1 + 4] = 0x01; tiff[e1 + 5] = 0x00; tiff[e1 + 6] = 0x00; tiff[e1 + 7] = 0x00;
    tiff[e1 + 8] = 80;   tiff[e1 + 9] = 0;    tiff[e1 + 10] = 0;   tiff[e1 + 11] = 0;
    // num = 1, den = 0 at offset 80
    uint32_t num = 1, den = 0;
    memcpy(tiff + 80, &num, 4);
    memcpy(tiff + 84, &den, 4);

    write_file(tmp, buf, sizeof(buf));

    ExifData data;
    // Must succeed reading without ASan crash, and invalid tags should be safely omitted
    TEST_ASSERT(exif_read(tmp, &data));
    TEST_ASSERT_STR_EQ(data.make, "");
    TEST_ASSERT_STR_EQ(data.exposure, "");

    unlink(tmp);
}

static void test_exif_synthetic_be(void) {
    const char *tmp = "/tmp/civ_test_synth_be.jpg";
    uint8_t buf[512];
    memset(buf, 0, sizeof(buf));

    // JPEG SOI
    buf[0] = 0xFF; buf[1] = 0xD8;
    // APP1 marker
    buf[2] = 0xFF; buf[3] = 0xE1;
    uint16_t seg_len = 300;
    buf[4] = (uint8_t)(seg_len >> 8); buf[5] = (uint8_t)(seg_len & 0xFF);
    memcpy(buf + 6, "Exif\0\0", 6);

    // TIFF starts at buf + 12 (Motorola big-endian)
    uint8_t *tiff = buf + 12;
    tiff[0] = 'M'; tiff[1] = 'M';
    // Magic: 42 in BE (0x002A)
    tiff[2] = 0x00; tiff[3] = 0x2A;
    // IFD0 offset: 8 in BE
    tiff[4] = 0x00; tiff[5] = 0x00; tiff[6] = 0x00; tiff[7] = 0x08;

    // IFD0 at tiff + 8
    // num_entries = 3
    tiff[8] = 0x00; tiff[9] = 0x03;

    // Tag 1: Orientation (0x0112), SHORT, count=1, value=3
    size_t e0 = 10;
    tiff[e0 + 0] = 0x01; tiff[e0 + 1] = 0x12; // Tag
    tiff[e0 + 2] = 0x00; tiff[e0 + 3] = 0x03; // SHORT
    tiff[e0 + 4] = 0x00; tiff[e0 + 5] = 0x00; tiff[e0 + 6] = 0x00; tiff[e0 + 7] = 0x01; // count=1
    tiff[e0 + 8] = 0x00; tiff[e0 + 9] = 0x03; tiff[e0 + 10] = 0x00; tiff[e0 + 11] = 0x00; // value=3

    // Tag 2: Software (0x0131), ASCII, count=7 ("Viewer\0"), offset=120
    size_t e1 = e0 + 12;
    tiff[e1 + 0] = 0x01; tiff[e1 + 1] = 0x31;
    tiff[e1 + 2] = 0x00; tiff[e1 + 3] = 0x02; // ASCII
    tiff[e1 + 4] = 0x00; tiff[e1 + 5] = 0x00; tiff[e1 + 6] = 0x00; tiff[e1 + 7] = 0x07;
    tiff[e1 + 8] = 0x00; tiff[e1 + 9] = 0x00; tiff[e1 + 10] = 0x00; tiff[e1 + 11] = 120;
    memcpy(tiff + 120, "Viewer\0", 7);

    // Tag 3: DateTime (0x0132), ASCII, count=4 ("2026"), inline
    size_t e2 = e1 + 12;
    tiff[e2 + 0] = 0x01; tiff[e2 + 1] = 0x32;
    tiff[e2 + 2] = 0x00; tiff[e2 + 3] = 0x02;
    tiff[e2 + 4] = 0x00; tiff[e2 + 5] = 0x00; tiff[e2 + 6] = 0x00; tiff[e2 + 7] = 0x04;
    memcpy(tiff + e2 + 8, "2026", 4);

    write_file(tmp, buf, 400);

    ExifData data;
    TEST_ASSERT(exif_read(tmp, &data));
    TEST_ASSERT(data.has_exif);
    TEST_ASSERT_INT_EQ(data.orientation, 3);
    TEST_ASSERT_STR_EQ(data.software, "Viewer");
    TEST_ASSERT_STR_EQ(data.datetime, "2026");

    unlink(tmp);
}

static void test_exif_corrupted_headers(void) {
    const char *tmp = "/tmp/civ_test_corrupt.jpg";
    uint8_t buf[256];
    memset(buf, 0, sizeof(buf));

    // Case 1: APP1 seg_len < 8
    buf[0] = 0xFF; buf[1] = 0xD8;
    buf[2] = 0xFF; buf[3] = 0xE1;
    buf[4] = 0x00; buf[5] = 0x05; // seg_len = 5
    write_file(tmp, buf, 64);
    ExifData data;
    TEST_ASSERT(exif_read(tmp, &data));
    TEST_ASSERT(!data.has_exif);

    // Case 2: Bad TIFF magic
    buf[4] = 0x00; buf[5] = 0x40; // seg_len = 64
    memcpy(buf + 6, "Exif\0\0", 6);
    buf[12] = 'I'; buf[13] = 'I';
    buf[14] = 0x99; buf[15] = 0x99; // Bad magic != 42
    write_file(tmp, buf, 64);
    TEST_ASSERT(exif_read(tmp, &data));
    TEST_ASSERT(!data.has_exif);

    // Case 3: IFD offset out of bounds
    buf[14] = 0x2A; buf[15] = 0x00; // Valid magic
    buf[16] = 0xFF; buf[17] = 0xFF; buf[18] = 0x00; buf[19] = 0x00; // ifd0 = 65535
    write_file(tmp, buf, 64);
    TEST_ASSERT(exif_read(tmp, &data));
    TEST_ASSERT(!data.has_exif);

    // Case 4: SOS marker before APP1 stops segment parsing
    memset(buf, 0, sizeof(buf));
    buf[0] = 0xFF; buf[1] = 0xD8;
    buf[2] = 0xFF; buf[3] = 0xDA; // SOS
    buf[4] = 0xFF; buf[5] = 0xE1; // APP1 after SOS should be ignored
    write_file(tmp, buf, 64);
    TEST_ASSERT(exif_read(tmp, &data));
    TEST_ASSERT(!data.has_exif);

    // Case 5: IFD offset < 8 (overlapping TIFF header)
    memset(buf, 0, sizeof(buf));
    buf[0] = 0xFF; buf[1] = 0xD8;
    buf[2] = 0xFF; buf[3] = 0xE1;
    buf[4] = 0x00; buf[5] = 0x40; // seg_len = 64
    memcpy(buf + 6, "Exif\0\0", 6);
    buf[12] = 'I'; buf[13] = 'I';
    buf[14] = 0x2A; buf[15] = 0x00;
    buf[16] = 0x04; buf[17] = 0x00; buf[18] = 0x00; buf[19] = 0x00; // ifd0 = 4 (< 8)
    write_file(tmp, buf, 64);
    TEST_ASSERT(exif_read(tmp, &data));
    TEST_ASSERT(!data.has_exif);

    // Case 6: Unexpected endianness marker (e.g. 'XX')
    buf[12] = 'X'; buf[13] = 'X';
    write_file(tmp, buf, 64);
    TEST_ASSERT(exif_read(tmp, &data));
    TEST_ASSERT(!data.has_exif);

    // Case 7: Truncated APP1 segment length (claims 400 bytes, file only 20 bytes)
    memset(buf, 0, sizeof(buf));
    buf[0] = 0xFF; buf[1] = 0xD8;
    buf[2] = 0xFF; buf[3] = 0xE1;
    buf[4] = 0x01; buf[5] = 0x90; // seg_len = 400
    memcpy(buf + 6, "Exif\0\0", 6);
    write_file(tmp, buf, 20);
    TEST_ASSERT(exif_read(tmp, &data));
    TEST_ASSERT(!data.has_exif);

    unlink(tmp);
}

static void test_exif_circular_ifd(void) {
    const char *tmp = "/tmp/civ_test_circular_ifd.jpg";
    uint8_t buf[512];
    memset(buf, 0, sizeof(buf));

    // Part A: Direct self-loop (IFD0 next_ifd points back to IFD0 at offset 8)
    buf[0] = 0xFF; buf[1] = 0xD8;
    buf[2] = 0xFF; buf[3] = 0xE1;
    uint16_t seg_len = 250;
    buf[4] = (uint8_t)(seg_len >> 8); buf[5] = (uint8_t)(seg_len & 0xFF);
    memcpy(buf + 6, "Exif\0\0", 6);

    uint8_t *tiff = buf + 12;
    tiff[0] = 'I'; tiff[1] = 'I';
    tiff[2] = 0x2A; tiff[3] = 0x00;
    tiff[4] = 0x08; tiff[5] = 0x00; tiff[6] = 0x00; tiff[7] = 0x00; // IFD0 = 8

    // IFD0 at 8: 1 tag (Orientation = 1)
    tiff[8] = 0x01; tiff[9] = 0x00;
    size_t e0 = 10;
    tiff[e0 + 0] = 0x12; tiff[e0 + 1] = 0x01;
    tiff[e0 + 2] = 0x03; tiff[e0 + 3] = 0x00;
    tiff[e0 + 4] = 0x01; tiff[e0 + 5] = 0x00; tiff[e0 + 6] = 0x00; tiff[e0 + 7] = 0x00;
    tiff[e0 + 8] = 0x01; tiff[e0 + 9] = 0x00; tiff[e0 + 10] = 0x00; tiff[e0 + 11] = 0x00;

    // next_ifd = 8 (points back to IFD0 itself -> infinite loop if unprotected)
    size_t next_ifd_pos = e0 + 12;
    tiff[next_ifd_pos + 0] = 0x08; tiff[next_ifd_pos + 1] = 0x00;
    tiff[next_ifd_pos + 2] = 0x00; tiff[next_ifd_pos + 3] = 0x00;

    write_file(tmp, buf, 300);

    ExifData data;
    // Must terminate cleanly without infinite loop or crash
    TEST_ASSERT(exif_read(tmp, &data));

    // Part B: 2-step circular loop (IFD0 points to IFD1, IFD1 points back to IFD0)
    // IFD0 next points to 50
    tiff[next_ifd_pos + 0] = 50;
    // IFD1 at offset 50: 1 tag, next points back to 8
    tiff[50] = 0x01; tiff[51] = 0x00;
    size_t e1 = 52;
    tiff[e1 + 0] = 0x0F; tiff[e1 + 1] = 0x01; // Make
    tiff[e1 + 2] = 0x02; tiff[e1 + 3] = 0x00; // ASCII
    tiff[e1 + 4] = 0x04; tiff[e1 + 5] = 0x00; tiff[e1 + 6] = 0x00; tiff[e1 + 7] = 0x00;
    memcpy(tiff + e1 + 8, "CIV\0", 4);
    // next of IFD1 at 52 + 12 = 64 points back to 8
    tiff[64] = 0x08; tiff[65] = 0x00; tiff[66] = 0x00; tiff[67] = 0x00;

    write_file(tmp, buf, 300);
    TEST_ASSERT(exif_read(tmp, &data));
    TEST_ASSERT_STR_EQ(data.make, "CIV");

    // Part C: SubIFD pointer points back to IFD0 (8)
    tiff[64] = 0x00; // terminate chain
    // Change tag 1 in IFD0 to TAG_EXIF_IFD_POINTER pointing to 8
    tiff[e0 + 0] = 0x69; tiff[e0 + 1] = 0x87;
    tiff[e0 + 2] = 0x04; tiff[e0 + 3] = 0x00; // LONG
    tiff[e0 + 8] = 0x08; tiff[e0 + 9] = 0x00; tiff[e0 + 10] = 0x00; tiff[e0 + 11] = 0x00;

    write_file(tmp, buf, 300);
    TEST_ASSERT(exif_read(tmp, &data));

    unlink(tmp);
}

static void test_exif_zero_denominators_and_nan(void) {
    const char *tmp = "/tmp/civ_test_zero_den.jpg";
    uint8_t buf[512];
    memset(buf, 0, sizeof(buf));

    buf[0] = 0xFF; buf[1] = 0xD8;
    buf[2] = 0xFF; buf[3] = 0xE1;
    uint16_t seg_len = 350;
    buf[4] = (uint8_t)(seg_len >> 8); buf[5] = (uint8_t)(seg_len & 0xFF);
    memcpy(buf + 6, "Exif\0\0", 6);

    uint8_t *tiff = buf + 12;
    tiff[0] = 'I'; tiff[1] = 'I';
    tiff[2] = 0x2A; tiff[3] = 0x00;
    tiff[4] = 0x08; // IFD0 at 8

    // IFD0: 1 tag (Exif IFD Pointer at offset 40)
    tiff[8] = 0x01; tiff[9] = 0x00;
    tiff[10] = 0x69; tiff[11] = 0x87;
    tiff[12] = 0x04; tiff[13] = 0x00;
    tiff[14] = 0x01; tiff[15] = 0x00; tiff[16] = 0x00; tiff[17] = 0x00;
    tiff[18] = 40;   tiff[19] = 0;    tiff[20] = 0;    tiff[21] = 0;
    // next_ifd = 0
    tiff[22] = 0; tiff[23] = 0; tiff[24] = 0; tiff[25] = 0;

    // SubIFD at offset 40: 5 tags
    // 1: Exposure time with den = 0
    // 2: FNumber with den = 0
    // 3: Focal length with den = 0
    // 4: Exposure time with num = 0
    // 5: Rational pointing to invalid offset < 8
    tiff[40] = 0x05; tiff[41] = 0x00;

    // Tag 1: Exposure (0x829A), RATIONAL, den=0 at offset 120
    size_t s0 = 42;
    tiff[s0 + 0] = 0x9A; tiff[s0 + 1] = 0x82;
    tiff[s0 + 2] = 0x05; tiff[s0 + 3] = 0x00;
    tiff[s0 + 4] = 0x01; tiff[s0 + 5] = 0x00; tiff[s0 + 6] = 0x00; tiff[s0 + 7] = 0x00;
    tiff[s0 + 8] = 120;  tiff[s0 + 9] = 0;    tiff[s0 + 10] = 0;   tiff[s0 + 11] = 0;
    uint32_t num1 = 100, den1 = 0;
    memcpy(tiff + 120, &num1, 4);
    memcpy(tiff + 124, &den1, 4);

    // Tag 2: FNumber (0x829D), RATIONAL, den=0 at offset 130
    size_t s1 = s0 + 12;
    tiff[s1 + 0] = 0x9D; tiff[s1 + 1] = 0x82;
    tiff[s1 + 2] = 0x05; tiff[s1 + 3] = 0x00;
    tiff[s1 + 4] = 0x01; tiff[s1 + 5] = 0x00; tiff[s1 + 6] = 0x00; tiff[s1 + 7] = 0x00;
    tiff[s1 + 8] = 130;  tiff[s1 + 9] = 0;    tiff[s1 + 10] = 0;   tiff[s1 + 11] = 0;
    uint32_t num2 = 28, den2 = 0;
    memcpy(tiff + 130, &num2, 4);
    memcpy(tiff + 134, &den2, 4);

    // Tag 3: FocalLength (0x920A), RATIONAL, den=0 at offset 140
    size_t s2 = s1 + 12;
    tiff[s2 + 0] = 0x0A; tiff[s2 + 1] = 0x92;
    tiff[s2 + 2] = 0x05; tiff[s2 + 3] = 0x00;
    tiff[s2 + 4] = 0x01; tiff[s2 + 5] = 0x00; tiff[s2 + 6] = 0x00; tiff[s2 + 7] = 0x00;
    tiff[s2 + 8] = 140;  tiff[s2 + 9] = 0;    tiff[s2 + 10] = 0;   tiff[s2 + 11] = 0;
    uint32_t num3 = 50, den3 = 0;
    memcpy(tiff + 140, &num3, 4);
    memcpy(tiff + 144, &den3, 4);

    // Tag 4: Rational with offset < 8 (pointing to 2)
    size_t s3 = s2 + 12;
    tiff[s3 + 0] = 0x9A; tiff[s3 + 1] = 0x82;
    tiff[s3 + 2] = 0x05; tiff[s3 + 3] = 0x00;
    tiff[s3 + 4] = 0x01; tiff[s3 + 5] = 0x00; tiff[s3 + 6] = 0x00; tiff[s3 + 7] = 0x00;
    tiff[s3 + 8] = 2;    tiff[s3 + 9] = 0;    tiff[s3 + 10] = 0;   tiff[s3 + 11] = 0; // offset < 8

    // Tag 5: Rational with offset > tiff_len
    size_t s4 = s3 + 12;
    tiff[s4 + 0] = 0x9D; tiff[s4 + 1] = 0x82;
    tiff[s4 + 2] = 0x05; tiff[s4 + 3] = 0x00;
    tiff[s4 + 4] = 0x01; tiff[s4 + 5] = 0x00; tiff[s4 + 6] = 0x00; tiff[s4 + 7] = 0x00;
    tiff[s4 + 8] = 0xFF; tiff[s4 + 9] = 0x01; tiff[s4 + 10] = 0;   tiff[s4 + 11] = 0; // 511 > tiff_len

    write_file(tmp, buf, sizeof(buf));

    ExifData data;
    TEST_ASSERT(exif_read(tmp, &data));
    // Verify no division by zero or crash, and corrupted fields were safely skipped
    TEST_ASSERT_STR_EQ(data.exposure, "");
    TEST_ASSERT_STR_EQ(data.fnumber, "");
    TEST_ASSERT_STR_EQ(data.focal, "");

    unlink(tmp);
}

static void test_exif_ascii_sanitization_and_garbage(void) {
    const char *tmp = "/tmp/civ_test_ascii_clean.jpg";
    uint8_t buf[512];
    memset(buf, 0, sizeof(buf));

    buf[0] = 0xFF; buf[1] = 0xD8;
    buf[2] = 0xFF; buf[3] = 0xE1;
    uint16_t seg_len = 200;
    buf[4] = (uint8_t)(seg_len >> 8); buf[5] = (uint8_t)(seg_len & 0xFF);
    memcpy(buf + 6, "Exif\0\0", 6);

    uint8_t *tiff = buf + 12;
    tiff[0] = 'I'; tiff[1] = 'I';
    tiff[2] = 0x2A; tiff[3] = 0x00;
    tiff[4] = 0x08;

    // IFD0: 2 tags
    tiff[8] = 0x02; tiff[9] = 0x00;

    // Tag 1: Make with control characters (e.g. "Ca\x01non\x1f\n\r")
    size_t e0 = 10;
    tiff[e0 + 0] = 0x0F; tiff[e0 + 1] = 0x01; // Make
    tiff[e0 + 2] = 0x02; tiff[e0 + 3] = 0x00; // ASCII
    tiff[e0 + 4] = 0x0A; tiff[e0 + 5] = 0x00; tiff[e0 + 6] = 0x00; tiff[e0 + 7] = 0x00; // count=10
    tiff[e0 + 8] = 80;   tiff[e0 + 9] = 0;    tiff[e0 + 10] = 0;   tiff[e0 + 11] = 0; // offset=80
    char dirty_make[] = "Ca\x01non\x1f \n\r";
    memcpy(tiff + 80, dirty_make, 10);

    // Tag 2: Model without NUL termination (count=5, "NIKON")
    size_t e1 = e0 + 12;
    tiff[e1 + 0] = 0x10; tiff[e1 + 1] = 0x01; // Model
    tiff[e1 + 2] = 0x02; tiff[e1 + 3] = 0x00; // ASCII
    tiff[e1 + 4] = 0x05; tiff[e1 + 5] = 0x00; tiff[e1 + 6] = 0x00; tiff[e1 + 7] = 0x00; // count=5
    tiff[e1 + 8] = 100;  tiff[e1 + 9] = 0;    tiff[e1 + 10] = 0;   tiff[e1 + 11] = 0; // offset=100
    memcpy(tiff + 100, "NIKON", 5);

    write_file(tmp, buf, sizeof(buf));

    ExifData data;
    TEST_ASSERT(exif_read(tmp, &data));
    // Control chars sanitized to space, trailing whitespace trimmed
    TEST_ASSERT_STR_EQ(data.make, "Ca non");
    // Non-NUL terminated string safely bounded
    TEST_ASSERT_STR_EQ(data.model, "NIKON");

    unlink(tmp);

    // Test with 32KB of random binary garbage
    const char *garbage_file = "/tmp/civ_test_garbage.jpg";
    uint8_t garbage[32768];
    for (size_t i = 0; i < sizeof(garbage); i++) garbage[i] = (uint8_t)(i * 37 + 13);
    write_file(garbage_file, garbage, sizeof(garbage));
    TEST_ASSERT(exif_read(garbage_file, &data));
    TEST_ASSERT(!data.has_exif);
    unlink(garbage_file);
}

static void test_exif_systematic_truncation_stress(void) {
    const char *tmp = "/tmp/civ_test_trunc_stress.jpg";
    uint8_t buf[512];
    memset(buf, 0, sizeof(buf));

    // Synthesize valid JPEG with full EXIF segment
    buf[0] = 0xFF; buf[1] = 0xD8; // SOI
    buf[2] = 0xFF; buf[3] = 0xE1; // APP1
    uint16_t seg_len = 200;
    buf[4] = (uint8_t)(seg_len >> 8);
    buf[5] = (uint8_t)(seg_len & 0xFF);
    memcpy(buf + 6, "Exif\0\0", 6);

    uint8_t *tiff = buf + 12;
    tiff[0] = 'I'; tiff[1] = 'I'; // Little-endian
    tiff[2] = 0x2A; tiff[3] = 0x00; // Magic 42
    tiff[4] = 0x08; tiff[5] = 0x00; tiff[6] = 0x00; tiff[7] = 0x00; // IFD0 offset: 8

    // IFD0 at tiff + 8: 2 tags
    tiff[8] = 0x02; tiff[9] = 0x00;
    // Tag 1: Orientation (0x0112) = 1
    size_t e0 = 10;
    tiff[e0 + 0] = 0x12; tiff[e0 + 1] = 0x01;
    tiff[e0 + 2] = 0x03; tiff[e0 + 3] = 0x00;
    tiff[e0 + 4] = 0x01; tiff[e0 + 5] = 0x00;
    tiff[e0 + 6] = 0x00; tiff[e0 + 7] = 0x00;
    tiff[e0 + 8] = 0x01; tiff[e0 + 9] = 0x00;
    tiff[e0 + 10] = 0x00; tiff[e0 + 11] = 0x00;

    // Tag 2: Exif Sub-IFD offset (0x8769) = 60
    size_t e1 = e0 + 12;
    tiff[e1 + 0] = 0x69; tiff[e1 + 1] = 0x87;
    tiff[e1 + 2] = 0x04; tiff[e1 + 3] = 0x00;
    tiff[e1 + 4] = 0x01; tiff[e1 + 5] = 0x00;
    tiff[e1 + 6] = 0x00; tiff[e1 + 7] = 0x00;
    tiff[e1 + 8] = 60;   tiff[e1 + 9] = 0x00;
    tiff[e1 + 10] = 0x00; tiff[e1 + 11] = 0x00;

    // Next IFD = 0
    tiff[e1 + 12] = 0x00; tiff[e1 + 13] = 0x00;
    tiff[e1 + 14] = 0x00; tiff[e1 + 15] = 0x00;

    // Sub-IFD at tiff + 60: 1 tag
    tiff[60] = 0x01; tiff[61] = 0x00;
    size_t se0 = 62;
    // Tag: ISO (0x8827) = 400
    tiff[se0 + 0] = 0x27; tiff[se0 + 1] = 0x88;
    tiff[se0 + 2] = 0x03; tiff[se0 + 3] = 0x00;
    tiff[se0 + 4] = 0x01; tiff[se0 + 5] = 0x00;
    tiff[se0 + 6] = 0x00; tiff[se0 + 7] = 0x00;
    tiff[se0 + 8] = 0x90; tiff[se0 + 9] = 0x01;
    tiff[se0 + 10] = 0x00; tiff[se0 + 11] = 0x00;

    size_t full_len = 12 + 80;

    // Truncate at EVERY single byte from 0 to full_len
    for (size_t trunc_len = 0; trunc_len <= full_len; trunc_len++) {
        write_file(tmp, buf, trunc_len);

        ExifData data;
        bool ok = exif_read(tmp, &data);
        TEST_ASSERT(ok); // File exists and is readable
        TEST_ASSERT_INT_EQ(data.orientation, 1); // Default orientation maintained
    }

    unlink(tmp);
}

void run_exif_tests(void) {
    printf("--- EXIF Test Suite ---\n");
    TEST_RUN(test_exif_null_inputs);
    TEST_RUN(test_exif_nonexistent_and_small);
    TEST_RUN(test_exif_synthetic_le);
    TEST_RUN(test_exif_synthetic_be);
    TEST_RUN(test_exif_malicious_bounds);
    TEST_RUN(test_exif_corrupted_headers);
    TEST_RUN(test_exif_circular_ifd);
    TEST_RUN(test_exif_zero_denominators_and_nan);
    TEST_RUN(test_exif_ascii_sanitization_and_garbage);
    TEST_RUN(test_exif_systematic_truncation_stress);
}
