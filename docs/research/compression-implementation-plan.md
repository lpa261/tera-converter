# Roadmap tối ưu nén TeraConvert

Cập nhật đợt 2026-10-23, phiên bản nguồn **2.1.0**. Phạm vi là giảm dung lượng có kiểm tra tính toàn vẹn, giữ bản gốc, rồi nghiên cứu chất lượng cảm nhận trên dữ liệu có nguồn gốc. Không có phương pháp bảo đảm mọi file đều nhỏ hơn mà không mất thông tin.

**Phần triển khai khả thi trong workspace đã được thực hiện. Roadmap chưa đạt toàn bộ gate phát hành:** còn corpus thật, hiệu chỉnh chất lượng và đánh giá A/B, IPC/GUI trên desktop thật, đo tải trên phần cứng đích, cùng cài nâng cấp/downgrade thực tế. Các phần này không được đánh dấu hoàn thành bằng test tổng hợp hoặc kết quả skip.

Báo cáo lệnh, phạm vi và giới hạn: [validation record](roadmap-validation-2026-10-23.md). Kết quả máy đọc được: [validation JSON](roadmap-validation-2026-10-23.json). Nghiên cứu và pilot cũ được giữ làm snapshot lịch sử, không dùng số đo đó để quảng bá bản 2.1.0.

## Theo dõi toàn bộ 30 hạng mục

“Đã triển khai” dưới đây chỉ nghĩa là có code và kiểm thử được nêu; không thay thế các gate tích hợp hoặc nghiệm thu ngoài môi trường này.

| ID | Trạng thái | Kết quả / phần còn lại |
|---|---|---|
| B01 | Đã triển khai | Inventory/hash, phiên bản tool, test logs, report JSON và lệnh tái hiện. Test engine, IPC framing, integration và research có phạm vi riêng. |
| B02 | Đã triển khai | Fixture độc lập trong thư mục riêng; không lấy dữ liệu người dùng ngoài dự án, không tự tải model. AI inference là opt-in. |
| B03 | Đã triển khai | Regression Office qua worker, trùng tên, hủy, nguồn đổi, fragmented IPC, listener chậm, nhiều batch và replay completion. |
| S01 | Đã triển khai | Office ghi vào candidate do worker sở hữu. OOXML/ODF có kiểm tra entry trước/sau; regression qua worker đạt. Mở bằng Office/LibreOffice thật còn là nghiệm thu đích. |
| S02 | Đã triển khai; còn tích hợp desktop | Refcount batch, POSIX worker scheduler, khóa/snapshot, định danh 64-bit, hủy/listener theo batch, giới hạn 64 batch và 10.000 job đang được giữ chỗ. TSan worker/governor không suppression đạt trong phạm vi fixture; không chứng nhận race trong mọi thư viện hệ thống. |
| S03 | Đã triển khai | Skip trước encode, auto-number/skip không clobber, replace tường minh; serialize quyết định hủy với publish. Test collision, symlink, source-change và cancel-after-write đạt. |
| S04 | Đã triển khai; còn IPC thật | Intake incremental, deadline/buffer/FD limits, sealed memfd, bounded writes, v2 incremental reader và UTF-8 bounds. Unit framing mock riêng credentials; production AF_UNIX/SO_PEERCRED bị môi trường chặn. |
| S05 | Đã triển khai | Workspace riêng cùng filesystem; binary record giữ flock, UID/device/inode; recovery chỉ xóa workspace inactive xác minh được. Test live/abandoned/unknown/symlink đạt. Không cam kết mọi cửa sổ mất điện. |
| C01 | Đã triển khai | Typed policy/result/phase; phân biệt created, no-benefit, unsupported, existing, cancelled, failed; enum quality-rejected dành cho đường nghiên cứu sau này. Ngưỡng tiết kiệm bytes/ppm kiểm tra tràn số. |
| C02 | Đã triển khai trong scope | Allowlist action–format/dimensions; media snapshot mọi stream/metadata/chapters và thuộc tính màu/depth/timing; packet manifest cho remux; parser riêng PNG/JPEG/PDF/Office. Feature không xác minh được bị từ chối. |
| C03 | Đã triển khai | Runner dùng argv, process group, timeout/cancel TERM→KILL, drain pipes, diagnostics, reap child. PDF/image/merge/AI dùng chung runner với media/probe. |
| C04 | Đã triển khai giới hạn bảo thủ; cần hiệu chỉnh | Admission CPU/RAM/disk thực, refined geometry/PCM estimates, live workspace/disk/RSS checks, child AS/FSIZE limits. Test simulated pressure và actual child growth đạt. Chưa là hard aggregate quota; chưa đo SLA/RSS/desktop đích. |
| C05 | Đã tách planner | `tc_media_plan.c/.h` nhận metadata/capability/binary tường minh, tách khỏi runner. Legacy conversion regression đạt; CPU/GPU legacy vẫn có chính sách khác nhau và vẫn lossy. Safe engines dùng validator riêng, không qua legacy fallback. |
| C06 | Đã triển khai; cần nghiệm thu migration | Header v1 48 byte giữ nguyên; envelope, ACK/capability, ID và result v2. Client v2 negotiate trước submit, không fallback ngầm; legacy controls chỉ chạy khi không mơ hồ. Cần cập nhật cả extension/daemon/CLI/HUD cùng gói và thử trên desktop. |
| L01 | Đã triển khai MVP bảo thủ | Byte-preserving trailing padding; validated MP4/MOV stream copy với hash packet/boundary/timing/flags/side-data, supported semantic atoms/properties, full decode. Hai audio stream được kiểm thử. Chapters/subtitle/attachments/opaque/fragmented layouts chưa được remux; không fallback lossy. |
| L02 | Đã triển khai MVP | PNG recompress DEFLATE với CRC và dữ liệu lọc/metadata bất biến; JPEG coefficient/Huffman optimize, quantization/sampling/allowed metadata bất biến. Fixture RGB/RGBA/16-bit/palette/progressive/EXIF và corruption/alias/cancel đạt. |
| L03 | Đã triển khai MVP | QPDF qua pikepdf, không JPEG re-encode; semantic graph/stream hash, page/version, text và render mọi trang trong giới hạn. Encryption/signature/form/annotations/unknown features skip; missing helper báo rõ. Cần corpus PDF thật và nghiệm thu app đích. |
| L04 | Đã triển khai MVP | OOXML native repack; ODF native pre/post validation + streaming Python ZIP writer, đúng mimetype đầu/stored/no-extra. Entry content hashes, name/count/expanded limits, signed/encrypted/path/link/duplicate rejection. Mở bằng ứng dụng đích còn chờ. |
| L05 | Đã triển khai MVP | `safe-flac`: 16/24-bit integer PCM/FLAC, known layout, hash toàn PCM s32 chuẩn hóa, sample count/rate/depth/layout và exposed metadata. Float/unknown/other depths/cover art/chapters skip; có fixture 24-bit với low bits khác 0. Không cam kết mọi opaque container feature. |
| L06 | Đã triển khai; cần GUI thật | HUD v2 hiển thị mode, phase, counts riêng, lý do và saved bytes; hủy đúng batch, không báo thành công khi disconnect. Menu phân biệt bảo toàn nội dung và lossy. Chưa click/quan sát HUD trên desktop thật. |
| Q01 | Chưa đạt gate dữ liệu | Có manifest 25 vị trí clip kèm nguồn/license/credit, công cụ đăng ký SHA-256. Chưa có 20–30 clip thật, ảnh/tài liệu thật và phân loại cảnh. Tải corpus bị lỗi DNS; không dùng synthetic thay thế. |
| Q02 | Đã có harness; cần chạy corpus | Full short-clip frame/timestamp/audio checks, lossless reference verified, SSIM/PSNR và optional VMAF với explicit model hash, time/RSS/bytes/commands. SDR-only; HDR/unknown color bị từ chối. Pilot chỉ xác minh công cụ; VMAF chưa có filter/model để nghiệm thu. |
| Q03 | Đã có search/repeat; benchmark thật chưa đạt | CPU x264/x265/SVT-AV1 pilot và final repeat đạt trên fixture. VAAPI/NVENC thử encode thật nhưng lỗi trong môi trường hiện tại; không suy capability từ H.264. Chưa có đường biên chất lượng–bytes–time trên corpus/hardware đích. |
| Q04 | Đã có prototype nghiên cứu; chưa bật sản phẩm | Bounded quality search theo toàn clip đăng ký, kiểm tra mọi frame/output và repeat, source hash trước/sau, size/metric gate. Chưa có scene sampling/corpus calibration hay release policy đủ căn cứ để tích hợp daemon. Không thêm preset “không giảm chất lượng cảm nhận” vào UI. |
| Q05 | Chưa đạt gate chủ quan | Harness xuất cặp worst-frame A/B và reviewer key riêng. Chưa có người đánh giá, tiêu chí tương thích/tốc độ/chất lượng đã hiệu chỉnh hoặc UI sản phẩm cho mode này. Đây là bước phụ thuộc Q01–Q04. |
| R01 | Đã kiểm tra build matrix | Full desktop, CLI-only, daemon-core, client-only, UI-only, safe-images-only, without-FFmpeg và without-Office build/test được. Install support files theo feature; ABI deps lấy từ binary package. |
| R02 | Đã đồng nhất tài liệu hiện hành | README/menu/AppStream/package mô tả đúng backends, limits, Python/runtime, mode lossy, không hứa % giảm hoặc SLA. Profile `.conf` ghi rõ legacy reference, không phải cấu hình thực thi. |
| R03 | Đã sửa setup; inference còn opt-in | Bỏ download mô phỏng/thread/dialog lifetime cũ; helper thống nhất RMBG, dùng local model+SHA receipt, bounded CPU pools. Registration checksum/no-clobber/wrong-checksum preservation được kiểm thử; chưa có model hợp lệ để chạy inference thật. |
| R04 | Đã có fault regression; gate tải đích chưa đạt | Disk/admission errors, child kill/timeout, RSS/output/workspace growth, malformed/alias/source mutation/cancel, queue bounds và listener chậm đã có test. Chưa đo p95 latency/interaction desktop hay process-tree peak trên workload/corpus thật. |
| R05 | Có gói và rollback artifact; chưa nghiệm thu cài thật | Package 2.1.0 test, staged payload/ABI/dependencies/scripts/checksums, feature flags và lệnh test installed đã chuẩn bị. Cài nâng cấp/downgrade, IPC/GUI và ứng dụng đích vẫn cần chạy ngoài sandbox. Phiên bản 2.0.0+test.20261023.2 đã cài trước đó không đại diện code mới. |

## Hợp đồng và giới hạn bản 2.1.0

Bản gốc được giữ lại. Compression chỉ publish candidate đã đạt validator và ngưỡng tiết kiệm. Không hỗ trợ hoặc không có lợi ích là kết quả bình thường có lý do. `safe-flac` là chuyển đổi bảo toàn mẫu âm thanh nên có thể tạo file lớn hơn. Legacy lossy presets không dùng kết quả synthetic làm cam kết chất lượng.

Ngân sách mặc định: 1 GiB RAM estimate, tối đa một media job, tối đa bốn general worker, CPU units theo giới hạn nhỏ; disk reservation theo device có 20% margin và 200 MiB headroom. Image/PDF geometry và PCM expansion refine estimate sau admission ban đầu. Giới hạn AS/FSIZE là per process/file; kiểm tra RSS/workspace là sampled. Native libraries không luôn có cancellation bên trong computation; systemd MemoryMax là lớp khác. Không mô tả những cơ chế này thành quota cứng toàn job hoặc bảo đảm desktop không giật.

PNG: input 32 MiB, decoded filtered data 64 MiB, 16 Mpixel. JPEG: 8-bit, input 32 MiB, 16 Mpixel; chỉ marker/metadata được hỗ trợ. PDF: 512 MiB input, 128 trang, 20.000 object, decoded stream/canonical limits; render so sánh ở kích thước giới hạn. Office: 10.000 entry, 1 GiB expanded, path tối đa 4096 byte, tổng entry names 16 MiB. Remux: known self-contained MP4/MOV AV tracks; không chứng nhận mọi format FFmpeg đọc được.

## Gate tiếp theo có thể tái hiện

1. Cài gói 2.1.0 trong desktop session bằng `tools/install-local.sh`, chạy `teraconvert-cli --doctor` và `python3 tests/test_daemon_rpc.py --installed`. Kiểm tra menu/HUD, hủy nhiều batch và mở kết quả bằng app đích. Chạy lại với gói 2.0.0 trước đó để nghiệm thu rollback.
2. Đăng ký 20–30 clip thật trong manifest: path, SHA-256, nguồn/license/credit, loại cảnh/codec/depth và lý do chọn. Thêm ảnh/tài liệu thật cùng các trường hợp biên; không tự lấy file cá nhân ngoài phạm vi dự án.
3. Chạy `quality_benchmark.py` trên corpus và phần cứng đích; ghi rõ GPU nào thực sự encode được, metric/model nào có. Đánh giá A/B mù và các cảnh tệ nhất; giữ nguyên các trường hợp quality-rejected/no-benefit.
4. Đo workload nhiều batch với validators đang hoạt động, RSS/process tree và tương tác desktop. Hiệu chỉnh budgets/thresholds theo số đo; chỉ sau đó hoàn thiện scene policy và UI chất lượng/tốc độ/tương thích cho sản phẩm.

Các gate này cần dữ liệu, phần cứng, quyền hệ thống hoặc đánh giá của người dùng. Không thể kết luận thay bằng việc compile thành công hay bỏ qua test bị môi trường chặn.
