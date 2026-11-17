/* Lightweight embedded-art extractors. These open the file and read only tag
 * headers / box headers + the picture payload. They never parse MP4 sample
 * tables or decode audio. Folder images should be tried first. */
#ifndef VITAVOX_TAGART_H
#define VITAVOX_TAGART_H
#include <stddef.h>
#include <stdint.h>

/* 0 if *out is malloc'd picture bytes (JPEG/PNG/etc.). -1 if none/error. */
int tagart_extract(const char *path, uint8_t **out, size_t *len);

/* Locate a top-level (or moov-descendant) MP4 box named typ (e.g. "moov").
 * Does not read mdat payload. 0 ok. */
int mp4_locate_box(const char *path, const char typ[4], int64_t *off, int64_t *size);

#endif
