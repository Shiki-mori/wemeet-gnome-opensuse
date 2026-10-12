# 腾讯会议在 GNOME Wayland 上的个人启动器

这是一套在 openSUSE Tumbleweed、GNOME Wayland 上使用官方 Linux 客户端的个人记录：启动脚本、菜单入口，以及对开源屏幕共享钩子的修改。

本仓库**不包含**腾讯会议安装包、官方客户端，或任何腾讯的专有二进制文件。客户端需要从腾讯会议官网或发行版软件源自行安装。

## 包含什么

| 路径 | 内容 |
| --- | --- |
| `wemeetapp` | 用户级启动脚本。开会走官方 X11，避免快速会议闪退 |
| `wemeet.desktop` | 应用菜单入口。其中的家目录路径需要改成你自己的 |
| `腾讯会议安装说明.md` | 这台机器上的安装与排查记录 |
| `wemeet-wayland-screenshare/` | 屏幕共享钩子源码，可在本机重新编译 |

## 第三方许可

钩子改自 [xuwd1/wemeet-wayland-screenshare](https://github.com/xuwd1/wemeet-wayland-screenshare)，原项目为 MIT 许可，版权归 David Xu（2024）。原 `LICENSE` 保留在 `wemeet-wayland-screenshare/LICENSE`。

`plthook.h` / `plthook_elf.c` 来自 [kubo/plthook](https://github.com/kubo/plthook)，BSD 许可，声明写在文件头部。

`stb/stb_image_resize2.h` 是 [nothings/stb](https://github.com/nothings/stb) 的公有领域文件，只收录了编译用到的这一份头文件。

## 编译钩子

依赖：`cmake`、`gcc-c++`、`libportal-devel`、`pipewire-devel`、`libX11-devel`、`libXrandr-devel`。不需要 OpenCV。

```bash
cd wemeet-wayland-screenshare
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

把生成的 `build/libhook.so` 放到 `wemeetapp` 同一目录后再启动。
