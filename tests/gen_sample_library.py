#!/usr/bin/env python3
"""Build a tiny host copy of a sample classic-literature library tree, plus a
formats/ layout covering extra codecs and cover embeddings."""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "tests/fixtures/sample_library_manifest.txt"
FFMPEG = shutil.which("ffmpeg") or "ffmpeg"


def run(cmd: list[str]) -> None:
    r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if r.returncode != 0:
        sys.stderr.write(r.stderr.decode("utf-8", "replace"))
        raise SystemExit(f"command failed ({r.returncode}): {' '.join(cmd)}")


def make_jpeg(path: Path, color: str = "red") -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    run(
        [
            FFMPEG,
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-f",
            "lavfi",
            "-i",
            f"color=c={color}:s=32x32:d=1",
            "-frames:v",
            "1",
            "-q:v",
            "5",
            str(path),
        ]
    )


def make_audio(path: Path, dur: str = "0.4") -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    ext = path.suffix.lower()
    cmd = [
        FFMPEG,
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-f",
        "lavfi",
        "-i",
        f"sine=frequency=440:sample_rate=44100:duration={dur}",
    ]
    if ext in (".m4b", ".m4a", ".mp4"):
        cmd += ["-c:a", "aac", "-profile:a", "aac_low", "-b:a", "32k", "-f", "mp4", str(path)]
    elif ext == ".mp3":
        cmd += ["-c:a", "libmp3lame", "-b:a", "32k", str(path)]
    elif ext == ".flac":
        cmd += ["-c:a", "flac", str(path)]
    elif ext == ".wav":
        cmd += ["-c:a", "pcm_s16le", str(path)]
    elif ext == ".wma":
        cmd += ["-c:a", "wmav2", "-b:a", "32k", str(path)]
    elif ext == ".opus":
        cmd += ["-c:a", "libopus", "-b:a", "16k", str(path)]
    elif ext in (".ogg", ".oga"):
        cmd += ["-c:a", "libvorbis", "-q:a", "0", str(path)]
    else:
        raise SystemExit(f"unsupported audio ext {ext} for {path}")
    run(cmd)



def flac_picture_block(image_path: Path, pic_type: int = 3) -> bytes:
    data = image_path.read_bytes()
    ext = image_path.suffix.lower()
    mime = b"image/jpeg" if ext in (".jpg", ".jpeg") else b"image/png"
    desc = b""
    parts = [
        pic_type.to_bytes(4, "big"),
        len(mime).to_bytes(4, "big"),
        mime,
        len(desc).to_bytes(4, "big"),
        desc,
        (32).to_bytes(4, "big"),
        (32).to_bytes(4, "big"),
        (24).to_bytes(4, "big"),
        (0).to_bytes(4, "big"),
        len(data).to_bytes(4, "big"),
        data,
    ]
    return b"".join(parts)


def make_ogg_with_picture(path: Path, cover: Path, title: str, opus: bool) -> None:
    """Encode audio, then remux with a METADATA_BLOCK_PICTURE vorbis comment."""
    import base64
    import tempfile

    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    make_audio(tmp, dur="0.6")
    b64 = base64.b64encode(flac_picture_block(cover)).decode("ascii")
    # ffmetadata file avoids shell-escaping the huge base64 blob
    meta = path.with_suffix(".ffmeta")
    meta.write_text(
        ";FFMETADATA1\n"
        f"title={title}\n"
        "artist=Fixture\n"
        f"METADATA_BLOCK_PICTURE={b64}\n"
    )
    cmd = [
        FFMPEG,
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-i",
        str(tmp),
        "-i",
        str(meta),
        "-map_metadata",
        "1",
        "-c",
        "copy",
        str(path),
    ]
    try:
        run(cmd)
    finally:
        tmp.unlink(missing_ok=True)
        meta.unlink(missing_ok=True)

def make_audio_with_pic(path: Path, cover: Path, title: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    ext = path.suffix.lower()
    cmd = [
        FFMPEG,
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-f",
        "lavfi",
        "-i",
        "sine=frequency=440:sample_rate=44100:duration=0.6",
        "-i",
        str(cover),
        "-map",
        "0:a",
        "-map",
        "1:v",
        "-metadata",
        f"title={title}",
        "-metadata",
        "artist=Fixture",
        "-disposition:v",
        "attached_pic",
    ]
    if ext == ".mp3":
        cmd += ["-c:a", "libmp3lame", "-b:a", "32k", "-c:v", "mjpeg", "-id3v2_version", "3", str(path)]
    elif ext == ".flac":
        cmd += ["-c:a", "flac", "-c:v", "copy", str(path)]
    elif ext == ".opus":
        cmd += ["-c:a", "libopus", "-b:a", "16k", "-c:v", "copy", str(path)]
    elif ext in (".ogg", ".oga"):
        cmd += ["-c:a", "libvorbis", "-q:a", "0", "-c:v", "copy", str(path)]
    elif ext in (".m4b", ".m4a"):
        cmd += ["-c:a", "aac", "-profile:a", "aac_low", "-b:a", "32k", "-c:v", "mjpeg", "-f", "mp4", str(path)]
    else:
        raise SystemExit(f"no picture mux for {ext}")
    run(cmd)


def write_text(path: Path, body: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(body)


def build_sample_library(dest: Path) -> int:
    rels = [ln.strip() for ln in MANIFEST.read_text().splitlines() if ln.strip()]
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    tmpl_dir = dest / ".templates"
    tmpl_dir.mkdir()
    templates: dict[str, Path] = {}
    n_audio = n_img = n_side = 0
    for rel in rels:
        out = dest / rel
        ext = out.suffix.lower()
        if ext in (".m4b", ".m4a", ".mp3", ".flac", ".wav", ".wma", ".opus", ".ogg"):
            if ext not in templates:
                t = tmpl_dir / f"audio{ext}"
                make_audio(t)
                templates[ext] = t
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(templates[ext], out)
            n_audio += 1
        elif ext in (".jpg", ".jpeg", ".png"):
            if "jpg" not in templates:
                t = tmpl_dir / "cover.jpg"
                make_jpeg(t, "blue")
                templates["jpg"] = t
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(templates["jpg"], out)
            n_img += 1
        else:
            write_text(out, f"sidecar fixture for {rel}\n")
            n_side += 1
    shutil.rmtree(tmpl_dir, ignore_errors=True)
    print(f"sample library: {n_audio} audio, {n_img} images, {n_side} sidecars -> {dest}")
    return n_audio


def build_formats(dest: Path) -> dict[str, Path]:
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    cover = dest / "assets" / "cover.jpg"
    make_jpeg(cover, "green")
    out: dict[str, Path] = {}

    # Embedded covers
    make_audio_with_pic(dest / "mp3_apic" / "track.mp3", cover, "MP3 APIC")
    out["mp3_apic"] = dest / "mp3_apic"
    make_audio_with_pic(dest / "flac_picture" / "track.flac", cover, "FLAC Picture")
    out["flac_picture"] = dest / "flac_picture"
    try:
        make_ogg_with_picture(dest / "opus_pic" / "track.opus", cover, "Opus Picture", True)
        out["opus_pic"] = dest / "opus_pic"
    except SystemExit as e:
        print("note: opus picture fixture failed, encoding audio only:", e)
        make_audio(dest / "opus_pic" / "track.opus")
        out["opus_pic"] = dest / "opus_pic"
    try:
        make_ogg_with_picture(dest / "ogg_vorbis" / "track.ogg", cover, "Ogg Vorbis", False)
        out["ogg_vorbis"] = dest / "ogg_vorbis"
    except SystemExit as e:
        print("note: ogg picture fixture failed, encoding audio only:", e)
        make_audio(dest / "ogg_vorbis" / "track.ogg")
        out["ogg_vorbis"] = dest / "ogg_vorbis"

    # Plain formats
    make_audio(dest / "wav_book" / "ch1.wav")
    out["wav_book"] = dest / "wav_book"
    try:
        make_audio(dest / "wma_book" / "ch1.wma")
        out["wma_book"] = dest / "wma_book"
    except SystemExit as e:
        print("note: wma fixture skipped:", e)

    make_audio(dest / "m4a_lc" / "track.m4a")
    out["m4a_lc"] = dest / "m4a_lc"

    # Nested author/book + named cover
    make_audio(dest / "Jane Austen" / "Pride and Prejudice" / "Pride and Prejudice.m4b")
    shutil.copy2(cover, dest / "Jane Austen" / "Pride and Prejudice" / "cover.jpg")
    out["nested"] = dest / "Jane Austen" / "Pride and Prejudice"

    # CD1/CD2 naming variant
    make_audio(dest / "Multi Disc Book" / "CD1" / "01 track.mp3")
    make_audio(dest / "Multi Disc Book" / "CD 2" / "02 track.mp3")
    shutil.copy2(cover, dest / "Multi Disc Book" / "CD1" / "front.JPEG")
    out["cd12"] = dest / "Multi Disc Book"

    # Folder image + audio in same folder
    make_audio(dest / "folder_and_image" / "chapter.mp3")
    shutil.copy2(cover, dest / "folder_and_image" / "Photo.JPG")
    out["folder_and_image"] = dest / "folder_and_image"

    # HE-AAC best-effort (native aac encoder may write an AOT the slim decoder rejects)
    he = dest / "he_aac" / "track.m4b"
    try:
        run(
            [
                FFMPEG,
                "-hide_banner",
                "-loglevel",
                "error",
                "-y",
                "-f",
                "lavfi",
                "-i",
                "sine=frequency=440:sample_rate=32000:duration=1",
                "-c:a",
                "aac",
                "-profile:a",
                "aac_he",
                "-ar",
                "32000",
                "-ac",
                "1",
                "-b:a",
                "48k",
                "-f",
                "mp4",
                str(he),
            ]
        )
        out["he_aac"] = he
    except SystemExit:
        print("note: HE-AAC encode failed")

    # Progressive JPEG cover (PIL)
    try:
        from PIL import Image

        prog = dest / "progressive" / "cover.jpg"
        prog.parent.mkdir(parents=True, exist_ok=True)
        Image.new("RGB", (64, 64), (20, 80, 180)).save(prog, "JPEG", quality=70, progressive=True)
        make_audio(dest / "progressive" / "track.mp3")
        out["progressive"] = dest / "progressive"
    except Exception as e:
        print("note: progressive JPEG fixture skipped:", e)

    print(f"formats library: {len(out)} layouts -> {dest}")
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--sample-lib", default="build-host/fixtures/sample_library")
    ap.add_argument("--formats", default="build-host/fixtures/formats")
    args = ap.parse_args()
    build_sample_library(Path(args.sample_lib))
    build_formats(Path(args.formats))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
