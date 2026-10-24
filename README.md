# TeraConvert-C

TeraConvert uses GNU C11, GLib/GIO and Meson for its Nautilus extension, conversion daemon, GTK4/libadwaita interface and JSON command-line client. Some backends use external tools and Python. Processing keeps the original file; a successful job publishes a separate output unless the user explicitly selects output replacement.

## Compression contracts

| Mode | What is checked | Current scope |
|---|---|---|
| `comp-padding` | Every retained byte and its offset | Remove only trailing `free`/`skip` atoms in supported MP4/MOV files. Internal padding stays. |
| `comp-remux` | Per-stream packet bytes, boundaries, rational timestamps/durations, flags, side data, codec properties and supported metadata; full output decode | Conservative self-contained MP4/MOV audio/video layouts. Chapters, other tracks, opaque/fragmented/external structures are skipped. |
| `comp-image` | PNG decoded filtered data and non-IDAT chunks; JPEG DCT coefficients, quantization tables, sampling and allowed metadata | Static PNG and 8-bit JPEG, at most 16 million pixels and 32 MiB input. APNG, unknown chunks/markers and unsupported signatures/features are skipped. |
| `comp-pdf-safe` | QPDF semantic object/decoded-stream graph, PDF version/pages, text and every page rendered at up to 1600 pixels | Static, unencrypted PDF, at most 128 pages, 20,000 objects and 512 MiB input. Forms, signatures, annotations and other active/embedded features are skipped. Render checks supplement the graph check; they are not full-resolution visual proof. |
| `comp-office` | Entry names, lengths, SHA-256 content manifests and container rules before/after repacking | OOXML and ODT/ODS/ODP. ODF keeps the first `mimetype` entry uncompressed with no extra field. Signed/encrypted packages, unsafe paths, links and duplicate entries are rejected. Limit: 10,000 entries, 1 GiB decoded content, 16 MiB total entry-name bytes. ZIP headers need not be identical; application-level compatibility still needs review. |
| `safe-flac` | Complete canonical decoded PCM hash/count, 16/24-bit depth, sample rate, known channel layout and exposed metadata | One integer PCM/FLAC audio stream, up to 8 channels; no cover art/chapters/groups. Float, unknown and other depths are skipped. This is an audio-sample contract, not byte preservation of every source-container feature. |

Compression candidates must also meet the savings policy. A smaller result is not guaranteed: already compact or unsupported inputs can produce **no benefit** or **unsupported**. Preservation modes never fall back to lossy encoding. `safe-flac` is a conversion and can produce a larger output.

The separate legacy presets `comp-web`, `comp-saver`, `comp-hq`, `comp-pdf`, `comp-pdf-web` and `comp-pdf-print` are **lossy**. They may change resolution, bit depth, frame rate, audio or other properties. Ordinary conversion to WAV/FLAC is also distinct from `safe-flac`. The `.conf` files in `data/profiles/` are legacy reference data; the daemon does not execute their command templates.

## Build and runtime

Build dependencies: Meson, Ninja, GCC with GNU C11, pkg-config, GLib/GIO >=2.74; Nautilus extension API 4 for the client; GTK >=4.10 and libadwaita >=1.4 for the UI. Optional engines use libvips >=8.12, Poppler GLib >=22, FFmpeg development libraries, libarchive >=3.6, zlib >=1.2.9 and libjpeg. libnotify is optional.

For a complete Debian/Ubuntu development environment:

```sh
sudo apt install meson ninja-build gcc pkg-config dpkg-dev \
  libglib2.0-dev libnautilus-extension-dev libgtk-4-dev libadwaita-1-dev \
  libvips-dev libpoppler-glib-dev libarchive-dev zlib1g-dev libjpeg-dev \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libnotify-dev \
  ffmpeg img2pdf ghostscript poppler-utils python3 python3-pil python3-pikepdf
meson setup build --prefix=/usr --buildtype=release
meson compile -C build
meson test -C build --print-errorlogs
```

Preservation media uses `/usr/bin/ffmpeg`. PDF uses the installed QPDF Python binding (`pikepdf`) and Poppler command-line validators. ODF uses Python's streaming ZIP writer after native validation. Missing runtime validators cause an explicit unsupported/error result. Capability bits describe compiled backends; they do not certify every runtime, codec or GPU.

Optional components are controlled by `client`, `daemon`, `ui`, `engine_vips`, `engine_ffmpeg`, `engine_poppler`, `engine_office`, `engine_safe_images` and `engine_ai`. Run isolated build/test configurations with:

```sh
python3 tools/check-build-matrix.py
```

## Install and test on the desktop

Build the complete desktop package on the distribution where it will run. ABI dependencies are derived from the actual binaries. Earlier packages remain available.

```sh
TC_PACKAGE_VERSION=2.1.0+test.20261023.1 ./build-deb.sh
./tools/install-local.sh packages/teraconvert-c_2.1.0+test.20261023.1_amd64.deb
```

Run the installer as your desktop user. It invokes sudo for apt, verifies installed payloads, restarts the user daemon/socket, checks the running daemon and negotiates IPC v2, then closes Nautilus so the extension can reload. Finish active jobs first and reopen Files afterwards. The same installer can select an older package for rollback; the desktop upgrade/downgrade must be verified on the target system.

Headless operations emit JSON lines with `batch_id`, phase, result counts, reason and saved bytes:

```sh
teraconvert-cli --doctor
teraconvert-cli --format comp-padding ./video.mp4
teraconvert-cli --format comp-image ./photo.png
teraconvert-cli --format comp-pdf-safe ./document.pdf
teraconvert-cli --format comp-office ./document.odt
teraconvert-cli --format safe-flac ./audio.wav
teraconvert-cli --cancel 12345
teraconvert-cli --listen 12345
```

The CLI defaults to auto-numbering; `--overwrite skip` and explicit `--overwrite replace` are available. SIGINT requests cancellation and waits for completion. IPC v2 has explicit envelopes/ACKs, bounded input, per-batch listeners/cancellation and a bounded recent-completion cache. The 48-byte v1 header remains supported for compatible legacy requests; ambiguous legacy batch controls do nothing. Update the extension, daemon, CLI and HUD together. New clients require v2 negotiation and do not silently switch to a legacy lossy path.

Run the production IPC check against installed binaries, using its own runtime and synthetic fixtures:

```sh
python3 tests/test_daemon_rpc.py --installed
python3 tools/prepare-functional-test.py
```

A production IPC test that cannot bind AF_UNIX or read real `SO_PEERCRED` exits **77 (skip)**. Dispatcher framing unit tests mock only credentials; this is separate evidence. The manual fixture generator and its `--verify DIRECTORY` command never classify missing outputs as passed.

## Resource and temporary-file handling

One media worker and up to four general workers use atomic admission for estimated RAM, CPU and per-filesystem disk space. The default cooperative RAM budget is 1 GiB; admission waits are cancellable and bounded to five minutes. Decoded image/PDF geometry and PCM expansion refine estimates before conversion. FFmpeg stages use small thread pools; x265 has a separate bounded pool. Inputs requiring more than the configured budget can be refused.

Helpers use a shared argv runner, process groups, cancellation, timeouts, pipe draining, bounded diagnostics and child reaping. Children have per-process address-space and per-file size limits. During helpers, sampled process-tree RSS, workspace growth and remaining disk headroom are checked. These sampled checks are **not** a hard aggregate RAM/disk quota; native in-process libraries cannot always be interrupted while computing. The systemd service has a separate `MemoryMax=4000M`. Direct engine calls bypass worker admission.

Each job owns a private workspace on the output filesystem. Locked binary records identify its directory/device/inode. Startup recovery removes only verifiable inactive workspaces; unknown leftovers and live jobs remain. This is process-crash recovery, not a guarantee against every power-loss window. Source identity is checked before publishing, and non-replacing publication uses a no-clobber hard link.

## Quality research

`tools/quality_benchmark.py` is a research tool. It searches bounded candidates, validates full registered short clips, measures SSIM/PSNR, optionally VMAF with an explicit model hash, records time/RSS/bytes, repeats the selected full encode, and exports blind worst-frame A/B images. It never replaces a source or enables a product preset. Current metrics require known 8-bit BT.709 limited-range YUV420; unknown/HDR contracts are refused.

`docs/research/quality-corpus-manifest.json` reserves 25 clips with source/license attribution. **Media paths and hashes remain unregistered** until real clips are supplied. Register each local clip, then benchmark into a new workspace:

```sh
python3 tools/quality_benchmark.py --manifest docs/research/quality-corpus-manifest.json \
  --register bbb-01 --path /absolute/path/short-clip.mp4
python3 tools/quality_benchmark.py --manifest docs/research/quality-corpus-manifest.json \
  --output /tmp/quality-report.json --work-dir /tmp/quality-experiment-unique
```

Default SSIM thresholds are research inputs, not calibrated guarantees. Reports remain `release_ready: false` until real-corpus coverage, human A/B review and release criteria are addressed. No measured perceptual-quality preset is exposed in the application yet.

## Optional AI

AI background removal uses the existing RMBG ONNX helper, NumPy/Pillow and ONNX Runtime. Automatic model download has been removed. Obtain a compatible model under its applicable license and verify its reference SHA-256 before registering it:

```sh
teraconvert-register-model /path/to/rembg.onnx --sha256 REFERENCE_SHA256
```

The registration tool refuses a mismatched checksum and requires explicit `--replace` for an existing model. The action is hidden without a local model and receipt; inference verifies the checksum again. A checksum is an integrity check, not model compatibility or inference validation.

See [the implementation roadmap](docs/research/compression-implementation-plan.md) and [validation record](docs/research/roadmap-validation-2026-10-23.md) for completed work, evidence and outstanding desktop/corpus gates.

License: GPL-3.0-or-later.
