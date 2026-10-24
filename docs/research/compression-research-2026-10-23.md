# TeraConvert: nghiên cứu nén giảm dung lượng và bảo toàn chất lượng

Ngày: 23/10/2026. Trạng thái: rà soát mã nguồn, pilot đã chạy, thiết kế đề xuất. Chưa thay đổi engine sản phẩm.

## 1. Quyết định đề xuất

Tách hai cam kết sản phẩm: **Tối ưu giữ nguyên nội dung** và **Nén giữ chất lượng cảm nhận**. Chế độ đầu là mặc định an toàn; nếu không tìm được kết quả nhỏ hơn mà vẫn đạt điều kiện bảo toàn thì giữ bản gốc và trả trạng thái “không có cải thiện”. Chế độ thứ hai là lossy có kiểm chứng, phải thể hiện rõ cho người dùng.

Không có cấu hình có thể bảo đảm mọi file đều nhỏ đi mà giữ nguyên mọi bit. Với cùng một tập dữ liệu hữu hạn, ánh xạ một-một tất cả chuỗi đầu vào sang chuỗi ngắn hơn là bất khả thi. Video JPEG/HEVC/AV1 và ZIP vốn đã nén thường còn rất ít phần dư. Đổi container có thể giảm overhead nhưng không tự làm codec hiệu quả hơn. Mã hóa lossless giữ các mẫu đã giải mã, không phục hồi thông tin đã bị mất trước đó.

Mục tiêu tối ưu cụ thể: **giảm số byte, dưới ràng buộc nội dung, tương thích, thời gian, RAM và dung lượng tạm**. Không dùng tỷ lệ giảm cố định làm thước đo chất lượng.

## 2. Phạm vi rà soát

Đã rà các tệp C/header trong `src/`, `include/`, `tests/`; helper Python `src/scripts/teraconvert-rembg`; các Meson build, tùy chọn, script đóng gói, README, systemd và profile cấu hình. Đi sâu đường đi menu → IPC → dispatcher → worker → engine → file tạm → commit → HUD. Đây là review tĩnh và pilot theo phạm vi, không phải chứng nhận không còn lỗi.

Không coi binary đã build, gói `.deb`, icon và mã thư viện bên thứ ba là mã nguồn ứng dụng cần review. Skill tải về là hướng dẫn, không phải engine. Phụ lục liệt kê tệp rà soát; JSON pilot lưu hash nguồn để truy lại snapshot.

| Nhóm | Vai trò và nhận xét |
|---|---|
| Client C và header | Menu phân loại MIME, dispatch, launch UI; nhãn nén chưa khớp cam kết chất lượng |
| Common/IPC/logging | Chưa có mô hình policy chất lượng, kết quả “không cải thiện”, manifest stream hay báo cáo bảo toàn |
| Daemon/dispatcher | Socket activation; đọc request ngay trong main loop; thiếu deadline đọc; header có thể bị phân mảnh trên SOCK_STREAM |
| Worker/pool/header | File tạm cùng thư mục và rename là nền tảng tốt; size guard chỉ chứng minh file nhỏ hơn, chưa chứng minh nội dung nguyên vẹn |
| Governor/disk guard | API reserve/release chưa được gọi từ engine/worker; ước lượng dung lượng 4 MiB/file không phù hợp video lớn |
| FFmpeg engine | 906 dòng trộn probing, lựa chọn policy, GPU, dựng argv, chạy subprocess, telemetry; cần phân tách trách nhiệm |
| libvips engine | Converter ảnh hiện tại; chưa có nhánh optimizer lossless JPEG/PNG riêng |
| Poppler/document engine | PDF qua Ghostscript; Office unzip/zip; chưa kiểm định tính toàn vẹn tính năng tài liệu |
| AI C/Python | Tách nền là chỉnh sửa nội dung; không thuộc lossless compression; setup UI và runtime đang dùng hai tên mô hình khác nhau |
| UI C/header | HUD/resize/setup; cần thêm kết quả skip/quality và policy rõ ràng |
| Meson/tests/package | Ba nhóm test pass nhưng coverage chất lượng, đa stream và định dạng phức tạp còn thiếu; binary FFmpeg trong PATH khác binary engine thực dùng |
| data/profiles | Chưa thấy loader hoặc lệnh cài profile trong build; thay `.conf` hiện chưa thay đổi policy engine |

## 3. Những vấn đề cần sửa trước khi tối ưu tỷ lệ nén

### 3.1 Video và âm thanh

1. `tc_media_ffmpeg.c::build_ffmpeg_args`: `comp-web` lấy khoảng 55% bitrate tổng, cap 1.800 kb/s; `comp-saver` khoảng 38%, cap 1.400 kb/s; `comp-hq` khoảng 68%, cap 3.200 kb/s. Đây là heuristic bitrate, chưa phải lựa chọn theo độ phức tạp cảnh hoặc chất lượng. Bitrate nguồn tính từ cả file, gồm audio, container và các stream khác.
2. `comp-hq` và `comp-saver` scale chiều ngang tối đa 1920; `comp-web` tối đa 1280. Video 1440p/4K mất độ phân giải. Vì chỉ giới hạn chiều ngang nên cũng không phải giới hạn long-edge/chất lượng thống nhất cho video dọc.
3. Chưa thu thập đủ pix_fmt, bit depth, primaries, transfer, matrix/range, HDR side data, SAR/DAR, rotation, interlacing, VFR, stream dispositions. `format=nv12`/`yuv420p` có thể hạ bit depth/chroma; không được coi là giữ nguyên HDR hay 4:4:4.
4. Argv thiếu mapping tường minh tất cả track được yêu cầu. FFmpeg tự chọn stream có thể bỏ audio phụ, subtitle, attachment. Video đẹp nhưng mất phụ đề hoặc track tiếng vẫn là thất bại bảo toàn.
5. Audio không phải AAC thường bị mã hóa lại AAC 128/160 kb/s. Cần copy khi container hỗ trợ; thay container hoặc skip nếu yêu cầu giữ nguyên audio. WAV ép `pcm_s16le` không bảo toàn nguồn 24-bit/float.
6. GPU hiện probe H.264 rồi suy rộng sang HEVC; cần capability theo codec, bit depth, chroma và kích thước. CPU fallback không được âm thầm đổi các thuộc tính nội dung.
7. CRF, CQ, QP khác encoder không tương đương. Không dùng một công thức QP như thể nó là điểm chất lượng chung cho CPU/NVENC/VAAPI. Chọn GPU theo mục tiêu tốc độ và benchmark thực, không mặc định GPU luôn tạo file nhỏ hơn.
8. Các nhãn giảm ~70%/~40% và so sánh “chuẩn HandBrake” trong menu chưa có corpus chứng minh; nên bỏ cam kết tỷ lệ cố định.

### 3.2 Ảnh, PDF, Office

- Ảnh: `vips_webpsave Q=85`, JPEG Q=90 và AVIF Q=80 là các cấu hình converter có mất dữ liệu. Nén JPEG lossless cần giữ hệ số DCT qua jpegtran/transcode API; decode rồi encode Q=100 vẫn không đồng nghĩa lossless. PNG có thể tối ưu lọc/DEFLATE mà giữ mẫu ảnh. WebP lossless có giới hạn biểu diễn; cần xử lý alpha, metadata và bit depth trước khi chọn.
- `vips_image_new_from_file` không yêu cầu nạp tất cả trang/frame; cần chặn hoặc xử lý đúng ảnh động, TIFF nhiều trang. Không dùng bộ so sánh chỉ đọc trang đầu.
- PDF: `comp-pdf-print` thực tế dùng `/ebook`; `comp-pdf-web` dùng `/screen`. Preset có thể giảm độ phân giải/chất lượng ảnh và thay đổi cấu trúc. Ghostscript ghi rõ preset không phải bảo đảm “giữ nguyên” [S4]. Bản ký số, form, attachment, bookmark, OCR layer cần đường xử lý riêng.
- Office: `doc_compress_office` chỉ repack ZIP ở DEFLATE 9, không tối ưu ảnh nhúng như tooltip nói. File `.doc/.xls/.ppt` cũ không phải OOXML ZIP nên phải phân loại trước.
- **Lỗi Office đã tái hiện ở mức cơ chế:** worker tạo sẵn file đích rỗng bằng mkstemp(s); `zip -9 -q -r existing-empty.docx .` trả mã 3 “Zip file structure invalid”. Test engine hiện xóa file đích trước nên bỏ sót đường đi thật qua worker.
- Repack ODF cần giữ quy tắc `mimetype` đầu ZIP và không nén; zip đệ quy chung không bảo đảm điều này [S9]. Với OOXML/ODF, giữ nguyên byte của từng entry, relationships, macros, embedded media, và kiểm tra trạng thái chữ ký; không hứa bảo toàn chữ ký sau biến đổi không được kiểm chứng.

### 3.3 Các điều kiện nền tảng ảnh hưởng trực tiếp kết quả

- `tc_worker_pool.c`: batch tạo từ dispatcher bình thường chưa có đường giải phóng sau hoàn tất; dùng refcount cho batch và job, job_id cho hủy/tiến độ. Một `active_batch_ctx` và một listener chung chưa đại diện đúng nhiều batch đồng thời. Các số đếm vẫn có đường đọc/ghi thiếu cùng khóa; mutex con trỏ từ lượt sửa trước chưa giải quyết toàn bộ data race.
- `TC_OVERWRITE_SKIP` chưa có nhánh xử lý tương ứng trong worker; auto-number dùng check-then-rename có thể va chạm khi nhiều job cùng tên. Cần commit không ghi đè bằng renameat2(RENAME_NOREPLACE) khi phù hợp, retry tên mới; replace chỉ theo policy rõ ràng.
- Kết quả lớn hơn đầu vào hiện bị gán lỗi “tệp gốc đã tối ưu”. Đây chỉ chứng minh candidate đó không tốt hơn; không chứng minh mọi thuật toán đều không thể cải thiện. Cần `NO_BENEFIT` thay vì lỗi/hứa quá mức.
- Socket tiến độ dùng send đồng bộ có thể tạo backpressure trong worker; cần hàng đợi, nonblocking framing và giới hạn hàng đợi. Socket request đọc trong main loop có thể làm daemon treo bởi client gửi chậm, dù Nautilus đã tách process.
- Pool chạy đến số logical CPU và mỗi encoder dùng toàn bộ CPU → oversubscription. Governor hiện chỉ có API; `MemoryMax=4000M` là trần thực tế của service, không phải bảo đảm 1 GiB cho mỗi cây tiến trình.
- Kiểm tra disk cần theo mount của từng output và tổng temporary peak; đặc biệt khi encode + validate hoặc unzip. Cleanup theo tên và tuổi trong `/tmp` chưa có ownership/liveness đủ mạnh; sibling output temps cũng không khớp toàn bộ pattern cleanup.
- `tc_ui_setup_ai.c`: download chỉ mô phỏng, worker giữ con trỏ dialog có thể đã bị giải phóng khi người dùng đóng cửa sổ. Theo dõi riêng, không trộn vào lời hứa nén.
- `meson.build`: tests phụ thuộc libtc_engines dù có tùy chọn tắt daemon; engine_ai chưa được nối điều kiện build; FFmpeg source include libav headers ngoài guard. Cần ma trận build cấu hình tối thiểu trước khi mở rộng dependency.

## 4. Pilot đã thực hiện

Chạy `python3 tools/research_compression.py --output docs/research/compression-pilot-2026-10-23.json`.

- Ba clip tổng hợp SDR yuv420p 8-bit, 12 fps: chuyển động 640×360 CRF18 dài 3 giây, bản đã nén CRF35 dài 3 giây, và 2560×1440 CRF18 dài 1 giây. Không có audio, subtitle hoặc HDR.
- Bảy phương án/clip: engine HQ và saver thực tế; stream copy; x265 medium CRF20/24/28; x264 lossless. Gọi engine trực tiếp nên kết quả lớn hơn nguồn vẫn được đo, không qua size guard của worker.
- Công cụ dùng `/usr/bin/ffmpeg` 8.0.1 như engine, CPU i7-1185G7 8 logical CPUs. PATH còn có FFmpeg 7.0.2 khác; `/usr/bin/ffmpeg` không có libvmaf ở môi trường này. Pilot đo SSIM và hash rawvideo, chưa đo VMAF.
- Khi đầu ra đổi kích thước, upscale bằng bicubic về kích thước nguồn trước khi đo SSIM, để tính cả mất mát resize. Hash chỉ so mẫu video giải mã theo cùng yuv420p của fixture; không phải phép kiểm định container/metadata/audio tổng quát.
- Một lần chạy cuối, không phải benchmark thống kê nhiều lần; GPU nếu engine phát hiện sẽ được ghi trong diagnostic JSON. Startup/probe được tính trong thời gian gọi engine. Không lấy thời gian pilot làm SLA.

| Mẫu | Phương án | Giảm dung lượng (%) | SSIM | Kích thước đầu ra | Hash pixel khớp | Thời gian (s) |
|---|---|---:|---:|---|---|---:|
| motion | current_hq | 30.26 | 0.995167 | 640×360 | Không | 0.103 |
| motion | current_saver | 64.48 | 0.981905 | 640×360 | Không | 0.279 |
| motion | stream_copy | 0.00 | 1.000000 | 640×360 | Có | 0.031 |
| motion | x265_crf20_medium | -6.89 | 0.998393 | 640×360 | Không | 0.471 |
| motion | x265_crf24_medium | 20.48 | 0.996860 | 640×360 | Không | 0.459 |
| motion | x265_crf28_medium | 41.83 | 0.993893 | 640×360 | Không | 0.404 |
| motion | x264_lossless | -176.24 | 1.000000 | 640×360 | Có | 0.159 |
| already_small | current_hq | 18.79 | 0.956691 | 640×360 | Không | 0.084 |
| already_small | current_saver | 64.02 | 0.950685 | 640×360 | Không | 0.230 |
| already_small | stream_copy | 0.00 | 1.000000 | 640×360 | Có | 0.032 |
| already_small | x265_crf20_medium | -216.30 | 0.998018 | 640×360 | Không | 0.523 |
| already_small | x265_crf24_medium | -134.06 | 0.996790 | 640×360 | Không | 0.438 |
| already_small | x265_crf28_medium | -70.97 | 0.994795 | 640×360 | Không | 0.394 |
| already_small | x264_lossless | -1099.44 | 1.000000 | 640×360 | Có | 0.164 |
| resolution_check | current_hq | 52.96 | 0.985144 | 1920×1080 | Không | 0.233 |
| resolution_check | current_saver | 90.97 | 0.974613 | 1920×1080 | Không | 0.508 |
| resolution_check | stream_copy | 0.00 | 1.000000 | 2560×1440 | Có | 0.040 |
| resolution_check | x265_crf20_medium | -12.25 | 0.998793 | 2560×1440 | Không | 1.737 |
| resolution_check | x265_crf24_medium | 16.38 | 0.997544 | 2560×1440 | Không | 1.691 |
| resolution_check | x265_crf28_medium | 42.18 | 0.995171 | 2560×1440 | Không | 1.538 |
| resolution_check | x264_lossless | -138.93 | 1.000000 | 2560×1440 | Có | 0.596 |

Số âm ở cột giảm dung lượng nghĩa là file lớn hơn nguồn. Hai hàng current trên mẫu resolution_check đã bị giảm kích thước; SSIM được tính sau upscale như mô tả.

Diễn giải: stream copy không giảm dung lượng trên cả ba mẫu; lossless giữ rawvideo nhưng làm file phình. HEVC CRF24/28 giảm được một số mẫu, nhưng đều làm bản CRF35 lớn hơn. HQ/saver hiện có thể giảm byte rất nhiều bằng đánh đổi hình ảnh hoặc kích thước. SSIM cao không chứng minh mắt người không phân biệt, và SSIM giữa hai nội dung khác nhau không phải thước đo chất lượng tuyệt đối.

**Không suy ra tỷ lệ nén cho video điện thoại, phim, grain, screencast, HDR hay tài liệu thực từ pilot này.**

Bộ test hiện hữu chạy lại: **3/3 nhóm pass**, khoảng 4 giây. Điều này xác nhận baseline chức năng hiện tại, không phủ định lỗi Office worker hoặc những thiếu sót chất lượng ở trên. Pilot không tải mô hình hay media từ Internet.

## 5. Phương án nén theo loại file

| Loại | Giữ nguyên nội dung: mặc định | Nén cảm nhận: tùy chọn | Chặn/giữ gốc khi |
|---|---|---|---|
| Video đã nén | Remux/copy các stream và thuộc tính cần giữ; cải thiện overhead nếu có | Tìm CRF/CQ theo mẫu cảnh; HEVC/AV1/H.264 theo tương thích | Không nhỏ hơn, mất track, HDR chưa được hỗ trợ/kiểm định |
| Video raw/lossless | Thử FFV1 hoặc encoder lossless có hỗ trợ pixel format; xác minh từng frame | Chỉ khi người dùng chấp nhận lossy | Decoder đích không hỗ trợ hoặc file lớn hơn |
| PNG/APNG | Oxipng hoặc DEFLATE/filter search; bảo toàn ICC/gamma/alpha/frame [S6] | Chuyển WebP/AVIF theo nhu cầu; công bố đổi định dạng | Mất animation/profile/depth |
| JPEG | jpegtran `-copy all -optimize`, tùy chọn progressive nếu đích hỗ trợ [S5] | Encoder lossy với kiểm tra cảm nhận | Chưa giữ được orientation/ICC hoặc không tiết kiệm |
| WebP/TIFF | Chỉ dùng mode lossless và biểu diễn đủ các mẫu nguồn [S7] | Q/effort được hiệu chỉnh riêng | Lossless mode không đủ để giữ bit depth/pages/metadata |
| PDF | QPDF object streams, recompress Flate; không bật `--optimize-images` trong mode này [S3] | Ghostscript với downsample/JPEG tùy chọn minh bạch | Có chữ ký số/encryption/tính năng chưa hỗ trợ |
| DOCX/XLSX/PPTX | Repack từng ZIP entry; giữ nguyên nội dung entry và semantics package | Nén ảnh nhúng chỉ qua mode riêng | Signed/encrypted, legacy Office, không mở lại được |
| ODT/ODS/ODP | Repack có quy tắc mimetype/manifest của ODF [S9] | Nén ảnh nhúng qua mode riêng | Sai cấu trúc package hoặc bảo toàn chưa xác minh |
| PCM/FLAC audio | FLAC với sample format hỗ trợ; giữ sample rate, channels, bit depth | AAC/Opus chỉ khi chấp nhận lossy | Không bảo toàn PCM hoặc đích không hỗ trợ |
| TXT/CSV/log/file bất kỳ | ZIP/Zstd/7z tùy khả dụng, giải nén được byte-exact | Không áp dụng “chất lượng cảm nhận” cho dữ liệu chung | File đã nén không cải thiện; luôn công bố cần giải nén |

Nén thành archive không đồng nghĩa giữ khả năng mở trực tiếp bằng ứng dụng cũ. Chế độ native-format và archive phải tách biệt. “Lossless codec” cũng không có nghĩa toàn pipeline lossless nếu trước encoder đã đổi màu/bit depth hoặc bỏ metadata.

## 6. Kiến trúc đề xuất

Giữ GLib/GIO, C11 và daemon hiện tại. Thêm các module có trách nhiệm tách biệt:

1. **Probe:** `TcFileProbe`/`TcMediaProbe` thu thập loại file thật, manifest stream, codec, duration/timestamps, pixel format/depth, color/HDR, audio, metadata, signature/encryption và tổng kích thước.
2. **Policy:** `TcCompressionPolicy` mô tả mức bảo toàn, tương thích đích, ngân sách thời gian/RAM và mức tiết kiệm tối thiểu. Các bất biến phải có thể kiểm tra; không dùng chuỗi `comp-hq` làm toàn bộ policy.
3. **Planner:** chọn tập candidate hợp lệ; ghi rõ thuộc tính nào có thể đổi. Safe mode không được fallback sang lossy. CPU/GPU là backend của cùng contract, không tự thay contract.
4. **Runner:** quản lý subprocess, argv, process group, cancel có deadline SIGTERM→SIGKILL, giới hạn threads/RAM, telemetry theo phase, log và exit status.
5. **Validator:** so manifest, kích thước, decode integrity, điều kiện chất lượng và feature preservation. Quality gate phải chạy trước commit.
6. **Commit/result:** ghi file tạm, fsync khi cần độ bền, no-clobber/replace theo policy; trả saved bytes, changed properties, validator evidence, skip reason. Giữ bản gốc là mặc định.

Kết quả nên phân biệt `SAVED`, `NO_BENEFIT`, `UNSUPPORTED_SAFE_MODE`, `QUALITY_REJECTED`, `CANCELLED`, `FAILED`. Sửa IPC có versioning hoặc message mới, không âm thầm thay packed struct đang dùng. HUD thể hiện các phase “Phân tích / Nén / Kiểm tra”, không báo xong ngay khi FFmpeg kết thúc mà validator chưa chạy.

## 7. Thiết kế thử nghiệm tiếp theo

### 7.1 Corpus

Thu thập bộ mẫu được phép sử dụng, tối thiểu 20–30 clip, khoảng 10–30 giây/clip: camera tĩnh/chuyển động, mặt người, tóc/cỏ/nước, low-light/grain, screencast chữ nhỏ, gradient/animation; 720p/1080p/1440p/4K; landscape/portrait; H.264/HEVC/AV1; nguồn đã nén mạnh và nguồn chất lượng cao. Thêm bộ regression VFR, multi-audio/subtitle, attachment, 10-bit HDR, rotation, interlaced và file lỗi. Clip tổng hợp chỉ dành regression.

Ảnh: JPEG baseline/progressive, PNG alpha/16-bit/ICC, APNG, TIFF nhiều trang. PDF: scan ảnh, vector/text, OCR, font nhúng, form, bookmark, attachment, ký số/encrypted. Office: tài liệu thật có hình, bảng, công thức, macro, linked resources; legacy và ODF là loại khác, không gộp vào OOXML.

### 7.2 Ma trận video

Benchmark cùng clip và cùng điều kiện bảo toàn: baseline hiện tại; stream copy; x264 medium/slow; x265 medium/slow; SVT-AV1 nếu encoder khả dụng; GPU theo capability thực. Thử CRF ở các mức riêng mỗi encoder; không so sánh CRF24 của encoder A với CRF24 encoder B như thể cùng chất lượng.

Chọn theo đường biên Pareto giữa byte, chất lượng và thời gian, không chỉ chọn file nhỏ nhất. Ghi command, versions, hardware/driver, peak RSS/process tree, wall/CPU time, output size, fail/skip reason. Lặp ít nhất ba lần cho các candidate lọt vòng cuối và báo median/range; chạy cùng giới hạn threads. Tách startup/capability probe khỏi encode ổn định khi đánh giá tốc độ.

### 7.3 Chọn chất lượng cho mode cảm nhận

Ưu tiên giữ nguyên geometry/fps/pixel format/depth/màu và copy audio. Lấy mẫu từ nhiều cảnh, gồm cảnh khó, không chỉ đoạn đầu. Đánh giá 4–6 mức chất lượng, refine quanh vùng đạt mục tiêu, có ngân sách rõ; không cần encode cả video nhiều lần ngay từ đầu. Sau đó encode toàn bộ và validate.

VMAF + SSIM/PSNR + kiểm tra trực quan mù/A-B là các bằng chứng bổ sung [S8]. VMAF mean ≥97 và P5 frame ≥93 có thể là **ngưỡng thử nghiệm ban đầu**, không phải chuẩn lossless hay cam kết thị giác. Cần chọn đúng model/viewing condition, align frame/timestamp, báo cả cảnh tệ; HDR cần phương pháp riêng. Chưa đạt corpus/calibration thì không tự gắn nhãn “không giảm chất lượng”. Sampling không bảo đảm mọi frame; profile nghiêm ngặt cần kiểm tra toàn video hoặc giữ stream.

CRF20/24/28 trong pilot chỉ là điểm khảo sát. Không đề nghị hardcode CRF20 hoặc AV1/HEVC cho mọi nguồn. Nếu không tìm được candidate nhỏ hơn và đạt gate trong ngân sách, trả `NO_BENEFIT`.

### 7.4 Tiêu chí nghiệm thu

- Byte-exact archive: giải nén rồi SHA-256 từng file khớp; giữ đúng cấu trúc/permissions cần thiết.
- Lossless video/audio: sample hash từng frame/audio block khớp với cùng biểu diễn nguyên thủy; kiểm tra thêm timestamp, số frame, sample rate, channel layout, metadata và stream inventory. Không convert mọi thứ về 8-bit để “chứng minh” giữ nguyên HDR.
- Ảnh: decoded samples/alpha/pages/frame timing khớp, ICC/EXIF/gamma/orientation theo policy. Giữ opaque metadata nếu không hiểu, hoặc skip safe mode.
- PDF: structural check, page count, text/search/OCR, links/forms/bookmarks/attachments và render diff bằng cùng renderer. Render giống nhau không chứng minh các tính năng tương tác hoặc chữ ký còn nguyên.
- Office: tên entry và hash byte giải nén khớp, ZIP/package validation, mở trong LibreOffice và kiểm tra trên ứng dụng đích khi cần; ODF mimetype đúng. Nén ảnh nhúng không được đi qua gate hash-entry của safe mode.
- Size gate khởi đầu có thể yêu cầu giảm ≥1% và ≥64 KiB; đây là policy UX cần hiệu chỉnh, các file nhỏ cần ngưỡng riêng.
- Không ghi đè gốc ngoài policy; không bỏ track lặng lẽ; validator fail thì xóa candidate và giữ gốc. Lỗi disk/cancel/process kill không để output cuối bị hỏng.

## 8. Thứ tự triển khai

| Đợt | Công việc | Điều kiện hoàn tất |
|---|---|---|
| 1 — Độ đúng | Office empty-temp regression qua worker; batch refcount; overwrite; safe skip; bỏ cam kết % | Test worker Office thật, cancel, nhiều batch và no-clobber pass |
| 2 — Safe mode | Probe/manifest; stream copy có mapping; JPEG/PNG; QPDF; OOXML/ODF native optimization | Nội dung/feature invariants pass; no-benefit là kết quả hợp lệ |
| 3 — Đánh giá | Corpus, harness metrics, baseline CPU/GPU, rate-quality curves | Có số liệu và review thị giác; xác định Pareto candidates |
| 4 — Mode cảm nhận | Quality search theo mẫu cảnh + validate output; UI mục tiêu chất lượng và tương thích | Không đổi geometry/HDR/audio âm thầm; gate, fallback và cancel đúng |
| 5 — Tài nguyên | Scheduler video 1–2 job theo RAM/CPU thực, cache capability, threads budget, mount-aware disk | Máy còn phản hồi khi batch lớn; đo RSS và latency trên phần cứng đích |

Làm đợt 1 và safe video probing/remux trước. Không cần thay toàn bộ bằng một codec mới để bắt đầu có giá trị. Chưa ước lượng ngày công cứng khi corpus và mức hỗ trợ HDR/Office chưa được chốt.

## 9. Nguồn sơ cấp

- [S1 — FFmpeg streamcopy/stream selection](https://ffmpeg.org/ffmpeg.html): copy không decode/encode nên không tạo generation loss; mapping phải tường minh theo contract.
- [S2 — x265 rate control và preset](https://x265.readthedocs.io/en/master/cli.html), [preset](https://x265.readthedocs.io/en/master/presets.html), [lossless](https://x265.readthedocs.io/en/master/lossless.html): CRF không bảo đảm kích thước; lossless giữ mẫu dựng lại; preset đổi chi phí tính toán.
- [S3 — QPDF CLI](https://qpdf.readthedocs.io/en/stable/cli.html): object/Flate optimization; `--optimize-images` có thể đổi sang JPEG lossy, không dùng trong safe path.
- [S4 — Ghostscript PDFSETTINGS](https://ghostscript.readthedocs.io/en/latest/VectorDevices.html): các preset làm thay đổi đầu vào, không tương đương lossless.
- [S5 — libjpeg-turbo usage/jpegtran](https://github.com/libjpeg-turbo/libjpeg-turbo/blob/main/doc/usage.txt): tối ưu hệ số DCT và entropy coding; metadata cần `-copy` phù hợp.
- [S6 — Oxipng](https://github.com/oxipng/oxipng): optimizer lossless PNG/APNG; vẫn phải giữ các chunk ảnh hưởng hiển thị.
- [S7 — libvips save options](https://www.libvips.org/API/current/cpp/classVImage.html): quality/lossless/metadata/multipage là các lựa chọn riêng.
- [S8 — Netflix VMAF](https://github.com/Netflix/vmaf/blob/master/README.md), [FAQ](https://github.com/Netflix/vmaf/blob/master/resource/doc/faq.md), [model selection](https://github.com/Netflix/vmaf/blob/master/resource/doc/models_v1.md): metric cảm nhận và model/viewing assumptions; không thay hash/invariants.
- [S9 — ODF 1.3 Packages](https://docs.oasis-open.org/office/OpenDocument/v1.3/OpenDocument-v1.3-part2-packages.html): quy tắc packaging của ODF, đặc biệt mimetype.
- [S10 — NVIDIA NVENC guide](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/index.html): capability encoder tùy phần cứng và tính năng.

## Phụ lục: tệp nguồn và cấu hình đã rà

45 tệp; loại trừ build output, binary asset và skill cộng đồng.

- `src/client/meson.build`
- `src/client/tc_client.c`
- `src/client/tc_client.h`
- `src/client/tc_nautilus_ext.c`
- `src/daemon/meson.build`
- `src/daemon/tc_daemon.c`
- `src/daemon/tc_disk_guard.c`
- `src/daemon/tc_disk_guard.h`
- `src/daemon/tc_dispatcher.c`
- `src/daemon/tc_dispatcher.h`
- `src/daemon/tc_governor.c`
- `src/daemon/tc_governor.h`
- `src/daemon/tc_worker_pool.c`
- `src/daemon/tc_worker_pool.h`
- `src/engines/meson.build`
- `src/engines/tc_ai_birefnet.c`
- `src/engines/tc_doc_poppler.c`
- `src/engines/tc_engine.h`
- `src/engines/tc_engine_registry.c`
- `src/engines/tc_img_vips.c`
- `src/engines/tc_media_ffmpeg.c`
- `src/scripts/teraconvert-rembg`
- `src/ui/meson.build`
- `src/ui/tc_ui.h`
- `src/ui/tc_ui_hud.c`
- `src/ui/tc_ui_main.c`
- `src/ui/tc_ui_resize.c`
- `src/ui/tc_ui_setup_ai.c`
- `include/tc_common.h`
- `include/tc_ipc.h`
- `include/tc_logging.h`
- `tests/meson.build`
- `tests/test_e2e_conversion.c`
- `tests/test_ipc.c`
- `tests/test_worker_pool.c`
- `meson.build`
- `meson_options.txt`
- `build-deb.sh`
- `README.md`
- `data/systemd/teraconvert.service`
- `data/systemd/teraconvert.socket`
- `data/profiles/document.conf`
- `data/profiles/image.conf`
- `data/profiles/pdf.conf`
- `data/profiles/video.conf`
