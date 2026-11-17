#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "probecache.h"
#include "test.h"

TEST(probecache_roundtrip_and_stale) {
    const char *dir = "build-host/probe_cache";
    mkdir("build-host", 0777);
    mkdir(dir, 0777);
    probecache_init(dir);

    ProbeCacheEnt in, out;
    memset(&in, 0, sizeof in);
    strcpy(in.path, "ux0:audiobooks/Frankenstein (Unabridged).m4b");
    in.size = 123456789;
    in.mtime_ms = 1700000000000ULL;
    strcpy(in.title, "Frankenstein");
    strcpy(in.artist, "Author");
    in.duration = 36000.5;
    in.has_cover = 1;
    strcpy(in.cover_path, "ux0:data/VitaVox/covers/abc123.png");
    in.nchapters = 2;
    in.chapters = calloc(2, sizeof *in.chapters);
    CHECK(in.chapters);
    in.chapters[0].start = 0;
    in.chapters[0].end = 60;
    strcpy(in.chapters[0].title, "Intro");
    in.chapters[1].start = 60;
    in.chapters[1].end = 36000.5;
    strcpy(in.chapters[1].title, "Chapter 2");
    CHECK_EQ_INT(probecache_put(&in), 0);
    CHECK_EQ_INT(probecache_get(in.path, in.size, in.mtime_ms, &out), 0);
    CHECK_STR(out.title, "Frankenstein");
    CHECK_STR(out.artist, "Author");
    CHECK_NEAR(out.duration, 36000.5, 1e-6);
    CHECK_EQ_INT(out.has_cover, 1);
    CHECK_STR(out.cover_path, "ux0:data/VitaVox/covers/abc123.png");
    CHECK_EQ_INT(out.ver, PROBE_CACHE_VER);
    CHECK_EQ_INT(out.nchapters, 2);
    CHECK_STR(out.chapters[0].title, "Intro");
    CHECK_NEAR(out.chapters[1].start, 60, 1e-6);
    CHECK_STR(out.chapters[1].title, "Chapter 2");
    free(in.chapters);
    probecache_ent_free(&out);

    /* size or mtime mismatch is a miss */
    CHECK_EQ_INT(probecache_get(in.path, in.size + 1, in.mtime_ms, &out), -1);
    CHECK_EQ_INT(probecache_get(in.path, in.size, in.mtime_ms + 1, &out), -1);
    CHECK_EQ_INT(probecache_get("ux0:audiobooks/other.m4b", in.size, in.mtime_ms, &out), -1);
}

/* 00.38: description + tag version; older records read as tags 0. */
TEST(probecache_description) {
    const char *dir = "build-host/probe_cache";
    probecache_init(dir);
    ProbeCacheEnt in, out;
    memset(&in, 0, sizeof in);
    strcpy(in.path, "ux0:audiobooks/Les Miserables/0101 - Chapter 1.mp3");
    in.size = 1000;
    in.mtime_ms = 2000;
    in.duration = 12.5;
    in.ver = PROBE_CACHE_VER;
    in.tags = PROBE_TAGS_VER;
    in.description = "Line one\n\nPara \"two\" \xC3\xA9\xE2\x80\x94 end";
    CHECK_EQ_INT(probecache_put(&in), 0);
    CHECK_EQ_INT(probecache_get(in.path, in.size, in.mtime_ms, &out), 0);
    CHECK_EQ_INT(out.tags, PROBE_TAGS_VER);
    CHECK_STR(out.description, in.description);
    probecache_ent_free(&out);
    CHECK(out.description == NULL);
    /* no description: none stored, tags still recorded */
    in.description = NULL;
    CHECK_EQ_INT(probecache_put(&in), 0);
    CHECK_EQ_INT(probecache_get(in.path, in.size, in.mtime_ms, &out), 0);
    CHECK(out.description == NULL);
    CHECK_EQ_INT(out.tags, PROBE_TAGS_VER);
    probecache_ent_free(&out);
    /* a duration-only MP3 header record never claims tags */
    in.dur_only = 1;
    in.hdr = PROBE_HDR_EXACT;
    CHECK_EQ_INT(probecache_put(&in), 0);
    CHECK_EQ_INT(probecache_get(in.path, in.size, in.mtime_ms, &out), 0);
    CHECK_EQ_INT(out.tags, 0);
    probecache_ent_free(&out);
    /* a 00.37 record (no "tags" key) reads as tags 0 */
    FILE *f = NULL;
    char fp[300];
    {
        uint64_t h = 14695981039346656037ULL;
        for (const unsigned char *p = (const unsigned char *)in.path; *p; p++) { h ^= *p; h *= 1099511628211ULL; }
        snprintf(fp, sizeof fp, "%s/%016llx.json", dir, (unsigned long long)h);
    }
    f = fopen(fp, "wb");
    CHECK(f != NULL);
    if (f) {
        fprintf(f, "{\"v\":2,\"path\":\"%s\",\"size\":1000,\"mtime\":2000,\"title\":\"T\",\"duration\":12.5,"
                   "\"hasCover\":false,\"coverPath\":\"\",\"chapters\":[]}", in.path);
        fclose(f);
    }
    CHECK_EQ_INT(probecache_get(in.path, in.size, in.mtime_ms, &out), 0);
    CHECK_EQ_INT(out.tags, 0);
    CHECK(out.description == NULL);
    CHECK_NEAR(out.duration, 12.5, 1e-9);
    probecache_ent_free(&out);
}

void suite_probecache(void) {
    RUN(probecache_roundtrip_and_stale);
    RUN(probecache_description);
}
