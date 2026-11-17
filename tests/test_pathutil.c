#include "pathutil.h"
#include "test.h"

TEST(path_join_vita_and_slash) {
    char b[64];
    path_join(b, sizeof b, "ux0:audiobooks", "book.m4b");
    CHECK_STR(b, "ux0:audiobooks/book.m4b");
    path_join(b, sizeof b, "ux0:audiobooks/", "book.m4b");
    CHECK_STR(b, "ux0:audiobooks/book.m4b");
    path_join(b, sizeof b, "ux0:", "audiobooks");
    CHECK_STR(b, "ux0:audiobooks");
    path_join(b, sizeof b, "ux0:/audiobooks", "Book/ch1.mp3");
    CHECK_STR(b, "ux0:/audiobooks/Book/ch1.mp3");
    path_join(b, sizeof b, "/tmp/lib", "x.mp3");
    CHECK_STR(b, "/tmp/lib/x.mp3");
}

TEST(path_ext_and_is_audio) {
    CHECK_STR(path_ext("book.m4b"), ".m4b");
    CHECK_STR(path_ext("BOOK.M4B"), ".M4B");
    CHECK_STR(path_ext("ux0:audiobooks/a.b.c.Mp3"), ".Mp3");
    CHECK_STR(path_ext("noext"), "");
    CHECK(path_is_audio("book.m4b"));
    CHECK(path_is_audio("BOOK.M4B"));
    CHECK(path_is_audio("story.Mp3"));
    CHECK(path_is_audio("ux0:audiobooks/Title.M4B"));
    CHECK(path_is_audio(".m4b"));
    CHECK(path_is_audio("m4b"));
    CHECK(path_is_audio("MP3"));
    CHECK(path_is_audio("file.opus"));
    CHECK(!path_is_audio("cover.jpg"));
    CHECK(!path_is_audio("book.txt"));
    CHECK(!path_is_audio("README"));
    CHECK(!path_is_audio(""));
    CHECK(!path_is_audio(NULL));
}

TEST(path_file_url_prefixes_device) {
    char b[80];
    path_file_url("ux0:audiobooks/book.m4b", b, sizeof b);
    CHECK_STR(b, "file:ux0:audiobooks/book.m4b");
    path_file_url("uma0:/books/a.mp3", b, sizeof b);
    CHECK_STR(b, "file:uma0:/books/a.mp3");
    path_file_url("file:ux0:x.m4b", b, sizeof b);
    CHECK_STR(b, "file:ux0:x.m4b");
    path_file_url("/tmp/x.m4b", b, sizeof b);
    CHECK_STR(b, "/tmp/x.m4b");
    path_file_url("book.m4b", b, sizeof b);
    CHECK_STR(b, "book.m4b");
}


TEST(path_is_image_and_sidecar) {
    CHECK(path_is_image("cover.jpg"));
    CHECK(path_is_image("Cover.JPEG"));
    CHECK(path_is_image("front.PNG"));
    CHECK(path_is_image("art.webp"));
    CHECK(!path_is_image("book.mp3"));
    CHECK(!path_is_image(""));
    CHECK(path_is_sidecar("album.cue"));
    CHECK(path_is_sidecar("playlist.M3U"));
    CHECK(path_is_sidecar("info.nfo"));
    CHECK(path_is_sidecar("rip.log"));
    CHECK(!path_is_sidecar("book.flac"));
}

TEST(disc_folder_names) {
    CHECK(path_is_disc_folder("CD1"));
    CHECK(path_is_disc_folder("cd 1"));
    CHECK(path_is_disc_folder("CD 01"));
    CHECK(path_is_disc_folder("Disc 1"));
    CHECK(path_is_disc_folder("Disc 2"));
    CHECK(path_is_disc_folder("DISK 10"));
    CHECK(path_is_disc_folder("dvd-2"));
    CHECK(path_is_disc_folder("Part 1"));
    CHECK(path_is_disc_folder("pt.2"));
    CHECK(path_is_disc_folder("Digital CD 1"));
    CHECK(path_is_disc_folder("Disc 1 (US)"));
    CHECK(!path_is_disc_folder("4 - The Sign of the Four"));
    CHECK(!path_is_disc_folder("The Count of Monte Cristo 07 - The Prisoner"));
    CHECK(!path_is_disc_folder("Vol. 07"));
    CHECK(!path_is_disc_folder("Book 3"));
    CHECK(!path_is_disc_folder("discography"));
    CHECK(!path_is_disc_folder("CDs"));
    CHECK(!path_is_disc_folder(""));
    CHECK(!path_is_disc_folder(NULL));
}

TEST(demuxer_name_from_ext) {
    CHECK_STR(path_demuxer_name("track.FLAC"), "flac");
    CHECK_STR(path_demuxer_name("a.mp3"), "mp3");
    CHECK_STR(path_demuxer_name("book.m4b"), "mov");
    CHECK_STR(path_demuxer_name("song.opus"), "ogg");
    CHECK_STR(path_demuxer_name("x.wav"), "wav");
    CHECK(path_demuxer_name("cover.jpg") == NULL);
}

void suite_pathutil(void) {
    RUN(path_join_vita_and_slash);
    RUN(path_ext_and_is_audio);
    RUN(path_file_url_prefixes_device);
    RUN(path_is_image_and_sidecar);
    RUN(disc_folder_names);
    RUN(demuxer_name_from_ext);
}
