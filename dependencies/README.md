# WCAS 依赖目录

本目录保存 CMake `FetchContent` 在系统没有安装对应 CMake 包时克隆的依赖源码。每个
依赖使用与仓库同名的目录，结构与 AMC 的 `dependencies/` 一致：

```text
dependencies/
├── ABIX/          # https://github.com/Sukanle/ABIX.git（含 MICS 子模块）
├── EPCD/          # https://github.com/Sukanle/EPCD.git
├── cmdly/         # https://github.com/Sukanle/cmdly.git
├── libdrmtap/     # https://github.com/fxd0h/libdrmtap.git
├── tomlplusplus/  # 仅在系统未安装 toml++ 时出现
└── README.md
```

首次配置 WCAS 时，如果系统没有安装 EPCD、ABIX、cmdly 或 libdrmtap 的 CMake 包，
CMake 会通过 SSH 从 GitHub 克隆对应仓库到上面对应的目录。FetchContent 的构建树和
subbuild 目录不再放在这里，而是位于 WCAS 构建目录下的 `_deps/`（例如
`build/Release/_deps/abix-build`）；不要把它们和依赖源码混在一起。

除本 `README.md` 外，本目录内容均被 `.gitignore` 忽略，**不会被提交或追踪**。可以
随时删除某个依赖目录（或整个目录）以强制重新获取；已存在的检出会被直接复用，不会
在每次 configure 时自动更新或切换版本。

libdrmtap 是 Meson 项目，`libdrmtap/` 只保存源码；它的 Meson 构建目录仍位于
WCAS 的构建目录（例如 `build/Release/libdrmtap-build`）。

可通过以下 cache 变量覆盖依赖位置或版本：

```sh
cmake -B build/Debug -S . \
  -DWCAS_DEPENDENCIES_DIR=/path/to/wcas-dependencies \
  -DWCAS_ABIX_REPOSITORY=git@github.com:Sukanle/ABIX.git \
  -DWCAS_EPCD_REPOSITORY=git@github.com:Sukanle/EPCD.git \
  -DWCAS_CMDLY_REPOSITORY=git@github.com:Sukanle/cmdly.git \
  -DWCAS_LIBDRMTAP_REPOSITORY=git@github.com:fxd0h/libdrmtap.git
```

默认 libdrmtap 使用上游 `fxd0h/libdrmtap`；如果已有 `Sukanle/libdrmtap` fork，覆盖
`WCAS_LIBDRMTAP_REPOSITORY` 即可。

在 CI 上可以缓存本目录，缓存 key 使用解析后的 ABIX、EPCD、cmdly 和 libdrmtap
版本。
