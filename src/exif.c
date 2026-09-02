/**
 * exif.c - Minimal EXIF parser implementation.
 *
 * Strategy:
 *   1. Read the first 128KB of the file (EXIF is always near the start for JPEG).
 *   2. Verify SOI (0xFFD8) and iterate JPEG markers to find APP1 (0xFFE1) with
 *      "Exif\0\0" header.
 *   3. Parse TIFF structure inside APP1: header (endianness, magic 0x2A, IFD offset),
 *      then IFD0 and Exif Sub-IFD via pointer tags. Values that fit in 4 bytes
 *      are inline; longer strings/rationals are offset-relative to TIFF start.
 *   4. Helpers handle both Intel (little) and Motorola (big) endianness.
 *
 * Limitations: Does not handle maker notes, GPS IFD, or thumbnail IFD. Rationals
 * are formatted as human-readable strings (e.g., "1/500", "f/2.8"). Truncation
 * is applied safely for display purposes.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "exif.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

// TIFF tag IDs we care about
#define TAG_ORIENTATION          0x0112
#define TAG_MAKE                 0x010F
#define TAG_MODEL                0x0110
#define TAG_SOFTWARE             0x0131
#define TAG_DATETIME             0x0132
#define TAG_EXIF_IFD_POINTER     0x8769
#define TAG_EXIF_DATETIME_ORIG   0x9003
#define TAG_EXIF_ISO             0x8827
#define TAG_EXIF_EXPOSURE_TIME   0x829A
#define TAG_EXIF_FNUMBER         0x829D
#define TAG_EXIF_FOCAL_LENGTH    0x920A
#define TAG_EXIF_PIXEL_X         0xA002
#define TAG_EXIF_PIXEL_Y         0xA003

// TIFF data types
#define TYPE_BYTE      1
#define TYPE_ASCII     2
#define TYPE_SHORT     3
#define TYPE_LONG      4
#define TYPE_RATIONAL  5
#define TYPE_UNDEFINED 7
#define TYPE_SLONG     9
#define TYPE_SRATIONAL 10

static uint16_t read_u16(const uint8_t *p, bool le) {
    if (le) return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    else    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static uint32_t read_u32(const uint8_t *p, bool le) {
    if (le) return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    else    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// Safely copy ASCII string from TIFF data area, handling offset vs inline.
static void copy_ascii(const uint8_t *tiff, size_t tiff_len, uint32_t count, uint32_t val_offset, bool le, char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    if (count == 0) return;

    const uint8_t *src = NULL;
    size_t src_len = 0;
    uint8_t inline_buf[4];

    if (count <= 4) {
        // Value is stored inline in the offset field itself.
        // Need to extract bytes from val_offset according to endianness.
        // For inline ASCII, the 4 bytes contain the string directly.
        if (le) {
            inline_buf[0] = (uint8_t)(val_offset & 0xFF);
            inline_buf[1] = (uint8_t)((val_offset >> 8) & 0xFF);
            inline_buf[2] = (uint8_t)((val_offset >> 16) & 0xFF);
            inline_buf[3] = (uint8_t)((val_offset >> 24) & 0xFF);
        } else {
            inline_buf[0] = (uint8_t)((val_offset >> 24) & 0xFF);
            inline_buf[1] = (uint8_t)((val_offset >> 16) & 0xFF);
            inline_buf[2] = (uint8_t)((val_offset >> 8) & 0xFF);
            inline_buf[3] = (uint8_t)(val_offset & 0xFF);
        }
        src = inline_buf;
        src_len = count;
    } else {
        if (val_offset < 8 || (size_t)val_offset > tiff_len || (size_t)count > tiff_len - (size_t)val_offset) return;
        src = tiff + val_offset;
        src_len = count;
    }
    size_t copy = src_len < dst_len ? src_len : dst_len - 1;
    size_t w = 0;
    for (size_t i = 0; i < copy; i++) {
        uint8_t c = src[i];
        if (c == '\0') break;
        if (c < 32 && c != '\t') c = ' ';
        dst[w++] = (char)c;
    }
    dst[w] = '\0';
    // Remove trailing spaces/newlines that some cameras emit
    while (w > 0 && (dst[w-1] == ' ' || dst[w-1] == '\n' || dst[w-1] == '\r' || dst[w-1] == '\t')) {
        dst[w-1] = '\0';
        w--;
    }
}

static void format_rational(const uint8_t *tiff, size_t tiff_len, uint32_t count, uint32_t val_offset, bool le, char *dst, size_t dst_len, bool as_fnumber) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    if (count < 1 || val_offset < 8 || (size_t)val_offset > tiff_len || 8 > tiff_len - (size_t)val_offset) return;
    uint32_t num = read_u32(tiff + val_offset, le);
    uint32_t den = read_u32(tiff + val_offset + 4, le);
    if (den == 0 || num == 0) return;
    double v = (double)num / (double)den;
    if (isnan(v) || isinf(v) || v <= 0.0) return;

    if (as_fnumber) {
        snprintf(dst, dst_len, "f/%.1f", v);
        // Trim trailing ".0"
        size_t l = strlen(dst);
        if (l >= 2 && dst[l-2] == '.' && dst[l-1] == '0') dst[l-2] = '\0';
    } else {
        // Exposure time: show as fraction if <1, else as decimal
        if (num == 1) {
            snprintf(dst, dst_len, "1/%u", den);
        } else if (den == 1) {
            snprintf(dst, dst_len, "%u", num);
        } else if (num < den) {
            snprintf(dst, dst_len, "%u/%u", num, den);
        } else {
            snprintf(dst, dst_len, "%.2f", v);
        }
    }
}

static void format_focal(const uint8_t *tiff, size_t tiff_len, uint32_t count, uint32_t val_offset, bool le, char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    if (count < 1 || val_offset < 8 || (size_t)val_offset > tiff_len || 8 > tiff_len - (size_t)val_offset) return;
    uint32_t num = read_u32(tiff + val_offset, le);
    uint32_t den = read_u32(tiff + val_offset + 4, le);
    if (den == 0 || num == 0) return;
    double v = (double)num / (double)den;
    if (isnan(v) || isinf(v) || v <= 0.0) return;
    snprintf(dst, dst_len, "%.0f mm", v);
}

// Parse a single IFD, filling ExifData. Returns pointer to next IFD offset (for chaining) or 0.
static uint32_t parse_ifd(const uint8_t *tiff, size_t tiff_len, uint32_t ifd_offset, bool le, ExifData *out, uint32_t *exif_ifd_offset) {
    if (ifd_offset < 8 || (size_t)ifd_offset > tiff_len || 2 > tiff_len - (size_t)ifd_offset) return 0;
    uint16_t num_entries = read_u16(tiff + ifd_offset, le);
    size_t entry_base = (size_t)ifd_offset + 2;
    if (entry_base > tiff_len || (size_t)num_entries > (tiff_len - entry_base) / 12) return 0;
    if (entry_base + (size_t)num_entries * 12 + 4 > tiff_len) return 0;

    for (int i = 0; i < num_entries; i++) {
        const uint8_t *e = tiff + entry_base + i * 12;
        uint16_t tag = read_u16(e, le);
        uint16_t type = read_u16(e + 2, le);
        uint32_t count = read_u32(e + 4, le);
        uint32_t val_offset = read_u32(e + 8, le);

        switch (tag) {
        case TAG_ORIENTATION:
            if (type == TYPE_SHORT && count == 1) {
                uint16_t v = read_u16(e + 8, le);
                if (v >= 1 && v <= 8) out->orientation = v;
            }
            break;
        case TAG_MAKE:
            if (type == TYPE_ASCII) copy_ascii(tiff, tiff_len, count, val_offset, le, out->make, sizeof(out->make));
            break;
        case TAG_MODEL:
            if (type == TYPE_ASCII) copy_ascii(tiff, tiff_len, count, val_offset, le, out->model, sizeof(out->model));
            break;
        case TAG_SOFTWARE:
            if (type == TYPE_ASCII) copy_ascii(tiff, tiff_len, count, val_offset, le, out->software, sizeof(out->software));
            break;
        case TAG_DATETIME:
            if (type == TYPE_ASCII) {
                // Only fill if DateTimeOriginal not already present
                if (out->datetime[0] == '\0') copy_ascii(tiff, tiff_len, count, val_offset, le, out->datetime, sizeof(out->datetime));
            }
            break;
        case TAG_EXIF_IFD_POINTER:
            if ((type == TYPE_LONG || type == TYPE_SHORT) && count == 1) {
                uint32_t ptr = (type == TYPE_LONG) ? val_offset : (uint32_t)read_u16(e + 8, le);
                if (ptr >= 8 && (size_t)ptr + 2 <= tiff_len) {
                    *exif_ifd_offset = ptr;
                }
            }
            break;
        default:
            break;
        }
    }
    uint32_t next_ifd = read_u32(tiff + entry_base + (size_t)num_entries * 12, le);
    return next_ifd;
}

static void parse_exif_ifd(const uint8_t *tiff, size_t tiff_len, uint32_t ifd_offset, bool le, ExifData *out) {
    if (ifd_offset < 8 || (size_t)ifd_offset > tiff_len || 2 > tiff_len - (size_t)ifd_offset) return;
    uint16_t num = read_u16(tiff + ifd_offset, le);
    size_t base = (size_t)ifd_offset + 2;
    if (base > tiff_len || (size_t)num > (tiff_len - base) / 12) return;
    if (base + (size_t)num * 12 > tiff_len) return;

    for (int i = 0; i < num; i++) {
        const uint8_t *e = tiff + base + i * 12;
        uint16_t tag = read_u16(e, le);
        uint16_t type = read_u16(e + 2, le);
        uint32_t count = read_u32(e + 4, le);
        uint32_t val = read_u32(e + 8, le);

        switch (tag) {
        case TAG_EXIF_DATETIME_ORIG:
            if (type == TYPE_ASCII) copy_ascii(tiff, tiff_len, count, val, le, out->datetime, sizeof(out->datetime));
            break;
        case TAG_EXIF_ISO:
            if (count >= 1) {
                if (type == TYPE_SHORT) {
                    out->iso = (int)read_u16(e + 8, le);
                } else if (type == TYPE_LONG) {
                    out->iso = (int)val;
                }
            }
            break;
        case TAG_EXIF_EXPOSURE_TIME:
            if (type == TYPE_RATIONAL || type == TYPE_SRATIONAL) {
                format_rational(tiff, tiff_len, count, val, le, out->exposure, sizeof(out->exposure), false);
            }
            break;
        case TAG_EXIF_FNUMBER:
            if (type == TYPE_RATIONAL || type == TYPE_SRATIONAL) {
                format_rational(tiff, tiff_len, count, val, le, out->fnumber, sizeof(out->fnumber), true);
            }
            break;
        case TAG_EXIF_FOCAL_LENGTH:
            if (type == TYPE_RATIONAL || type == TYPE_SRATIONAL) {
                format_focal(tiff, tiff_len, count, val, le, out->focal, sizeof(out->focal));
            }
            break;
        case TAG_EXIF_PIXEL_X:
            if (type == TYPE_LONG) out->exif_width = (int)val;
            else if (type == TYPE_SHORT) out->exif_width = (int)read_u16(e + 8, le);
            break;
        case TAG_EXIF_PIXEL_Y:
            if (type == TYPE_LONG) out->exif_height = (int)val;
            else if (type == TYPE_SHORT) out->exif_height = (int)read_u16(e + 8, le);
            break;
        default: break;
        }
    }
}

/**
 * Read and parse EXIF metadata from a JPEG image file.
 *
 * Strategy:
 *   1. Reads up to the first 128KB of the specified file (EXIF header is located
 *      near file start in JPEG format).
 *   2. Validates JPEG Start-Of-Image marker (0xFFD8) and scans markers for
 *      APP1 (0xFFE1) containing the "Exif\0\0" magic identifier.
 *   3. Parses the TIFF header to detect endianness (II / MM), verifies magic 42,
 *      and parses IFD0 and the Exif Sub-IFD with circular reference protection.
 *   4. Populates string fields with ASCII sanitization, extracts numeric values,
 *      and formats rationals (exposure, aperture, focal length).
 *
 * @param path Filesystem path to the image file.
 * @param out Output struct; always zero-initialized on entry with orientation
 *            defaulting to 1.
 * @return true if the file was opened and read successfully (even if no EXIF exists),
 *         false on I/O failure or permission error.
 */
bool exif_read(const char *path, ExifData *out) {
    if (!path || !out) return false;
    memset(out, 0, sizeof(*out));
    out->orientation = 1;

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    // Read first 128KB - sufficient for EXIF which is always near start
    size_t cap = 128 * 1024;
    uint8_t *buf = malloc(cap);
    if (!buf) { fclose(f); return false; }
    size_t len = fread(buf, 1, cap, f);
    fclose(f);
    if (len < 10) { free(buf); return true; } // Too small to contain EXIF, but file is readable

    // Check JPEG SOI
    if (buf[0] != 0xFF || buf[1] != 0xD8) { free(buf); return true; } // Not JPEG

    size_t pos = 2;
    uint8_t *tiff = NULL;
    size_t tiff_len = 0;
    bool found = false;

    while (pos + 4 <= len) {
        if (buf[pos] != 0xFF) break;
        uint8_t marker = buf[pos + 1];
        // Skip padding 0xFF bytes
        if (marker == 0xFF) { pos++; continue; }
        // 0x00 is stuffed byte, not a marker
        if (marker == 0x00) break;
        // Standalone markers without length: RST0-RST7 (D0-D7), TEM (01)
        if ((marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
            pos += 2;
            continue;
        }
        // EOI (D9) or SOS (DA) or extra SOI (D8) - stop segment scanning
        if (marker == 0xD8 || marker == 0xD9 || marker == 0xDA) break;

        uint16_t seg_len = (uint16_t)((buf[pos + 2] << 8) | buf[pos + 3]);
        if (seg_len < 2) break;
        if ((size_t)seg_len > len - (pos + 2)) break; // Truncated segment
        size_t seg_end = pos + 2 + (size_t)seg_len;

        if (marker == 0xE1) { // APP1
            if (seg_len >= 8 && memcmp(buf + pos + 4, "Exif\0\0", 6) == 0) {
                tiff = buf + pos + 10;
                tiff_len = seg_len - 8;
                found = true;
                break;
            }
        }
        pos = seg_end;
    }

    if (!found || !tiff || tiff_len < 14) { free(buf); return true; }

    // Parse TIFF header
    bool le = false;
    if (tiff[0] == 'I' && tiff[1] == 'I') le = true;
    else if (tiff[0] == 'M' && tiff[1] == 'M') le = false;
    else { free(buf); return true; }

    uint16_t magic = read_u16(tiff + 2, le);
    if (magic != 42) { free(buf); return true; }

    uint32_t ifd0_offset = read_u32(tiff + 4, le);
    if (ifd0_offset < 8 || ifd0_offset >= tiff_len) { free(buf); return true; }

    // Traverse IFD chain with circular reference protection
    #define MAX_IFD_CHAIN 16
    uint32_t visited[MAX_IFD_CHAIN];
    int visited_count = 0;
    uint32_t cur_ifd = ifd0_offset;
    uint32_t exif_ifd = 0;

    while (cur_ifd != 0 && visited_count < MAX_IFD_CHAIN) {
        if (cur_ifd < 8 || (size_t)cur_ifd + 6 > tiff_len) break;

        bool circular = false;
        for (int v = 0; v < visited_count; v++) {
            if (visited[v] == cur_ifd) {
                circular = true;
                break;
            }
        }
        if (circular) break;
        visited[visited_count++] = cur_ifd;

        uint32_t next = parse_ifd(tiff, tiff_len, cur_ifd, le, out, &exif_ifd);
        if (next == cur_ifd) break; // Direct self-loop
        cur_ifd = next;
    }

    // Parse Exif Sub-IFD if found and not circular
    if (exif_ifd >= 8 && (size_t)exif_ifd + 2 <= tiff_len) {
        bool circular = false;
        for (int v = 0; v < visited_count; v++) {
            if (visited[v] == exif_ifd) {
                circular = true;
                break;
            }
        }
        if (!circular) {
            parse_exif_ifd(tiff, tiff_len, exif_ifd, le, out);
        }
    }

    out->has_exif = (out->make[0] != '\0' || out->model[0] != '\0' ||
                     out->datetime[0] != '\0' || out->software[0] != '\0' ||
                     out->exposure[0] != '\0' || out->fnumber[0] != '\0' ||
                     out->focal[0] != '\0' || out->orientation != 1 ||
                     out->iso != 0 || out->exif_width != 0 || out->exif_height != 0);
    // Consider has_exif true only if at least one field was populated; orientation alone is not enough
    // but we keep it as parsed regardless.

    free(buf);
    return true;
}
