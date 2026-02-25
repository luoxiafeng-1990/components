## Cursor Cloud specific instructions

### Project overview

C++17 embedded video processing framework (Component Framework) targeting RISC-V 64-bit via Buildroot. Uses GNU Autotools (autoconf + automake + libtool) as build system.

### Build dependencies

System packages required: `autoconf automake libtool liburing-dev liblog4cplus-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev pkg-config g++`

FFmpeg dev libraries are pre-installed in the VM image.

### Stub library: libtacosys

`Makefile.am` links against `-ltacosys` (a proprietary embedded platform library), but no source file actually uses any symbols from it. A stub shared/static library is installed at `/usr/local/lib/stubs/` and registered via `/etc/ld.so.conf.d/stubs.conf`. This allows the linker to resolve `-ltacosys` on x86_64 without the actual embedded SDK.

### Build commands

```bash
autoreconf -i
LDFLAGS="-L/usr/local/lib/stubs" ./configure --enable-debug
make -j$(nproc)
```

### Known pre-existing compilation issues on main branch

1. `BufferFillingWorkerFacade.cpp` has source/header API mismatch (references `use_buffer_mode`, `config_.file.file_path`, 4-arg `open()`, and `int getBytesPerPixel()` — all renamed/refactored in headers to `datasource_buffer_mode`, `data_source.path`, 0/1-arg `open()`, and `double getBytesPerPixel()`).
2. `FfmpegDecodeRtspWorker.cpp` and `FfmpegDecodeVideoFileWorker.cpp` require `taco_sys_api.h`, a proprietary header only available on the target RISC-V embedded platform.

These issues are fixed on the `dev` branch (major refactoring with 44k+ lines changed).

22 out of 25 source files compile successfully on x86_64 with the native toolchain.

### Runtime limitations

The `display_test` binary requires a Linux framebuffer device (`/dev/fb0`) and is designed for embedded hardware. It cannot be fully run in cloud VMs without framebuffer emulation.
