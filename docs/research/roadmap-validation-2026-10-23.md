# Validation record — TeraConvert 2.1.0

Đợt triển khai roadmap ngày 2026-10-23. [JSON đi kèm](roadmap-validation-2026-10-23.json) lưu thời điểm UTC thực tế, SHA-256 của 133 file source/build/data/test/tool, tên/result từng nhóm test, commands của build matrix và kiểm tra package. Không có commit/push hoặc cài hệ thống mới trong đợt này.

## Thay đổi đã kiểm tra

- IPC v2 có envelope/ACK/capability, batch ID, min-saving policy, phase và structured result. Header v1 vẫn 48 byte. Client mới negotiate v2; HUD/listen/cancel theo batch, cached completion giới hạn, không chuyển ngầm sang legacy lossy. Có CLI JSON không tự mở HUD.
- POSIX worker queues/mutex/condition, batch refcount và bounded registration. Hủy job đang xếp hàng không phải đợi một video khác chạy xong. Quyết định hủy và publish được serialize. Output skip/auto-number không clobber; source identity được kiểm tra, gồm mọi nguồn merge.
- Admission RAM/CPU/disk theo output device, geometry/PCM estimates, child AS/FSIZE limits, sampled process-tree RSS/workspace/disk checks. Helper image/PDF/merge/AI dùng chung runner; workspace riêng và locked binary recovery records thay cho prefix/age sweep.
- Native coefficient-preserving JPEG, static PNG DEFLATE, QPDF semantic/text/page-render checks, ODF mimetype rules và SHA entry validation, 16/24-bit canonical PCM checks, conservative MP4/MOV remux packet/timing/metadata validation. Feature chưa xác minh không fallback lossy.
- AI download mô phỏng đã được bỏ. Model registration dùng local input + reference SHA-256, explicit replace và no-clobber. Chưa chạy inference với model thật.
- README/menu/AppStream/package đồng nhất scope và giới hạn. Install support files theo Meson feature; Python/helper/runtime dependencies được công bố.

## Kết quả thực chạy

| Cấu hình | Kết quả | Giới hạn |
|---|---|---|
| Full desktop release | **15 nhóm OK, 1 nhóm SKIP, 0 FAIL** | Production AF_UNIX/SO_PEERCRED integration bị EPERM; AI inference opt-in có subtest skip. |
| ASan + UBSan | **15 nhóm OK, 1 nhóm SKIP, 0 FAIL** | `detect_leaks=0`; không chứng nhận sạch leak hoặc thư viện hệ thống. RSS/AS child-cap subtest skip vì ASan giữ vùng địa chỉ ảo lớn; normal build đã chạy ca này. |
| TSan worker + governor | **2 nhóm OK, 25 ca (12 + 13), không suppression** | Chỉ chứng cứ các đường concurrency của fixture được chạy; không chứng nhận mọi codec/helper/system library. |
| CLI-only | **7 nhóm OK** | Không daemon/client/UI/backends. |
| Daemon-core | **7 nhóm OK** | Optional engines tắt. |
| Client-only | **7 nhóm OK** | Compile Nautilus extension; không chạy menu trên desktop. |
| UI-only | **7 nhóm OK** | Compile GTK helper; không nghiệm thu tương tác GUI. |
| Safe-images-only | **8 nhóm OK** | PNG/JPEG backend độc lập. |
| Without FFmpeg | **10 nhóm OK** | Không tham chiếu libav target bị tắt. |
| Without Office | **13 nhóm OK** | Không tham chiếu libarchive target bị tắt. |

Nhóm fault/regression bao gồm: 16 ca padding/atom, 11 ca IPC, 12 ca worker lifetime/output, 13 ca governor, 8 ca dispatcher, 11 ca media probe, 7 ca Office/ODF, 12 ca runner, 4 ca image preservation, 4 ca PDF, 3 ca audio, 2 ca remux và 2 ca owned temp recovery. Ngoài các ca TAP này, bộ engine/worker hiện hữu cũng chạy trong full suite. Hai audio track trong remux, low bits 24-bit audio, ODF ký số/mã hóa, JPEG lỗi, source mutation và cancel-after-write đều được kiểm tra.

Sau sửa bộ đếm admission khi nhiều worker hoàn tất đồng thời, bộ worker được chạy **lặp 30 lần**, tất cả đạt, gồm kiểm tra trả đủ 10.000 slot cho batch kế tiếp. Đây không phải benchmark tương tác desktop.

Lệnh tái hiện phần chính:

```sh
meson test -C build --print-errorlogs
python3 tools/check-build-matrix.py

meson setup /tmp/tc-asan-new . -Dclient=false -Dui=disabled -Dengine_ai=disabled \
  -Db_sanitize=address,undefined -Db_lundef=false --buildtype=debug
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  meson test -C /tmp/tc-asan-new --print-errorlogs

meson setup /tmp/tc-tsan-new . -Dclient=false -Dui=disabled -Ddaemon=false \
  -Dengine_vips=disabled -Dengine_ffmpeg=disabled -Dengine_poppler=disabled \
  -Dengine_office=disabled -Dengine_ai=disabled -Dengine_safe_images=disabled \
  -Db_sanitize=thread -Db_lundef=false --buildtype=debug
TSAN_OPTIONS=halt_on_error=1 meson test -C /tmp/tc-tsan-new \
  'Worker Lifetime and Output Safety' 'Resource Admission Regression' --print-errorlogs
```

Dùng thư mục build mới cho các lệnh `meson setup`. Kết quả và compiler flags cụ thể của đợt chạy này nằm trong JSON và các build logs được dẫn ở đó. Hệ thống hiện tại có GCC 15.2, GLib 2.88, FFmpeg 8.0.1 và pikepdf 10.3; không suy ra các phiên bản/distro khác đã được nghiệm thu.

## Research harness

[Quality harness pilot](quality-harness-pilot-2026-10-23.json) là **một clip tổng hợp 96×64, 10 fps, một giây**, với SSIM target 0.9 cố ý nới để kiểm tra đường search/repeat/A-B. CPU libx264/libx265/libsvtav1 đã encode và validate repeat; VAAPI/NVENC trial thất bại trong môi trường này và có reason/command trong report. Clip thiếu primaries/transfer bị từ chối trước metric. Đã ghi source/output bytes, thời gian, peak child RSS, SSIM/PSNR, minimum per-frame SSIM và hash repeat. Không có VMAF model/filter đã được nghiệm thu.

Pilot chỉ xác minh harness chạy được, **không chứng minh chất lượng cảm nhận**, không thay thế corpus 20–30 clip. GNU time RSS là maximum child-process RSS, không phải tổng peak RSS đồng thời của process tree. Các đường dẫn `/tmp` trong report là vị trí thí nghiệm, không phải corpus phát hành; generation command và source hash được lưu để tái lập.

[Corpus manifest](quality-corpus-manifest.json) hiện có **0 clip thật đã đăng ký** trong 25 vị trí dự kiến. Source/license/credit có trong catalog; path, hash và scene review vẫn chờ dữ liệu. DNS lookup từ môi trường thực thi bị lỗi khi thử tải nguồn công khai. Không thay thế bằng số liệu synthetic rồi đánh dấu Q01/Q03/Q05 hoàn tất.

Model registration đã được thử bằng fixture byte có giới hạn để kiểm tra checksum, no-clobber và giữ model/receipt hiện hữu khi checksum sai. Fixture này **không phải ONNX model inference hợp lệ**.

## Package và trạng thái cài đặt

Gói: `packages/teraconvert-c_2.1.0+test.20261023.1_amd64.deb`.

SHA-256:

```text
808abbb5cb1bbc1dbf62246ba0d8b23016f207d57981d9ccc4fd6fd19c73b45a
```

Đã kiểm tra payload/md5sums, shell/Python syntax, ELF dependencies trên build host, component binary khớp build, AppStream (`--no-net`) và desktop-file validation. `apt-get -s install --reinstall` cho gói mới và gói cũ đều exit 0. Private payload overlay theo chiều upgrade rồi downgrade khớp hash/mode; **không thực thi maintainer scripts hoặc giao dịch apt/dpkg thật**. `dpkg-shlibdeps` có warning về usr-merge loader diversion; đây không phải bằng chứng package đã portable sang distro khác.

Phiên bản hệ thống đọc lại vẫn là **2.0.0+test.20261023.2**. Code/gói 2.1.0 chưa cài lên hệ thống. Môi trường hiện tại không cho ghi `/usr`/nâng quyền và không truy cập được user session bus; không giả lập việc cài bằng cách chỉ ghi đè staging.

Chạy trong terminal desktop của người dùng:

```sh
./tools/install-local.sh packages/teraconvert-c_2.1.0+test.20261023.1_amd64.deb
python3 tests/test_daemon_rpc.py --installed
python3 tools/prepare-functional-test.py
```

Production integration dùng socket riêng, credentials thật và synthetic fixtures, không nối vào daemon làm việc của người dùng. Cần xác minh thêm GUI/HUD/menu, nhiều batch/hủy, mở kết quả bằng ứng dụng đích, cài nâng cấp/downgrade thật, corpus và đánh giá A/B, cùng workload/RSS/latency trên phần cứng đích. Các gate còn lại được giữ rõ trong [roadmap](compression-implementation-plan.md).
