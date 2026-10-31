# WCAS 依赖目录

本目录是 CMake `FetchContent` 的依赖基目录（`FETCHCONTENT_BASE_DIR`）。首次配置
WCAS 时，如果系统没有安装 EPCD、ABIX、cmdly 或 libdrmtap 的 CMake 包，CMake 会通过
SSH 从 GitHub 克隆对应仓库到这里，并在本目录生成 FetchContent 的源码、构建和
subbuild 目录。

除本 `README.md` 外，本目录内容均被 `.gitignore` 忽略，**不会被提交或追踪**。可以
随时删除这些克隆目录以强制重新获取依赖；不要在本目录内修改依赖源码。

常用目录示例：

- `abix-src/`、`epcd-src/`、`cmdly-src/`、`libdrmtap-src/`：依赖源码
- `abix-build/`、`epcd-build/`、`cmdly-build/`：FetchContent 构建树
- `*-subbuild/`：FetchContent 的下载/更新子构建

libdrmtap 是 Meson 项目，`libdrmtap-src/` 会在这里，但它的 Meson 构建目录仍位于
WCAS 的构建目录（例如 `build/Debug/libdrmtap-build`）。

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
