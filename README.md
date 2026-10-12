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

本机实际运行目录是 `/home/phrolova/Applications/TencentMeeting`。本仓库的修改需要同步到该目录下的 `wemeet-wayland-screenshare`，在那里编译，再将 `build/libhook.so` 安装到 `/home/phrolova/Applications/TencentMeeting/libhook.so`。彻底退出腾讯会议（包括托盘）后，从应用菜单重新打开才能加载新库。

## 回归检查

可选检查需要 Python 的 PyGObject（`gi`）以及 `dbus-run-session`：

```bash
cmake -S wemeet-wayland-screenshare -B wemeet-wayland-screenshare/build \
  -DCMAKE_BUILD_TYPE=Release -DWEMEET_BUILD_TESTS=ON
cmake --build wemeet-wayland-screenshare/build -j"$(nproc)"
ctest --test-dir wemeet-wayland-screenshare/build --output-on-failure
```

门户检查在独立 D-Bus 上模拟创建失败、选屏取消和请求未完成时停止；不会打开真实选屏或会议。X11 检查读取自建的 8×8 pixmap，验证加载钩子后的原函数转调，需要可访问的 X11 显示；无显示时跳过。像素检查验证首帧前不改写图像以及颜色转换。上述检查不代替实际会议中的共享验证。

新钩子的启动日志带有 `revision=capture-v2`。正常选屏后应出现 `[portal] first pipewire frame`；停止或取消后应出现 `[portal] session cleaned up`。日志路径为 `/tmp/wemeet-hook.log`，每条新日志带时间和进程号。
