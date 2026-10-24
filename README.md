# TeraConvert — Chuyển đổi, tối ưu file và Quick Look cho GNOME

TeraConvert tích hợp vào menu chuột phải của **GNOME Files / Nautilus** để chuyển đổi ảnh, video, âm thanh và tài liệu; tối ưu dung lượng với kiểm tra tính toàn vẹn; xem nhanh file bằng giao diện **Quick Look**.

Phần lõi dùng **GNU C11, GLib/GIO và Meson**. Giao diện dùng GTK4/libadwaita. Các backend sử dụng libvips, FFmpeg, Poppler, libarchive, libjpeg, zlib và một số chương trình phụ trợ/Python. Dự án không phải ứng dụng thuần C không có dependency ngoài.

**Phiên bản source hiện tại: 2.2.0 — nhánh phát triển Quick Look.** Gói test gần nhất được chuẩn bị trong workspace là `2.2.0+test.20261024.3`; đây không phải tuyên bố đã phát hành bản stable hoặc đã nghiệm thu mọi định dạng trên mọi máy.

Repository: [github.com/lpa261/tera-converter](https://github.com/lpa261/tera-converter).

## Mục lục

- [Chuyển đổi và nén khác nhau thế nào?](#chuyển-đổi-và-nén-khác-nhau-thế-nào)
- [Cách xử lý hình ảnh](#cách-xử-lý-hình-ảnh)
- [Cách xử lý video](#cách-xử-lý-video)
- [Cách xử lý âm thanh](#cách-xử-lý-âm-thanh)
- [Cách xử lý PDF và tài liệu Office](#cách-xử-lý-pdf-và-tài-liệu-office)
- [Quick Look và thông tin file](#quick-look-và-thông-tin-file)
- [Menu chỉ hiện các thao tác phù hợp](#menu-chỉ-hiện-các-thao-tác-phù-hợp)
- [Kiến trúc và vòng đời tác vụ](#kiến-trúc-và-vòng-đời-tác-vụ)
- [Build và cài đặt](#build-và-cài-đặt)
- [Sử dụng CLI](#sử-dụng-cli)
- [Kiểm thử và giới hạn đã biết](#kiểm-thử-và-giới-hạn-đã-biết)
- [Cấu trúc source và hướng phát triển](#cấu-trúc-source-và-hướng-phát-triển)

## Chuyển đổi và nén khác nhau thế nào?

| Thao tác | Mục tiêu | File đầu ra có thể lớn hơn? | Cam kết chất lượng |
|---|---|---|---|
| Chuyển định dạng | Tạo file phù hợp ứng dụng/thiết bị đích | **Có** | Tùy codec và tham số; có thể mã hóa lại, đổi độ sâu bit hoặc bỏ thuộc tính |
| Tối ưu giữ nguyên nội dung | Giảm phần dư hoặc thay cách đóng gói/mã hóa bảo toàn dữ liệu đã xác định | Chế độ `comp-*` chỉ publish khi đạt ngưỡng tiết kiệm | Bảo toàn theo hợp đồng riêng của từng backend |
| Nén có mất dữ liệu | Đổi dung lượng lấy chất lượng/tốc độ/tính tương thích | Candidate không đạt tiết kiệm sẽ bị loại | Có thể mất chi tiết; chưa có cam kết chất lượng cảm nhận đã hiệu chỉnh |
| `safe-flac` | Chuyển âm thanh với mẫu PCM được kiểm tra đầy đủ | **Có** | Giữ các mẫu âm thanh trong phạm vi được hỗ trợ |

Mặc định ứng dụng giữ nguồn và tạo file đầu ra riêng, tự đánh số nếu trùng tên. Việc chọn `--overwrite replace` là tường minh: nó có thể thay file đã tồn tại, kể cả file nguồn nếu đường dẫn đích trùng nguồn.

“Giữ nguyên nội dung” không có nghĩa SHA-256 của **toàn bộ file** không thay đổi. Ví dụ: ZIP/Office được nén lại nhưng giữ byte từng entry; JPEG được tối ưu bảng Huffman nhưng giữ hệ số DCT; FLAC được đối chiếu theo PCM đã giải mã.

Không có tỷ lệ giảm dung lượng cố định. File đã nén tốt có thể không còn phần nào tối ưu được. Mã hóa lại file lớn hơn không khôi phục những chi tiết đã mất trong nguồn.

## Cách xử lý hình ảnh

### Chuyển định dạng và thay đổi kích thước

Backend chính là **libvips**, đọc ảnh theo chế độ sequential và chọn bộ ghi theo định dạng đích. Các định dạng hiện có trong luồng chuyển đổi gồm WebP, PNG, JPEG, AVIF, BMP, TIFF, ICO và PDF; khả năng thực tế còn phụ thuộc loader/saver và helper được cài.

| Đích | Thiết lập hiện tại | Ý nghĩa |
|---|---|---|
| WebP | libvips, `Q=85` | Có mất dữ liệu; không phải chế độ WebP lossless |
| JPEG | libvips, `Q=90` | Mã hóa lại có mất dữ liệu; lưu JPEG lần nữa có thể mất thêm chi tiết |
| AVIF | libvips/HEIF với codec AV1, `Q=80` | Có mất dữ liệu; cần hỗ trợ codec tương ứng |
| PNG | libvips, compression level 6 | Nén dữ liệu ảnh đầu ra; không khôi phục chất lượng đã mất từ nguồn JPEG |
| TIFF | libvips, DEFLATE | Không đồng nghĩa mọi metadata, frame hoặc đặc tính nguồn đều được giữ |
| BMP / GIF và các saver khác | Bộ ghi tương ứng của libvips | Phụ thuộc định dạng và module runtime |
| ICO | Python/Pillow | Tạo nhiều kích thước icon từ 16 đến 256 px; resize tường minh giới hạn mỗi chiều ở 256 |
| PDF từ ảnh | Ưu tiên `img2pdf`; fallback Pillow | Đường fallback có thể đổi mode màu, alpha hoặc mã hóa lại ảnh |

Resize dùng tỷ lệ ngang/dọc tính từ kích thước đích. Nếu truyền chiều rộng và chiều cao không cùng tỷ lệ với ảnh gốc, ảnh có thể bị kéo giãn. Giao diện resize hỗ trợ lựa chọn kích thước; CLI yêu cầu cả hai chiều.

Giới hạn hình học của engine libvips là **16.384 px mỗi chiều**. Governor còn kiểm tra RAM ước lượng từ kích thước giải mã, số kênh và độ sâu mẫu; một ảnh nằm dưới giới hạn hình học vẫn có thể bị từ chối nếu vượt ngân sách.

Các giá trị Q thuộc những encoder khác nhau **không thể so sánh trực tiếp**. Chuyển định dạng thông thường không mang hợp đồng bảo toàn toàn bộ ICC/EXIF/alpha/animation/multipage/bit depth. Với nội dung nhiều frame/trang, không nên suy rằng mọi frame đều được giữ.

### Tối ưu PNG/JPEG giữ nguyên nội dung — `comp-image`

Đây là đường xử lý riêng, không chạy lại JPEG với Q100 rồi gọi là lossless.

**PNG:** kiểm tra cấu trúc chunk, CRC, kích thước, dữ liệu zlib và scanline/filter; nối/recompress IDAT bằng DEFLATE; đối chiếu dữ liệu đã giải nén và các chunk ngoài IDAT. Alpha, palette và metadata trong phạm vi cho phép được giữ. APNG, chunk lạ và cấu trúc chưa có validator bị từ chối.

**JPEG:** đọc/ghi ở miền hệ số DCT, tối ưu entropy/Huffman mà không lượng tử hóa lại. Đối chiếu hệ số, bảng lượng tử, thành phần, sampling và metadata được cho phép; giữ chế độ progressive khi được hỗ trợ. Marker/feature chưa xác minh, MPO và dữ liệu ngoài phạm vi bị từ chối.

Giới hạn riêng của optimizer: **32 MiB input, 16 triệu pixel**, JPEG 8-bit; PNG có thêm giới hạn **64 MiB dữ liệu lọc sau giải nén**. Candidate vẫn phải đạt điều kiện tiết kiệm trước khi worker publish.

### Tách nền AI

Tách nền dùng helper **RMBG ONNX**, NumPy/Pillow và ONNX Runtime. Đây là thao tác thay đổi nội dung ảnh, không phải nén lossless.

Model phải được cung cấp sẵn cùng reference SHA-256. Ứng dụng không còn luồng tải model mô phỏng hoặc tự tải model. Có thể đăng ký model tương thích bằng:

```sh
teraconvert-register-model /path/to/rembg.onnx --sha256 REFERENCE_SHA256
```

Tool mặc định đặt model trong thư mục dữ liệu của người dùng, từ chối ghi đè nếu không có `--replace`, và kiểm tra checksum trước khi cài. Helper kiểm tra checksum lại khi chạy. SHA-256 xác nhận tính toàn vẹn với giá trị đã cung cấp, không chứng minh model tương thích hoặc có nguồn gốc đáng tin cậy.

## Cách xử lý video

MP4, WebM, MKV và MOV là các lựa chọn container; kết quả còn phụ thuộc video/audio codec bên trong. Luồng chuyển đổi thông thường và luồng tối ưu bảo toàn nội dung là hai đường khác nhau.

### Chuyển định dạng thông thường

| Đích | Hành vi hiện tại |
|---|---|
| MP4 | Có thể copy H.264/HEVC và audio phù hợp; trường hợp khác dùng encoder H.264 CPU/GPU theo khả năng phát hiện |
| MOV | Có thể copy H.264/HEVC/ProRes và audio phù hợp; nếu cần thì mã hóa lại |
| MKV | Planner legacy chọn stream copy; không có hợp đồng validator đầy đủ như `comp-remux` |
| WebM | Copy video nếu đã là VP8/VP9; còn lại mã hóa VP9. Audio copy khi phù hợp hoặc chuyển Opus 128 kbit/s |
| AVI | MPEG-4/Xvid và MP3; có mã hóa lại |
| GIF từ video | 15 fps, chiều rộng 480 px, chiều cao theo tỷ lệ; có thay đổi độ phân giải/fps và không chứa audio |
| Trích audio | Dùng các thiết lập trong phần âm thanh bên dưới |

Những nhánh legacy có stream copy không đồng nghĩa toàn bộ track, chapter, attachment, metadata và thuộc tính nguồn đã được bảo toàn. Chỉ các mode có validator tương ứng mới mang hợp đồng đó.

### Vì sao MP4 24,1 MB có thể thành WebM 57 MB?

Nếu video nguồn chưa là VP8/VP9, đường WebM hiện chọn:

```text
video: libvpx-vp9
-deadline realtime -cpu-used 5 -row-mt 1 -crf 30 -b:v 0
```

Đây là cấu hình ưu tiên tốc độ với mức chất lượng cố định, **không đặt trần bitrate theo nguồn hoặc mục tiêu kích thước file**. MP4 gốc có thể đã được nén hiệu quả; encode lại VP9 vẫn có thể tạo file lớn hơn. Nếu thời lượng giữ nguyên, 57 / 24,1 tương ứng bitrate tổng trung bình tăng khoảng 2,37 lần.

Chuyển sang WebM hiện là thao tác **chuyển định dạng**, nên candidate lớn hơn được phép xuất. Không có preset “nén WebM theo dung lượng/chất lượng đã kiểm chứng” trong sản phẩm hiện tại. Không nên chọn WebM chỉ vì kỳ vọng phần mở rộng này tự làm file nhỏ đi.

### Hai mode video bảo toàn nội dung

| Mode | Cách làm | Kiểm tra | Giới hạn |
|---|---|---|---|
| `comp-padding` | Chỉ cắt suffix gồm atom top-level `free`/`skip` trong MP4/MOV được hỗ trợ | So từng byte của toàn bộ phần giữ lại, giữ offsets và kiểm tra nguồn không đổi | Không bỏ padding giữa file, không mã hóa lại audio/video, không hỗ trợ mọi brand/layout |
| `comp-remux` | FFmpeg stream copy để đóng gói lại cấu trúc MP4/MOV được hỗ trợ | Manifest packet theo từng stream: byte, boundary, timestamp/duration dạng rational, flags, side data; codec properties/metadata/semantic atoms; decode đầy đủ output | Chỉ layout AV self-contained đã có validator; không remux chapter, subtitle, cover art, opaque/fragmented/external structures |

`comp-padding` không làm nhỏ bitstream video. `comp-remux` không tự chuyển codec thành codec “nén tốt hơn”. Cả hai đều có thể trả về không có cải thiện hoặc không hỗ trợ; chúng không fallback sang encode mất dữ liệu.

### Preset nén có mất dữ liệu

| Mode | Mục tiêu legacy | Giới hạn chiều rộng hiện tại | Heuristic bitrate video CPU/NVENC |
|---|---|---|---|
| `comp-web` | Chia sẻ, H.264 | Tối đa 1280 px, làm tròn chẵn | Khoảng 55% bitrate video nguồn; clamp 60–1800 kbit/s |
| `comp-saver` | Ưu tiên dung lượng, HEVC | Tối đa 1920 px, làm tròn chẵn | Khoảng 38%; clamp 50–1400 kbit/s |
| `comp-hq` | Nén nhẹ hơn; HEVC với nguồn HEVC/AV1, H.264 với nguồn khác | Tối đa 1920 px, làm tròn chẵn | Khoảng 68%; clamp 60–3200 kbit/s |

Bảng mô tả heuristic trong code, không phải tỷ lệ tiết kiệm đo được hay chất lượng tương đương. Khi thiếu bitrate nguồn, planner dùng giá trị fallback. VAAPI dùng cách tính QP thay cho cùng rate-control; CPU và GPU không được xem là có chất lượng giống nhau.

Giới hạn trên là **chiều rộng**, không phải cam kết mọi output là 720p/1080p: chiều cao được suy theo tỷ lệ, đặc biệt cần lưu ý video dọc. Audio AAC phù hợp có thể được copy; audio khác có thể chuyển AAC 128/160 kbit/s tùy preset. Các mode này có thể thay đổi độ sâu bit, màu/HDR, track và metadata; không mang hợp đồng giữ nguyên các thuộc tính đó.

FFmpeg backend thử GPU NVENC/VAAPI khi phát hiện được khả năng sử dụng, có đường fallback CPU cho conversion legacy. Probe H.264 thành công không chứng nhận mọi codec/depth/GPU path; hardware trial và chất lượng vẫn cần kiểm tra trên máy đích.

## Cách xử lý âm thanh

| Mode/đích | Cách xử lý |
|---|---|
| `mp3` | `libmp3lame`, 320 kbit/s |
| `wav` | PCM signed 16-bit little-endian; có thể giảm độ sâu từ nguồn 24/32-bit |
| `flac` thông thường | Copy nếu nguồn đã là FLAC; trường hợp khác dùng encoder FLAC với chính sách conversion legacy |
| `m4a` | Copy AAC khi phù hợp; trường hợp khác encode AAC 256 kbit/s |
| `ogg` | Vorbis, quality 5 |
| `opus` | Opus, 128 kbit/s |
| `safe-flac` | Chuyển đổi và đối chiếu toàn bộ mẫu PCM trong phạm vi bên dưới |

Chuyển MP3 sang WAV/FLAC không phục hồi chi tiết đã mất trong MP3 và thường làm tăng dung lượng. Container/codec đầu ra hỗ trợ lossless không tự biến toàn bộ quá trình chuyển đổi thành bảo toàn nguồn.

**`safe-flac`** nhận một audio stream integer PCM/FLAC **16 hoặc 24-bit**, sample rate hợp lệ, channel layout đã biết, tối đa 8 kênh. Không resample/downmix hoặc ép về 16-bit một cách ngầm định. Nguồn float, độ sâu khác, layout không xác định, cover art, chapter, program/group ngoài phạm vi bị bỏ qua.

Engine giải mã nguồn và candidate thành biểu diễn chuẩn **PCM s32le**, so SHA-256 và tổng byte/sample data; đồng thời kiểm tra depth, rate, layout và metadata được probe nhìn thấy. Chuẩn hóa s32le là để so sánh đủ độ chính xác, không phải tuyên bố output được nâng thành âm thanh 32-bit.

Hợp đồng này giữ **mẫu âm thanh** được hỗ trợ, không giữ mọi opaque feature của container nguồn. `safe-flac` là conversion, nên output có thể lớn hơn input.

## Cách xử lý PDF và tài liệu Office

### PDF

| Thao tác | Backend và hành vi |
|---|---|
| PDF → PNG/JPEG | Poppler `pdftoppm`, 150 DPI, `-singlefile`; luồng hiện tại xuất trang đầu, không tự xuất mọi trang thành bộ ảnh |
| PDF → TXT | `pdftotext -layout` |
| Nén PDF legacy | Ghostscript `pdfwrite`, PDF 1.4, `/screen` cho web và `/ebook` cho print; có thể downsample/re-encode nội dung |
| Tối ưu bảo toàn — `comp-pdf-safe` | QPDF qua Python `pikepdf`, recompress Flate/object streams phù hợp; không bật JPEG image optimization |

Validator của `comp-pdf-safe` đối chiếu semantic object graph và stream data, PDF version, page count, text extraction và **render mọi trang ở kích thước tối đa 1600 pixel**. Render là lớp kiểm tra bổ sung cho graph/stream validation, không phải bằng chứng trực quan ở mọi mức zoom.

Phạm vi hiện tại: PDF tĩnh, không mã hóa, tối đa **512 MiB input, 128 trang, 20.000 object**, có thêm giới hạn graph/decoded streams. Forms, chữ ký số, annotations, active/embedded content và feature chưa được hỗ trợ bị từ chối. Thiếu helper/QPDF/validator sẽ báo unsupported hoặc lỗi rõ ràng.

### OOXML và ODF — `comp-office`

Menu hiện cho phép tối ưu **DOCX/XLSX/PPTX và ODT/ODS/ODP** trong phạm vi validator. Đây là nén lại package ZIP, không chuyển tài liệu sang định dạng khác.

- Kiểm tra danh sách entry, đường dẫn, kích thước và SHA-256 nội dung trước/sau repack; xử lý streaming, không extract tùy ý ra filesystem.
- OOXML dùng libarchive. ODF dùng native pre/post validation và Python ZIP writer để giữ `mimetype` ở đầu, không nén, không có extra field.
- Không re-encode ảnh nhúng và không thay XML nội dung tài liệu.
- Từ chối path traversal, link, duplicate entry, package ký số/mã hóa và cấu trúc ngoài phạm vi.

Giới hạn: **10.000 entry, 1 GiB tổng nội dung giải nén, 4096 byte cho đường dẫn entry và 16 MiB tổng tên entry**. Byte từng entry được kiểm tra; ZIP headers/metadata không có cam kết giống từng byte. Khả năng mở và mọi tính năng trên Office/LibreOffice đích vẫn cần nghiệm thu với tài liệu thật.

`.doc/.xls/.ppt` legacy không thuộc optimizer này. Chuyển Office sang PDF chưa có backend hoàn chỉnh nên không xuất hiện trong menu. Ghép ảnh thành PDF dùng `img2pdf` và fallback Pillow, không hỗ trợ ghép tùy ý video/tài liệu hỗn hợp.

## Quick Look và thông tin file

`teraconvert-preview` là ứng dụng xem trước riêng, không chạy qua daemon nén. Có thể truyền nhiều file:

```sh
teraconvert-preview ./photo.png ./document.pdf ./clip.mp4
```

| Nội dung | Hành vi preview |
|---|---|
| Ảnh raster được hỗ trợ | Fit cửa sổ, áp dụng embedded orientation; không cam kết playback ảnh động |
| Văn bản/JSON | Chỉ đọc, tối đa 256 KiB; có thông báo khi bị cắt; không hiện nội dung nhị phân như văn bản |
| PDF | Một trang mỗi lần, raster tối đa 1600 px; hỗ trợ chuyển trang, phụ thuộc `engine_poppler` |
| Audio/video | GtkMediaFile/GtkVideo và codec/backend đã cài; bắt đầu ở trạng thái tạm dừng |
| Định dạng khác | Fallback thông tin file; không có renderer Office/archive trong MVP |

Giới hạn riêng của viewer: inline file tối đa **64 MiB**, ảnh tối đa **24 triệu pixel** và 16.384 px mỗi chiều; media dùng backend streaming thay cho cùng giới hạn đọc inline. PDF preview giới hạn số trang ở 4096. Đây không phải các giới hạn giống optimizer ở daemon.

Giao diện có tên file trên header và dòng thông tin phía dưới: loại file, dung lượng, độ phân giải **gốc** của ảnh hoặc số trang/khổ PDF. Thời lượng media hiện khi backend cung cấp. Mục **Thông tin chi tiết / File details** có MIME, số byte chính xác, ngày sửa đổi và đường dẫn có thể chọn/copy.

| Phím | Tác dụng |
|---|---|
| Space / Escape | Đóng preview |
| ← / → | Chuyển file trong danh sách CLI; provider Space gửi selection event cho Nautilus |
| Page Up / Page Down | Chuyển trang PDF |

Loader chạy nền, tối đa một tác vụ decode tại một thời điểm; điều hướng nhanh được coalesce và kết quả cũ bị loại bằng generation/cancellation. Media được tạm dừng khi chuyển/đóng. Một số thao tác native decoder không thể bị ngắt ngay.

### Bật Space trong Nautilus

Context menu sử dụng bus riêng `org.gnome.TeraConvert.Preview`. Tích hợp Space hỗ trợ interface legacy `org.gnome.NautilusPreviewer` và `org.gnome.NautilusPreviewer2`.

Để chọn TeraConvert làm provider Space ở cấp người dùng, chạy sau khi cài gói 2.2:

```sh
teraconvert-quicklook-setup --enable
```

Đăng xuất rồi đăng nhập lại để nhả name của Sushi đang chạy. Sau đó chọn file trong Nautilus và nhấn Space. Để trở lại provider hệ thống:

```sh
teraconvert-quicklook-setup --disable
```

Helper không sửa service thuộc package Sushi, không tự kill process preview, và từ chối ghi đè user service khác có sẵn. Cần đăng xuất/đăng nhập sau khi đổi provider. Wayland/X11 focus, foreign parenting, selection và giao diện thực tế còn cần nghiệm thu desktop.

## Menu chỉ hiện các thao tác phù hợp

Các thao tác chuyển đổi/nén được lọc theo **mọi file trong lựa chọn**, source format được hỗ trợ, backend được build và sự hiện diện của executable/helper. Menu con rỗng bị bỏ. Resize/AI ẩn khi không khả dụng; trạng thái lựa chọn cũ được xóa khi chọn thư mục hoặc bỏ chọn file.

Ví dụ bị ẩn từ đầu: `safe-flac` với MP3; `comp-image` với WebP; `comp-padding`/`comp-remux` với MKV; chuyển Office sang PDF; conversion khi không có daemon; lựa chọn remote hoặc hỗn hợp không có thao tác chung. Context-menu Quick Look hiện xử lý file đầu trong lựa chọn; CLI viewer nhận danh sách nhiều file.

Menu không chạy encoder, import Python hay probe media nặng trên UI thread. Vì vậy mã hóa/chữ ký, PCM depth/layout, opaque tracks và thiếu module/codec đặc thù vẫn có thể chỉ được phát hiện khi backend kiểm tra nội dung. Việc có mục menu không thay thế validation. AI còn phải xác minh ONNX Runtime và model khi chạy, ngoài các kiểm tra hiện diện helper/model/receipt ban đầu.

## Kiến trúc và vòng đời tác vụ

```text
Nautilus extension ── IPC v2 / Unix socket ── Conversion daemon
                                               │
                                  Admission → worker → engine
                                               │
                                  Validate → savings → publish
                                               │
                                      JSON result / HUD

Nautilus / CLI ── D-Bus ── Quick Look (GTK4)
                              │
                  Background loader / GTK media backend
```

| Thành phần | Vai trò |
|---|---|
| `libnautilus-teraconvert.so` | Menu, lọc capability, gửi yêu cầu; không encode trong process Nautilus |
| `teraconvert-daemon` | Registry engines, scheduler, tài nguyên, temp, validation và publish |
| `teraconvert-ui` | HUD theo batch, resize, thông tin AI setup |
| `teraconvert-preview` | Quick Look và metadata, độc lập với worker nén |
| `teraconvert-cli` | Submit, theo dõi, hủy batch và chẩn đoán IPC; output JSON lines |

Một job đi qua: xác thực yêu cầu → resolve output → giữ chỗ RAM/CPU/disk → probe/refine estimate → tạo workspace riêng → chạy engine → validate → kiểm tra tiết kiệm nếu là compression → quyết định hủy/publish → dọn tài nguyên và trả result.

Trạng thái phân biệt tạo mới, không có cải thiện, không hỗ trợ, file đã tồn tại, hủy và thất bại. HUD hiển thị mode/phase, số đếm, lý do và saved bytes; ngắt kết nối không bị coi là hoàn tất thành công.

### File, IPC và hủy tác vụ

- Skip không encode nếu đích đã có. Auto-number/skip publish bằng hard link cùng filesystem để không clobber entry đã tồn tại; replace dùng đường tường minh.
- Workspace `.teraconvert-*` riêng cùng output filesystem, có binary ownership record giữ flock. Recovery chỉ dọn directory inactive xác minh được UID/device/inode; giữ lại file lạ và job sống. Không quét xóa chỉ theo prefix/tuổi.
- Kiểm tra identity/size/mtime/ctime của nguồn trước publish, gồm các nguồn merge. Hủy được serialize với quyết định publish; nguồn thay đổi thì candidate bị loại. Cơ chế này không phải bảo đảm mọi tình huống mất điện hoặc thay đổi filesystem đối kháng.
- IPC v2 có batch ID, ACK/capabilities, structured progress/results, listener/cancel theo batch. Header v1 vẫn 48 byte; legacy controls chỉ ánh xạ khi không mơ hồ. Client mới không fallback ngầm sang legacy lossy.
- Intake incremental có giới hạn payload/FD/deadline; tối đa 64 batch và 10.000 job giữ chỗ, cache completion 32 batch. CLI dùng inline payload tối đa 32 KiB; extension có sealed memfd cho batch lớn hơn trong giới hạn protocol.
- Helper runner dùng argv, drain pipes, bounded stderr, process group, timeout, TERM→KILL và reap direct child. Không interpolate tên file vào shell command.

### CPU, RAM và disk

Mặc định có một media worker và tối đa bốn general worker. Admission giữ chỗ CPU/RAM/disk đồng thời, ngân sách RAM estimate **1 GiB**, thời gian chờ tối đa 5 phút và có hủy. Estimate được refine theo decoded geometry/depth hoặc PCM expansion.

Disk reservation nhóm theo device, có margin 20% và headroom 200 MiB. Helpers có per-process address-space/per-file size limits, kiểm tra sampled process-tree RSS, workspace growth và dung lượng còn lại. FFmpeg dùng thread limit nhỏ; x265 có pool/frame-thread riêng được giới hạn. Systemd service có lớp `MemoryMax=4000M` riêng.

Estimate và sampled checks không phải hard aggregate RAM/disk quota; mọi thread nội bộ, thư viện native và tác động của process khác không được ràng buộc tuyệt đối. Direct engine calls bỏ qua worker admission. Không có SLA về RAM, tốc độ hoặc desktop responsiveness áp dụng cho mọi input.

## Build và cài đặt

### Dependency

| Nhóm | Build dependency | Runtime đáng chú ý |
|---|---|---|
| Core | GCC/GNU C11, Meson, Ninja, pkg-config; GLib/GIO ≥2.74 | GLib/GIO, systemd user session cho desktop package |
| Nautilus | Extension API 4 | GNOME Files/Nautilus tương thích |
| UI / Quick Look | GTK ≥4.10, libadwaita ≥1.4, GdkPixbuf ≥2.42 | Desktop D-Bus; image/Glycin và media codecs tùy hệ thống |
| Ảnh | libvips ≥8.12, zlib ≥1.2.9, libjpeg | Saver/loader tương ứng; Pillow cho một số conversion |
| Media | libavcodec, libavformat, libavutil, libswscale | FFmpeg; các preservation engine dùng `/usr/bin/ffmpeg` |
| PDF | Poppler GLib ≥22 | Poppler tools, Ghostscript; `pikepdf`/QPDF cho safe PDF |
| Office | libarchive ≥3.6 | Python stdlib cho ODF writer |
| AI tùy chọn | Engine wrapper | NumPy, Pillow, ONNX Runtime, compatible local model + SHA receipt |

libnotify là dependency tùy chọn. Danh sách capability phản ánh backend được build, không chứng nhận mọi codec/module runtime hoặc GPU. Gói Debian tính ABI dependencies từ binary thật; cần build trên distro đích, không suy rằng một `.deb` chạy trên mọi bản Ubuntu/Debian.

Ví dụ cài dependency trên Debian/Ubuntu có các package tương ứng:

```sh
sudo apt install meson ninja-build gcc pkg-config dpkg-dev \
  libglib2.0-dev libnautilus-extension-dev libgtk-4-dev libadwaita-1-dev \
  libgdk-pixbuf-2.0-dev libvips-dev libpoppler-glib-dev libarchive-dev \
  zlib1g-dev libjpeg-dev libavcodec-dev libavformat-dev libavutil-dev \
  libswscale-dev libnotify-dev ffmpeg img2pdf ghostscript poppler-utils \
  python3 python3-pil python3-pikepdf
```

AI cần các package Python tương ứng riêng. Playback còn phụ thuộc backend GTK/codec; package hiện recommend GStreamer plugins-good/libav khi có sẵn.

### Build từ thư mục source

```sh
meson setup build --prefix=/usr --buildtype=release
meson compile -C build
meson test -C build --print-errorlogs
```

Dùng `meson configure build` để xem option; dùng `meson setup build --reconfigure ...` khi thay cấu hình build hiện hữu.

| Option | Mặc định | Phạm vi |
|---|---|---|
| `client` | `true` | Nautilus extension |
| `daemon` | `true` | Conversion daemon và engine registry |
| `ui` | `auto` | HUD/resize/setup và Quick Look |
| `engine_vips` | `auto` | Chuyển/resize ảnh |
| `engine_ffmpeg` | `auto` | Audio/video, padding, remux và safe FLAC |
| `engine_safe_images` | `auto` | Native PNG/JPEG optimizer |
| `engine_poppler` | `auto` | PDF engine, helper safe PDF và PDF preview |
| `engine_office` | `auto` | OOXML/ODF optimizer |
| `engine_ai` | `auto` | Wrapper AI tách nền |

`auto` cho phép dependency detection quyết định với các feature có dependency; `enabled` yêu cầu dependency, `disabled` tắt feature. AI wrapper không tự cài runtime/model dù option đang bật. Quick Look chưa có option độc lập ngoài `ui`.

### Đóng gói và cài đè

```sh
TC_PACKAGE_VERSION=2.2.0+test.20261024.3 ./build-deb.sh
./tools/install-local.sh packages/teraconvert-c_2.2.0+test.20261024.3_amd64.deb
```

Ví dụ tên file trên dành cho amd64; script chọn architecture qua dpkg. `build-deb.sh` cấu hình lại complete desktop build, giữ các gói cũ và stage trong thư mục riêng. Package version test và version source không nhất thiết trùng chuỗi.

Chạy installer trong terminal desktop bằng tài khoản người dùng, **không prefix sudo**. Script dùng sudo cho apt, kiểm tra package/files, restart user daemon/socket, đối chiếu binary đang chạy, negotiate IPC v2 và đóng Nautilus để nạp extension mới. Hoàn tất job đang chạy trước khi cài, rồi mở Files lại. Cập nhật extension/daemon/UI/CLI cùng gói. Có thể chọn gói cũ để rollback; upgrade/downgrade thực tế vẫn cần nghiệm thu trên máy đích.

## Sử dụng CLI

```sh
# Chẩn đoán kết nối/version/capabilities
teraconvert-cli --doctor

# Chuyển định dạng: không bảo đảm nhỏ hơn
teraconvert-cli --format webp ./photo.jpg
teraconvert-cli --format webm ./video.mp4

# Resize ảnh: cần cả chiều rộng và chiều cao
teraconvert-cli --format png --width 1280 --height 720 ./photo.jpg

# Các đường tối ưu/bảo toàn có validator riêng
teraconvert-cli --format comp-image ./photo.png
teraconvert-cli --format comp-padding ./video.mp4
teraconvert-cli --format comp-remux ./video.mov
teraconvert-cli --format comp-pdf-safe ./document.pdf
teraconvert-cli --format comp-office ./document.odt
teraconvert-cli --format safe-flac ./audio.wav

# Chọn chính sách đích / điều khiển batch
teraconvert-cli --format comp-image --overwrite skip ./photo.png
teraconvert-cli --cancel 12345
teraconvert-cli --listen 12345
```

Overwrite mặc định `auto-number`; có `skip` và explicit `replace`. SIGINT yêu cầu hủy rồi chờ kết quả. JSON lines gồm `batch_id`, phase/status, số file đã xử lý/tạo/bỏ qua/hủy/lỗi, byte input/output/saved và reason. CLI có exit code khác nhau cho lỗi, hủy và timeout; exit thành công không có nghĩa mọi file đều tạo output — cần đọc count `unsupported`/`no_benefit`.

CLI và direct invocation không dùng cùng bộ lọc menu; server/engine vẫn kiểm tra yêu cầu và có thể trả unsupported. Payload inline quá lớn cần chia batch.

## Kiểm thử và giới hạn đã biết

Các lệnh chính:

```sh
meson test -C build --print-errorlogs
python3 tools/check-build-matrix.py
python3 tests/test_daemon_rpc.py --installed
python3 tools/prepare-functional-test.py
# Kiểm tra fixture/output sau khi thử menu:
python3 tools/prepare-functional-test.py --verify /path/to/functional-test-directory
```

Bộ test gồm IPC/framing, governor, process runner, batch lifetime/cancel, output collisions/source mutation, ownership recovery, Office và preservation engines, Quick Look loader, menu policy và dựng menu bằng API Nautilus thật với fixture metadata.

| Chứng cứ đã ghi nhận | Phạm vi và giới hạn |
|---|---|
| Full suite sau menu filtering | **18 nhóm OK, 1 nhóm SKIP, 0 FAIL** ở đợt đã chạy; không phải kết quả mới được chạy lại chỉ vì sửa README |
| Quick Look loader ASan/UBSan | Các ca local-only, text/size/type và PDF đạt; positive image bị skip do Glycin/D-Bus bị chặn; LSan tắt |
| Menu ASan/UBSan | Hai nhóm policy/native menu đạt; fixture metadata, không phải thao tác trên Nautilus GUI đang chạy |
| Worker/governor TSan ở đợt 2.1 | Hai nhóm concurrency đạt, không suppression; không chứng nhận mọi thư viện/codec sạch race |
| Build matrix | Có công cụ và báo cáo cấu hình tùy chọn; kết quả lịch sử gắn với source/config của lần chạy |

Production IPC test trả **77 / skip** nếu không thể bind AF_UNIX hoặc đọc real `SO_PEERCRED`. Dispatcher unit test mock riêng credentials, không thay bằng chứng IPC thật. Fixture generator báo output thiếu là chưa thử, không phải pass.

Môi trường đã chạy hiện không cho tạo private D-Bus session; image/Glycin test và compression integration có phần bị chặn. Chưa nghiệm thu đầy đủ GUI Quick Look, Space/selection/focus, playback codecs, AI inference với model thật, mở tài liệu bằng Office đích, desktop responsiveness và cài upgrade/downgrade. ASan dùng `detect_leaks=0`; system libraries/helpers không được rebuild cùng sanitizer. Không diễn giải những giới hạn này thành pass hoặc bảo đảm sạch leak/race.

### Nghiên cứu nén theo chất lượng cảm nhận

`tools/quality_benchmark.py` là harness nghiên cứu riêng: search candidate trong ngân sách, kiểm tra frame/timestamp/audio trên toàn clip đăng ký, verified lossless reference, SSIM/PSNR và optional VMAF có explicit model hash; ghi bytes/time/RSS/command, encode lặp ứng viên cuối và xuất cặp worst-frame A/B.

Metric hiện yêu cầu **SDR 8-bit BT.709 limited-range YUV420**, clip ngắn trong giới hạn harness; HDR/unknown color bị từ chối. RSS từ GNU time là maximum child-process RSS, không phải tổng process-tree peak đồng thời.

[Corpus manifest](docs/research/quality-corpus-manifest.json) hiện dành 25 vị trí có thông tin source/license/credit; media thật chưa được đăng ký trong snapshot hiện có. Pilot synthetic chỉ kiểm tra công cụ, không chứng minh chất lượng cảm nhận. Chưa đưa mode quality-search này vào daemon/menu như preset đã được nghiệm thu.

```sh
python3 tools/quality_benchmark.py --manifest docs/research/quality-corpus-manifest.json \
  --register bbb-01 --path /absolute/path/short-clip.mp4
python3 tools/quality_benchmark.py --manifest docs/research/quality-corpus-manifest.json \
  --output /tmp/quality-report.json --work-dir /tmp/quality-experiment-unique
```

SSIM/CRF mặc định là tham số nghiên cứu, không phải chuẩn “không giảm chất lượng”. Vẫn cần corpus thật, review A/B, benchmark phần cứng đích và tiêu chí phát hành; report không tự promote `release_ready` thành true.

## Cấu trúc source và hướng phát triển

```text
include/            Hợp đồng chung, IPC, policy/result, resource/process APIs
src/client/         Nautilus extension, dispatch, menu capability policy
src/daemon/         IPC intake, scheduler, governor, temp ownership và publish
src/engines/        Backend ảnh/media/PDF/Office/AI, probe/planner/runner/validators
src/preview/        Quick Look app, background loader, metadata và D-Bus
src/ui/             HUD, resize và AI setup information
src/cli/            JSON command-line client
tests/              Regression/unit/integration fixtures
tools/              Package install, model registration, benchmark và build matrix
data/               Icons, UI resources, D-Bus/systemd, desktop metadata
skills/             Community coding instructions cho contributor
docs/research/      Roadmap, nghiên cứu, snapshot validation và corpus manifest
```

Các file `.conf` trong `data/profiles/` là **legacy reference templates**; daemon không load/execute chúng như cấu hình hiện hành. Policy/planner thực nằm trong C source. Sửa `.conf` không làm thay đổi codec/preset đang chạy.

Tài liệu kiểm chứng:

- [Roadmap 30 hạng mục](docs/research/compression-implementation-plan.md).
- [Validation nền tảng nén 2.1](docs/research/roadmap-validation-2026-10-23.md).
- [Phạm vi Quick Look 2.2](docs/research/quicklook-2.2-validation-2026-10-24.md).
- [Metadata Quick Look](docs/research/quicklook-metadata-2026-10-24.json).
- [Menu filtering và package checks](docs/research/menu-filtering-2026-10-24.json).

Các bước còn lại gồm nghiệm thu desktop/codec, corpus thật và A/B, hiệu chỉnh tài nguyên, UX preview như zoom/thumbnail, thêm renderer Office/archive khi có backend phù hợp và kiểm tra nâng cấp/rollback trên máy đích. Chúng là hướng phát triển, không phải tính năng đã có.

Đóng góp code cần đọc [AGENTS.md](AGENTS.md), giữ GNU C11/GLib/GIO/Meson, chạy test phù hợp và báo rõ skip/giới hạn môi trường. Không đưa build output, `.deb`, model, credential hoặc dữ liệu cá nhân vào source repository.

**Giấy phép dự án được khai báo: GPL-3.0-or-later.** Thư viện, helper, codec và model AI có giấy phép/điều kiện riêng; sử dụng chúng theo giấy phép tương ứng.
