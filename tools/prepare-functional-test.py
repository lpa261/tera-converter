#!/usr/bin/env python3
"""Create synthetic desktop test files, or verify their preserved contents.

No user files are read or overwritten. Generated directories live in packages/.
Requires /usr/bin/ffmpeg and Ghostscript; Python dependencies are standard library.
"""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
FFMPEG = "/usr/bin/ffmpeg"
PADDING_SIZE = 256 * 1024


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def run(argv):
    result = subprocess.run(argv, capture_output=True, text=True, timeout=60)
    if result.returncode:
        raise RuntimeError(result.stderr[-3000:])


def create_docx(path):
    # A complete, unsigned minimal OOXML package, deliberately stored without
    # DEFLATE so repacking has a predictable opportunity to save space.
    parts = {
        "[Content_Types].xml": """<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
<Default Extension="xml" ContentType="application/xml"/>
<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>
</Types>""",
        "_rels/.rels": """<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>
</Relationships>""",
        "word/document.xml": """<?xml version="1.0" encoding="UTF-8"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body>"""
        + "".join(
            f"<w:p><w:r><w:t>Đoạn {i}: TeraConvert giữ nguyên nội dung tài liệu khi tối ưu cấu trúc ZIP.</w:t></w:r></w:p>"
            for i in range(1, 121)
        )
        + "<w:sectPr><w:pgSz w:w=\"11906\" w:h=\"16838\"/></w:sectPr></w:body></w:document>",
    }
    with zipfile.ZipFile(path, "x", compression=zipfile.ZIP_STORED) as document:
        for name, text in parts.items():
            document.writestr(name, text.encode("utf-8"))


def generate():
    packages = ROOT / "packages"
    packages.mkdir(exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="functional-test-", dir=packages))
    print(f"Tạo bộ mẫu: {output}", flush=True)
    common = [FFMPEG, "-v", "error", "-nostdin", "-n", "-filter_threads", "2"]
    for filename, size in (("anh-mau.png", "640x360"), ("anh-thu-hai.png", "480x640")):
        run(common + ["-f", "lavfi", "-i", f"testsrc=size={size}:rate=1:duration=1",
                      "-frames:v", "1", "-threads", "1", str(output / filename)])
    shutil.copyfile(output / "anh-mau.png", output / "ảnh có dấu và khoảng trắng.png")
    video = output / "video-khong-vung-dem.mp4"
    run(common + ["-f", "lavfi", "-i", "testsrc2=size=640x360:rate=24:duration=3",
                  "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=3",
                  "-c:v", "libx264", "-threads", "2", "-preset", "fast", "-crf", "18",
                  "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "96k", "-shortest",
                  "-movflags", "+faststart", str(video)])
    padded = output / "video-co-vung-dem.mp4"
    with video.open("rb") as source, padded.open("xb") as target:
        shutil.copyfileobj(source, target)
        target.write(struct.pack(">I4s", PADDING_SIZE, b"free"))
        target.write(bytes(PADDING_SIZE - 8))
    run(common + ["-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=3",
                  "-c:a", "pcm_s16le", str(output / "am-thanh.wav")])
    run(["gs", "-q", "-dBATCH", "-dNOPAUSE", "-sDEVICE=pdfwrite",
         f"-sOutputFile={output / 'tai-lieu.pdf'}", "-c",
         "/Helvetica findfont 20 scalefont setfont 50 700 moveto "
         "(TeraConvert - page 1) show showpage "
         "/Helvetica findfont 20 scalefont setfont 50 700 moveto "
         "(TeraConvert - page 2) show showpage"])
    create_docx(output / "van-ban.docx")
    (output / "tep-loi.mp4").write_bytes(b"TeraConvert: intentionally invalid video.\n")
    manifest = {
        "schema": 1,
        "padding_bytes": PADDING_SIZE,
        "files": {path.name: {"bytes": path.stat().st_size, "sha256": digest(path)}
                  for path in sorted(output.iterdir()) if path.is_file()},
    }
    (output / "MANIFEST.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (output / "HUONG_DAN.md").write_text("""# Thử TeraConvert trong Files

Các file trong thư mục này là dữ liệu tổng hợp. Mở thư mục bằng Files và thử lần lượt.

| File | Thao tác | Kết quả cần kiểm tra |
| --- | --- | --- |
| `video-co-vung-dem.mp4` | Nén video → Bỏ vùng đệm dư cuối MP4/MOV | File `_compressed.mp4` nhỏ hơn đúng 262144 byte và giống từng byte với `video-khong-vung-dem.mp4`. |
| `video-khong-vung-dem.mp4` | Cùng chế độ bỏ vùng đệm | Không tạo file mới vì không có vùng đệm dư; nguồn giữ nguyên. HUD hiện chưa giải thích riêng trường hợp bỏ qua. |
| `van-ban.docx` | Tối ưu tài liệu | File `_compressed.docx` nhỏ hơn; tên và nội dung từng mục ZIP giữ nguyên. Mở cả hai bằng trình đọc tài liệu để so sánh. |
| `anh-mau.png` | Đổi sang JPG/WebP/PDF, thay đổi kích thước | File kết quả mở được và kích thước ảnh đúng lựa chọn. JPG/WebP có thể giảm chất lượng. |
| Hai file PNG khác nhau | Chọn cả hai, gộp thành PDF | PDF có hai trang theo thứ tự chọn. |
| `ảnh có dấu và khoảng trắng.png` | Đổi định dạng hoặc thay đổi kích thước | Xử lý được đường dẫn có dấu và khoảng trắng. |
| `am-thanh.wav` | Đổi sang FLAC hoặc MP3 | Nghe đủ 3 giây; FLAC giữ dữ liệu âm thanh, MP3 có giảm chất lượng. |
| `tai-lieu.pdf` | Đổi sang TXT/PNG | TXT có chữ của cả hai trang; PNG hiện xuất trang đầu. |
| `tep-loi.mp4` | Đổi sang WebM | Báo lỗi, không xuất một file kết quả hỏng, nguồn giữ nguyên. |

Thử nén lại file có vùng đệm để kiểm tra cách xử lý khi đã có kết quả trùng tên.
Thử một lô video dài của riêng bạn nếu cần kiểm tra hủy; bộ mẫu 3 giây có thể hoàn tất quá nhanh.
Các chế độ Web/Saver/HQ và nén PDF hiện có thể giảm chất lượng. AI cần runtime/model cài sẵn;
không dùng giao diện tải model để đánh giá bản này.

Từ thư mục dự án, xác minh nguồn và các kết quả tối ưu sau khi thử:

```bash
python3 tools/prepare-functional-test.py --verify "ĐƯỜNG_DẪN_THƯ_MỤC_NÀY"
```

Lệnh xác minh chỉ đọc file. Chưa có file kết quả sẽ được ghi rõ là CHƯA THỬ.
Tỷ lệ giảm của hai mẫu có dữ liệu dư được tạo chủ ý, không đại diện cho file thực tế.
""", encoding="utf-8")
    print(f"Đã tạo {len(manifest['files'])} file mẫu; hướng dẫn: {output / 'HUONG_DAN.md'}")
    return output


def zip_contents(path):
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)):
            raise ValueError(f"ZIP có mục trùng tên: {path.name}")
        return {name: hashlib.sha256(archive.read(name)).hexdigest() for name in names}


def verify(directory):
    output = directory.resolve(strict=True)
    manifest = json.loads((output / "MANIFEST.json").read_text(encoding="utf-8"))
    failures = 0
    for name, record in manifest["files"].items():
        if name != Path(name).name:
            raise ValueError("Manifest chứa đường dẫn không hợp lệ")
        path = output / name
        if not path.is_file() or digest(path) != record["sha256"]:
            print(f"LỖI: Nguồn thay đổi hoặc bị mất: {name}")
            failures += 1
    if not failures:
        print(f"ĐẠT: {len(manifest['files'])} file nguồn giữ nguyên SHA-256.")
    for stem, suffix in (("video-co-vung-dem", "mp4"), ("van-ban", "docx")):
        candidates = sorted(output.glob(f"{stem}_compressed*.{suffix}"))
        if not candidates:
            print(f"CHƯA THỬ: Chưa có kết quả tối ưu của {stem}.{suffix}.")
        for path in candidates:
            if suffix == "mp4":
                valid = digest(path) == digest(output / "video-khong-vung-dem.mp4")
            else:
                valid = zip_contents(path) == zip_contents(output / "van-ban.docx")
            smaller = path.stat().st_size < (output / f"{stem}.{suffix}").stat().st_size
            print(f"{'ĐẠT' if valid and smaller else 'LỖI'}: {path.name}: nội dung và dung lượng.")
            failures += not (valid and smaller)
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", type=Path, metavar="DIRECTORY")
    args = parser.parse_args()
    if args.verify:
        return verify(args.verify)
    generate()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
