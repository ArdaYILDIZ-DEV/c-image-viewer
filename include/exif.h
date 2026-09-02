#ifndef EXIF_H
#define EXIF_H

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

/**
 * Parsed EXIF metadata container.
 *
 * All string fields are guaranteed NUL-terminated. Numeric fields default
 * to 0 (or 1 for orientation) when absent.
 */
typedef struct {
    int orientation;          // TIFF Orientation 1..8, default 1 (no rotation)
    char make[64];            // Camera make string
    char model[64];           // Camera model string
    char datetime[32];        // DateTimeOriginal preferred, else DateTime
    char software[64];        // Software name/version string
    int iso;                  // ISO sensitivity, 0 if absent
    char exposure[32];        // Exposure time string, e.g. "1/500" or "0.50"
    char fnumber[32];         // Aperture value string, e.g. "f/2.8"
    char focal[32];           // Focal length string, e.g. "50 mm"
    int exif_width;           // PixelXDimension from Exif Sub-IFD
    int exif_height;          // PixelYDimension from Exif Sub-IFD
    bool has_exif;            // True if APP1/Exif was found and parsed with valid tags
} ExifData;

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
bool exif_read(const char *path, ExifData *out);

#endif /* EXIF_H */
