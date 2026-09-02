#pragma once

/**
 * exif.h - Minimal JPEG EXIF parser for metadata panel.
 *
 * Parses only the tags needed for the info overlay and does not aim to be a
 * full EXIF implementation. PNG/WebP/BMP files correctly report has_exif=false
 * and the viewer falls back to stat-based file info.
 *
 * Supported tags (when present in JPEG APP1/Exif):
 *   - Orientation, Make, Model, Software, DateTime, DateTimeOriginal
 *   - ISOSpeedRatings, ExposureTime, FNumber, FocalLength, Pixel dimensions
 * All string fields are NUL-terminated and truncated to fit.
 */

#include <stdbool.h>

typedef struct {
    int orientation;          // TIFF Orientation 1..8, default 1 (no rotation)
    char make[64];            // Camera make
    char model[64];           // Camera model
    char datetime[32];        // DateTimeOriginal preferred, else DateTime
    char software[64];        // Software
    int iso;                  // ISO value, 0 if absent
    char exposure[32];        // e.g., "1/500"
    char fnumber[32];         // e Татья., "f/2.8"
    char focal[32];           // "50 mm"
    int exif_width;           // PixelXDimension
    int exif_height;          // PixelYDimension
    bool has_exif;            // True if APP1/Exif was found and parsed
} ExifData;

/**
 * Read EXIF data from an image file.
 *
 * @param path Filesystem path (JPEG expected; other formats return has_exif=false).
 * @param out  Output struct, always zero-initialized on return. Orientation
 *             defaults to 1. Returns true if file was readable (even without EXIF).
 * @return true on file read success, false on I/O error.
 */
bool exif_read(const char *path, ExifData *out);
