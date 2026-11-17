/* Path join, audio-extension checks, and FFmpeg file: URL helpers.
 * No FFmpeg dependency — safe for host tests. */
#ifndef VITAVOX_PATHUTIL_H
#define VITAVOX_PATHUTIL_H
#include <stdbool.h>
#include <stddef.h>

/* Join a and b with a single separator. A trailing slash or a trailing colon
 * (Vita "ux0:") is not doubled. Returns the number of characters that would
 * have been written (snprintf). */
int path_join(char *dst, size_t n, const char *a, const char *b);

/* Last ".<ext>" of the filename (not earlier path components). "" if none.
 * A leading-dot name like ".m4b" is treated as an extension. */
const char *path_ext(const char *path);

/* True if the path, filename, or extension (".m4b", "m4b", "BOOK.M4B") is a
 * supported audiobook type. Case-insensitive. False for NULL/empty. */
bool path_is_audio(const char *name);

/* True if the path is a folder/cover image we can try to load (jpg/jpeg/png).
 * Case-insensitive. webp is recognized but may not be decodable. */
bool path_is_image(const char *name);

/* True for common sidecar files next to audio (cue, m3u, nfo, log, ...). */
bool path_is_sidecar(const char *name);

/* True if the folder name looks like a disc/part of one book: "CD1", "CD 2",
 * "Disc 1", "Disk 2", "DVD 1", "Part 1", "Digital CD 1". Case-insensitive.
 * Does not match series titles such as "Vol. 07" or "Book 3". */
bool path_is_disc_folder(const char *name);

/* FFmpeg demuxer short name to force from the extension ("flac", "mp3",
 * "mov", ...), or NULL if unknown. */
const char *path_demuxer_name(const char *path);

/* If path has a scheme colon (ux0:..., usb0:..., C:...), write "file:<path>"
 * into buf so FFmpeg does not treat the device as a protocol. Already-prefixed
 * and plain POSIX paths are copied as-is. Returns buf. */
const char *path_file_url(const char *path, char *buf, size_t n);

#endif
