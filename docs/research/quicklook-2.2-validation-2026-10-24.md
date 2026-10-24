# TeraConvert 2.2 — Quick Look

Bản 2.2 bổ sung `teraconvert-preview`, ứng dụng GTK4/libadwaita riêng. Không thay đổi hợp đồng nén 2.1. Dịch vụ context menu là `org.gnome.TeraConvert.Preview`; provider Space là tùy chọn cấp người dùng, không thay file hệ thống của GNOME Sushi.

## Phạm vi đã triển khai

- Ảnh raster vừa cửa sổ, áp dụng orientation; văn bản chỉ đọc, tối đa 256 KiB; PDF phân trang, raster mỗi trang tối đa 1600 pixel. Ảnh tối đa 24 triệu pixel, file đọc nội tuyến tối đa 64 MiB. Định dạng khác hiển thị thông tin và nút Open.
- Audio/video dùng GtkMediaFile/GtkVideo và backend/codec hiện có; mặc định tạm dừng. Không gắn cam kết mọi codec đều hỗ trợ.
- Mũi tên trái/phải chuyển danh sách file truyền qua CLI. PgUp/PgDn chuyển trang PDF; Space/Escape đóng. Provider NautilusPreviewer2 phát SelectionEvent để Nautilus đổi vùng chọn khi dùng Space.
- Giao thức legacy ShowFile(sib) và NautilusPreviewer2 ShowFile(ssbs), Close, Visible/ParentHandle. Đã đối chiếu giao thức với resource Sushi cài trên máy. X11 foreign parenting/Wayland focus và activation vẫn cần nghiệm thu thực tế.
- Một loader nền tại một thời điểm; coalesce điều hướng nhanh, cancellation/generation loại kết quả cũ; media được tạm dừng khi chuyển file/đóng. Lời gọi preview từ Nautilus lấy bus và gửi yêu cầu bất đồng bộ.
- Nút Open chỉ mở ứng dụng mặc định sau thao tác người dùng. Remote URI, directory, SVG, preview Office/archives không nằm trong MVP. Native decoding có giới hạn bảo thủ nhưng không phải hard resource quota hay security sandbox.

## Kiểm tra thực chạy

- Full release: **16 nhóm OK, 1 nhóm SKIP, 0 FAIL**. Production compression IPC vẫn bị sandbox chặn socket.
- Loader Quick Look: 5 ca, **4 pass + 1 skip**: local-only/regular-file/cancel, Unicode text, text truncation/binary detection, PDF pagination đạt. Ảnh positive bị skip vì Glycin không truy cập được D-Bus trong môi trường này; không tính thành bằng chứng decode ảnh thành công.
- Loader ASan/UBSan: cùng **4 pass + 1 skip**, `detect_leaks=0`; không chứng nhận sạch leak, GUI lifetime hay thư viện hệ thống.
- UI-only, tất cả engine tắt kể cả Poppler: **8 nhóm OK**. Xác minh viewer build độc lập và không yêu cầu PDF backend khi bị tắt.
- CLI `--help`, Bash syntax, payload md5, binary preview khớp build, service không ghi đè Sushi và APT simulation đạt.
- `dbus-run-session -- true` thất bại khi bind socket với EPERM. **Chưa chạy GUI, Space/selection/focus trên Nautilus, phát media, enable/disable provider hoặc cài gói thật.**

[Validation JSON](quicklook-2.2-validation-2026-10-24.json) lưu kết quả nhóm/subtest skip và checksum package. Báo cáo 2.1 là snapshot riêng, không đại diện bằng chứng GUI của 2.2.

## Thử bản mới trên desktop

Gói: `packages/teraconvert-c_2.2.0+test.20261024.1_amd64.deb`.

```sh
./tools/install-local.sh packages/teraconvert-c_2.2.0+test.20261024.1_amd64.deb
teraconvert-preview ./photo.png ./document.pdf ./clip.mp4
teraconvert-quicklook-setup --enable
```

Chạy bằng tài khoản desktop, không prefix sudo. Đăng xuất/đăng nhập lại để nhả name của Sushi đang chạy; sau đó chọn file trong Nautilus, nhấn Space và thử các phím mũi tên. Để trở lại provider hệ thống: `teraconvert-quicklook-setup --disable`, rồi đăng xuất/đăng nhập. Helper không ghi đè user service khác có sẵn và không tự kill preview process.

Kiểm tra thêm: mở liên tục nhiều file/PDF, đóng khi đang tải, ảnh có EXIF, media thiếu codec, file hỏng/lớn, tắt Poppler, chuyển vùng chọn và activation/focus trên cả Wayland/X11. Zoom ảnh, thumbnails/directory navigation, Office/archive render và kiểm thử GUI tự động là phần tiếp theo; chưa quảng bá chúng như tính năng sẵn có.
