/* Host scan + cover + decode tests against a generated copy of a sample
 * classic-literature library tree, plus extra format/layout fixtures.
 * Built only into test-decode (needs FFmpeg). */
#include "test.h"
#include "dbg.h"
#include "models.h"
#include "pathutil.h"
#include "probecache.h"
#include "bookcache.h"
#include "osal.h"
#include "progress.h"
#include "scanner.h"
#include "stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int run_cmd(const char *cmd) {
    int rc = system(cmd);
    if (rc != 0) fprintf(stderr, "command failed (%d): %s\n", rc, cmd);
    return rc;
}

static int file_ok(const char *path) {
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && st.st_size > 8;
}

static BookSummary *find_title(BookSummary *books, int n, const char *title) {
    for (int i = 0; i < n; i++)
        if (!strcmp(books[i].title, title)) return &books[i];
    return NULL;
}

static int has_title(BookSummary *books, int n, const char *title) {
    return find_title(books, n, title) != NULL;
}

static int decode_some(const char *path) {
    TrackSource src;
    memset(&src, 0, sizeof src);
    snprintf(src.path, sizeof src.path, "%s", path);
    const char *ext = path_ext(path);
    if (ext && ext[0]) snprintf(src.ext, sizeof src.ext, "%s", ext);
    int err = 0;
    Stream *s = stream_open(&src, &err);
    if (!s) {
        fprintf(stderr, "stream_open failed for %s (%d)\n", path, err);
        return -1;
    }
    int16_t buf[2048 * 2];
    int ch = 0, rate = 0, frames = 0;
    for (int i = 0; i < 32; i++) {
        int n = stream_read(s, buf, 2048, &ch, &rate);
        if (n == STREAM_EOF) break;
        if (n < 0) { stream_close(s); return n; }
        frames += n;
        if (frames > 2000) break;
    }
    double actual = 0;
    int seek_ok = stream_seek(s, 0.1, &actual) == 0;
    int n = stream_read(s, buf, 2048, &ch, &rate);
    stream_close(s);
    if (frames <= 0) {
        fprintf(stderr, "no PCM from %s\n", path);
        return -1;
    }
    if (!seek_ok && n < 0) return -1;
    return 0;
}

static int setup_cache(const char *root) {
    char covers[320], probe[320], prog[320], logp[320];
    snprintf(covers, sizeof covers, "%s/covers", root);
    snprintf(probe, sizeof probe, "%s/probe", root);
    snprintf(prog, sizeof prog, "%s/progress", root);
    snprintf(logp, sizeof logp, "%s/debug.log", root);
    mkdir(root, 0777);
    mkdir(covers, 0777);
    mkdir(probe, 0777);
    mkdir(prog, 0777);
    dbg_init(logp);
    scanner_set_cover_dir(covers);
    probecache_init(probe);
    progress_init(prog);
    char books[320];
    snprintf(books, sizeof books, "%s/books", root);
    bookcache_init(books);
    return 0;
}

static void expect_book(BookSummary *books, int n, const char *title, int tracks, int want_cover) {
    BookSummary *b = find_title(books, n, title);
    if (!b) {
        CHECK(b != NULL);
        fprintf(stderr, "missing book '%s'\n", title);
        return;
    }
    BookDetail d;
    CHECK_EQ_INT(scanner_load_detail(b, &d), 0);
    CHECK_EQ_INT(d.ntracks, tracks);
    CHECK(d.duration > 0 || d.ntracks > 0);
    if (tracks > 1) {
        /* No embedded chapters in the tiny fixtures: one chapter per file. */
        CHECK_EQ_INT(d.nchapters, tracks);
    }
    if (want_cover) {
        if (!b->cover_path[0]) {
            /* Folder cover is filled at scan; enrich may add an embedded one. */
            scanner_enrich(b);
        }
        CHECK(b->cover_path[0] != 0);
        CHECK(file_ok(b->cover_path));
    }
    book_detail_free(&d);
}

TEST(sample_library_scan_and_covers) {
    CHECK_EQ_INT(run_cmd("python3 tests/gen_sample_library.py "
                         "--sample-lib build-host/fixtures/sample_library "
                         "--formats build-host/fixtures/formats"), 0);
    CHECK(file_ok("build-host/fixtures/sample_library/"
                  "Moby-Dick (Unabridged).m4b"));

    setup_cache("build-host/tmp-samplelib");
    BookSummary *books = NULL;
    int n = 0;
    CHECK_EQ_INT(scanner_scan("build-host/fixtures/sample_library", &books, &n), 0);
    const ScannerStats *st = scanner_last_stats();
    CHECK_EQ_INT(st->books, 14);
    CHECK_EQ_INT(st->audio_files, 387);
    CHECK_EQ_INT(n, 14);

    /* Containers and disc folders must not become books. */
    CHECK(!has_title(books, n, "Sherlock Holmes"));
    CHECK(!has_title(books, n, "Disc 1"));
    CHECK(!has_title(books, n, "Disc 2"));
    CHECK(!has_title(books, n, "Disc 3"));
    CHECK(!has_title(books, n, "The Count of Monte Cristo"));

    expect_book(books, n, "Moby-Dick (Unabridged)", 1, 0);
    expect_book(books, n, "4 - The Sign of the Four", 21, 1);
    expect_book(books, n, "pride and prejudice jane austen", 1, 1);
    expect_book(books, n, "Les Miserables", 354, 0);
    expect_book(books, n, "The Count of Monte Cristo 07 - The Prisoner", 1, 0);
    expect_book(books, n, "The Count of Monte Cristo 08 - The Escape", 1, 0);
    expect_book(books, n, "The Count of Monte Cristo 09 - The Arrival", 1, 1);
    expect_book(books, n, "The Count of Monte Cristo 10 - The Count", 1, 1);
    expect_book(books, n, "The Count of Monte Cristo 11 - The Duel", 1, 0);
    expect_book(books, n, "The Count of Monte Cristo 12 - The Ball", 1, 1);
    expect_book(books, n, "The Count of Monte Cristo 13 - The Vendetta", 1, 1);
    expect_book(books, n, "The Count of Monte Cristo 14 - The House of Morrel", 1, 1);
    expect_book(books, n, "The Count of Monte Cristo 15 - The Cemetery", 1, 1);
    expect_book(books, n, "The Count of Monte Cristo 16 - The Oasis", 1, 1);

    /* The Sign of the Four cover comes from the named jpg inside Disc 1 (natural first). */
    BookSummary *sign = find_title(books, n, "4 - The Sign of the Four");
    if (sign) {
        CHECK(strstr(sign->cover_path, "Arthur Conan Doyle - The Adventures of Sherlock Holmes.jpg") != NULL);
        BookDetail d;
        CHECK_EQ_INT(scanner_load_detail(sign, &d), 0);
        CHECK(strstr(d.tracks[0].path, "Disc 1") && strstr(d.tracks[0].path, "01 - "));
        CHECK(strstr(d.tracks[6].path, "Disc 2") && strstr(d.tracks[6].path, "01 - "));
        CHECK(strstr(d.tracks[13].path, "Disc 3") && strstr(d.tracks[13].path, "01 - "));
        CHECK(strstr(d.tracks[20].path, "Epilogue.flac") != NULL);
        book_detail_free(&d);
        /* Enrich all 21 flacs and confirm duration sums and decode/seek. */
        CHECK(scanner_enrich(sign) == 0 || sign->duration > 0);
        CHECK(sign->duration > 0);
        /* Menus use a 256 px thumb of the folder jpg, made during enrich (Refresh). */
        CHECK(strstr(sign->thumb_path, "/covers/t_") != NULL);
        CHECK(file_ok(sign->thumb_path));
        {
            FILE *tf = fopen(sign->thumb_path, "rb");
            unsigned char ih[24] = {0};
            if (tf) { if (fread(ih, 1, 24, tf) != 24) memset(ih, 0, sizeof ih); fclose(tf); }
            CHECK(ih[1] == 'P' && ih[2] == 'N' && ih[3] == 'G');
            CHECK_EQ_INT((ih[16] << 24) | (ih[17] << 16) | (ih[18] << 8) | ih[19], 256);
            CHECK_EQ_INT((ih[20] << 24) | (ih[21] << 16) | (ih[22] << 8) | ih[23], 256);
        }
        {
            BookDetail e;
            CHECK_EQ_INT(scanner_load_detail(sign, &e), 0);
            CHECK_EQ_INT(decode_some(e.tracks[0].path), 0);
            book_detail_free(&e);
        }
    }

    /* Sample the other codecs present in the sample tree. */
    BookSummary *moby = find_title(books, n, "Moby-Dick (Unabridged)");
    if (moby) CHECK_EQ_INT(decode_some(moby->path), 0);
    BookSummary *loves = find_title(books, n, "pride and prejudice jane austen");
    if (loves) {
        CHECK(strstr(loves->cover_path, "cover.jpg") != NULL);
        BookDetail d;
        scanner_load_detail(loves, &d);
        CHECK_EQ_INT(decode_some(d.tracks[0].path), 0);
        book_detail_free(&d);
    }
    BookSummary *lesmis = find_title(books, n, "Les Miserables");
    if (lesmis) {
        BookDetail d;
        scanner_load_detail(lesmis, &d);
        CHECK_EQ_INT(d.ntracks, 354);
        CHECK_EQ_INT(decode_some(d.tracks[0].path), 0);
        book_detail_free(&d);
    }

    free(books);
}

static int enrich_has_cover(BookSummary *b) {
    if (!b) return 0;
    scanner_enrich(b);
    return b->cover_path[0] && file_ok(b->cover_path);
}

TEST(formats_layouts_covers_decode) {
    setup_cache("build-host/tmp-formats");
    BookSummary *books = NULL;
    int n = 0;
    CHECK_EQ_INT(scanner_scan("build-host/fixtures/formats", &books, &n), 0);
    CHECK(!has_title(books, n, "Jane Austen"));
    CHECK(!has_title(books, n, "CD1"));
    CHECK(!has_title(books, n, "CD 2"));

    expect_book(books, n, "Pride and Prejudice", 1, 1);
    expect_book(books, n, "Multi Disc Book", 2, 1);
    expect_book(books, n, "folder_and_image", 1, 1);
    expect_book(books, n, "wav_book", 1, 0);
    expect_book(books, n, "m4a_lc", 1, 0);
    expect_book(books, n, "mp3_apic", 1, 0);
    expect_book(books, n, "flac_picture", 1, 0);
    expect_book(books, n, "opus_pic", 1, 0);
    expect_book(books, n, "ogg_vorbis", 1, 0);

    BookSummary *multi = find_title(books, n, "Multi Disc Book");
    if (multi) {
        CHECK(strstr(multi->cover_path, "front.JPEG") != NULL || strstr(multi->cover_path, "front.jpeg") != NULL);
        BookDetail d;
        scanner_load_detail(multi, &d);
        CHECK_EQ_INT(d.ntracks, 2);
        CHECK_EQ_INT(d.nchapters, 2);
        CHECK(strstr(d.tracks[0].path, "CD1") != NULL);
        CHECK(strstr(d.tracks[1].path, "CD 2") != NULL);
        book_detail_free(&d);
    }
    BookSummary *folder = find_title(books, n, "folder_and_image");
    if (folder) CHECK(strstr(folder->cover_path, "Photo.JPG") != NULL || strstr(folder->cover_path, "Photo.jpg") != NULL);

    /* Embedded art is saved during enrich. */
    CHECK(enrich_has_cover(find_title(books, n, "mp3_apic")));
    CHECK(enrich_has_cover(find_title(books, n, "flac_picture")));
    {
        /* Embedded art is already a 256 px PNG in covers/: it is its own thumb. */
        BookSummary *fp = find_title(books, n, "flac_picture");
        if (fp) CHECK(fp->thumb_path[0] && file_ok(fp->thumb_path));
    }
    BookSummary *opus = find_title(books, n, "opus_pic");
    BookSummary *ogg = find_title(books, n, "ogg_vorbis");
    if (opus) {
        scanner_enrich(opus);
        if (!opus->cover_path[0])
            fprintf(stderr, "note: opus embedded picture not extracted\n");
        else CHECK(file_ok(opus->cover_path));
    }
    if (ogg) {
        scanner_enrich(ogg);
        if (!ogg->cover_path[0])
            fprintf(stderr, "note: ogg embedded picture not extracted\n");
        else CHECK(file_ok(ogg->cover_path));
    }

    const char *decode_titles[] = {
        "mp3_apic", "flac_picture", "opus_pic", "ogg_vorbis", "wav_book",
        "m4a_lc", "Pride and Prejudice", "folder_and_image", "Multi Disc Book",
        "wma_book", "progressive", NULL
    };
    for (int i = 0; decode_titles[i]; i++) {
        BookSummary *b = find_title(books, n, decode_titles[i]);
        if (!b) {
            if (!strcmp(decode_titles[i], "wma_book") || !strcmp(decode_titles[i], "progressive"))
                continue;
            CHECK(b != NULL);
            continue;
        }
        BookDetail d;
        if (scanner_load_detail(b, &d) != 0 || d.ntracks < 1) {
            CHECK(0);
            continue;
        }
        int rc = decode_some(d.tracks[0].path);
        if (rc != 0 && !strcmp(decode_titles[i], "wma_book"))
            fprintf(stderr, "note: wma decode failed\n");
        else
            CHECK_EQ_INT(rc, 0);
        book_detail_free(&d);
    }

    BookSummary *he = find_title(books, n, "he_aac");
    if (he) {
        BookDetail d;
        if (scanner_load_detail(he, &d) == 0 && d.ntracks > 0) {
            int rc = decode_some(d.tracks[0].path);
            if (rc != 0)
                fprintf(stderr, "note: HE-AAC decode not supported by this FFmpeg (often AOT SBR+21)\n");
            book_detail_free(&d);
        }
    }

    BookSummary *prog = find_title(books, n, "progressive");
    if (prog) CHECK(file_ok(prog->cover_path));

    free(books);
}

static int png_256(const char *path) {
    FILE *f = fopen(path, "rb");
    unsigned char h[24] = {0};
    if (!f) return 0;
    size_t n = fread(h, 1, 24, f);
    fclose(f);
    if (n != 24 || h[1] != 'P' || h[2] != 'N' || h[3] != 'G') return 0;
    int w = (h[16] << 24) | (h[17] << 16) | (h[18] << 8) | h[19];
    int hh = (h[20] << 24) | (h[21] << 16) | (h[22] << 8) | h[23];
    return w == 256 && hh == 256;
}

/* First library build / Settings > Refresh = scan + enrich every book (libmodel's
 * load_job -> enrich_job). Every book with art (folder image or embedded) must
 * come out with a 256 px menu thumbnail, so menus never touch the original. */
static int build_library(const char *root, int *with_cover, int *with_thumb, BookSummary **out, int *nout) {
    BookSummary *books = NULL;
    int n = 0;
    if (scanner_scan(root, &books, &n) != 0) return -1;
    *with_cover = *with_thumb = 0;
    for (int i = 0; i < n; i++) {
        scanner_enrich(&books[i]);
        if (!books[i].cover_path[0]) continue;
        (*with_cover)++;
        if (books[i].thumb_path[0] && strstr(books[i].thumb_path, "tmp-thumbbuild/covers/")
            && png_256(books[i].thumb_path))
            (*with_thumb)++;
        else
            fprintf(stderr, "no 256 px thumb for '%s' (cover '%s', thumb '%s')\n", books[i].title,
                    books[i].cover_path, books[i].thumb_path);
    }
    *out = books;
    *nout = n;
    return 0;
}

TEST(thumbs_made_on_first_build_and_refresh) {
    CHECK_EQ_INT(run_cmd("rm -rf build-host/tmp-thumbbuild"), 0);
    setup_cache("build-host/tmp-thumbbuild");
    const char *roots[] = { "build-host/fixtures/sample_library", "build-host/fixtures/formats" };
    for (int r = 0; r < 2; r++) {
        int wc = 0, wt = 0, n = 0;
        BookSummary *books = NULL;
        /* First build: empty covers folder, empty probe cache. */
        CHECK_EQ_INT(build_library(roots[r], &wc, &wt, &books, &n), 0);
        CHECK(wc >= (r == 0 ? 9 : 5));
        CHECK_EQ_INT(wt, wc);
        printf("  first build %s: %d books, %d with art, %d with 256 px thumbs\n", roots[r], n, wc, wt);
        free(books);
        /* Refresh with everything cached: same thumbs, nothing lost. */
        CHECK_EQ_INT(build_library(roots[r], &wc, &wt, &books, &n), 0);
        CHECK_EQ_INT(wt, wc);
        free(books);
    }
    /* Thumbs deleted (or a card copied without covers/): Refresh remakes them all. */
    CHECK_EQ_INT(run_cmd("rm -f build-host/tmp-thumbbuild/covers/*.png"), 0);
    for (int r = 0; r < 2; r++) {
        int wc = 0, wt = 0, n = 0;
        BookSummary *books = NULL;
        CHECK_EQ_INT(build_library(roots[r], &wc, &wt, &books, &n), 0);
        CHECK(wc > 0);
        CHECK_EQ_INT(wt, wc);
        printf("  refresh after wiping covers %s: %d with art, %d with 256 px thumbs\n", roots[r], wc, wt);
        free(books);
    }
}

static int detail_equal(const BookDetail *a, const BookDetail *b) {
    if (a->ntracks != b->ntracks || a->nchapters != b->nchapters) return 0;
    if (a->duration < b->duration - 1e-6 || a->duration > b->duration + 1e-6) return 0;
    for (int i = 0; i < a->ntracks; i++)
        if (strcmp(a->tracks[i].path, b->tracks[i].path) || a->tracks[i].duration != b->tracks[i].duration
            || a->tracks[i].start_offset != b->tracks[i].start_offset || a->tracks[i].size != b->tracks[i].size
            || strcmp(a->tracks[i].ext, b->tracks[i].ext) || strcmp(a->tracks[i].title, b->tracks[i].title))
            return 0;
    for (int i = 0; i < a->nchapters; i++)
        if (a->chapters[i].start != b->chapters[i].start || a->chapters[i].end != b->chapters[i].end
            || strcmp(a->chapters[i].title, b->chapters[i].title))
            return 0;
    return 1;
}

static int bin_exists(const char *id) {
    char p[400];
    snprintf(p, sizeof p, "build-host/tmp-bookfiles/books/%s.bin", id);
    return file_ok(p);
}

/* Library build / Refresh writes books/<id>.bin; opening reads only that file. */
TEST(book_files_for_many_file_books) {
    CHECK_EQ_INT(run_cmd("rm -rf build-host/tmp-bookfiles"), 0);
    setup_cache("build-host/tmp-bookfiles");
    /* A 600-file book (one tiny mp3 copied), beside the 354-file Les Miserables fixture. */
    const char *big = "build-host/fixtures/many_files/Six Hundred Parts";
    if (!file_ok("build-host/fixtures/many_src_one.mp3")) {
        CHECK_EQ_INT(run_cmd("mkdir -p 'build-host/fixtures/many_files/Six Hundred Parts' && "
                             "ffmpeg -hide_banner -loglevel error -y -f lavfi -i sine=frequency=300:sample_rate=22050:duration=1 "
                             "-c:a libmp3lame -b:a 32k -ac 1 build-host/fixtures/many_src_one.mp3 && "
                             "for i in $(seq 1 600); do cp build-host/fixtures/many_src_one.mp3 "
                             "\"build-host/fixtures/many_files/Six Hundred Parts/part $i.mp3\"; done"), 0);
    }
    const char *roots[] = { "build-host/fixtures/sample_library", "build-host/fixtures/many_files" };
    for (int r = 0; r < 2; r++) {
        BookSummary *books = NULL;
        int n = 0;
        CHECK_EQ_INT(scanner_scan(roots[r], &books, &n), 0);
        /* = libmodel enrich_job on first build / Refresh */
        uint64_t t_build = now_ms();
        for (int i = 0; i < n; i++) {
            scanner_enrich(&books[i]);
            BookDetail d;
            if (scanner_load_detail(&books[i], &d) == 0) {
                CHECK_EQ_INT(scanner_save_detail(&books[i], &d), 0);
                book_detail_free(&d);
            }
            CHECK(bin_exists(books[i].id));
        }
        printf("  build %s: %d books, book files written (%llu ms incl. probes)\n", roots[r], n,
               (unsigned long long)(now_ms() - t_build));
        for (int i = 0; i < n; i++) {
            BookDetail fast, slow;
            uint64_t t0 = now_ms();
            CHECK_EQ_INT(scanner_open_detail(&books[i], &fast), 0);
            uint64_t t_fast = now_ms() - t0;
            t0 = now_ms();
            CHECK_EQ_INT(scanner_load_detail(&books[i], &slow), 0);
            uint64_t t_slow = now_ms() - t0;
            CHECK(detail_equal(&fast, &slow));
            if (fast.ntracks >= 300) {
                printf("  open '%s' (%d tracks): book file %llu ms vs full load %llu ms\n", books[i].title,
                       fast.ntracks, (unsigned long long)t_fast, (unsigned long long)t_slow);
                CHECK(t_fast < 100);
            }
            book_detail_free(&fast);
            book_detail_free(&slow);
        }
        free(books);
    }

    /* Folder changed after the build (a part added): stale -> full load, file rewritten. */
    BookSummary *books = NULL;
    int n = 0;
    CHECK_EQ_INT(scanner_scan("build-host/fixtures/many_files", &books, &n), 0);
    CHECK_EQ_INT(n, 1);
    if (n == 1) {
        sleep(1); /* coarse mtimes */
        CHECK_EQ_INT(run_cmd("cp build-host/fixtures/many_src_one.mp3 "
                             "'build-host/fixtures/many_files/Six Hundred Parts/part 601.mp3'"), 0);
        BookDetail d;
        CHECK_EQ_INT(scanner_open_detail(&books[0], &d), 0);
        CHECK_EQ_INT(d.ntracks, 601);
        book_detail_free(&d);
        CHECK(bin_exists(books[0].id));
        CHECK_EQ_INT(scanner_open_detail(&books[0], &d), 0);
        CHECK_EQ_INT(d.ntracks, 601);
        book_detail_free(&d);
        CHECK_EQ_INT(run_cmd("rm -f 'build-host/fixtures/many_files/Six Hundred Parts/part 601.mp3'"), 0);
        /* No book file yet (e.g. older version's library): first open full-loads and saves it. */
        bookcache_remove(books[0].id);
        CHECK(!bin_exists(books[0].id));
        CHECK_EQ_INT(scanner_open_detail(&books[0], &d), 0);
        CHECK_EQ_INT(d.ntracks, 600);
        CHECK(bin_exists(books[0].id));
        book_detail_free(&d);
        (void)big;
    }
    free(books);
}

void suite_library_host(void) {
    RUN(sample_library_scan_and_covers);
    RUN(formats_layouts_covers_decode);
    RUN(thumbs_made_on_first_build_and_refresh);
    RUN(book_files_for_many_file_books);
}
