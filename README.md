# 腾讯会议在 GNOME Wayland 上的个人启动器

这是一套在 openSUSE Tumbleweed、GNOME Wayland 上使用官方 Linux 客户端的个人记录：启动脚本、菜单入口，以及对开源屏幕共享钩子的修改。

本仓库**不包含**腾讯会议安装包、官方客户端，或任何腾讯的专有二进制文件。客户端需要从腾讯会议官网或发行版软件源自行安装。

**当前本机可用版本：`native-portal-v2-working`。** 2026-10-12 用户完成原生共享及嵌入鼠标修复的实际测试，反馈「测试没有问题」。十项回归检查也已通过，该标签作为后续修改的优先对照基线。

此前桥接版本保留为 `capture-v4-working`：选屏、持续更新及重复共享曾通过测试，但随后反馈卡顿严重和鼠标残影，因此保留作历史对照。

当前使用 `native-portal-v2`：应用菜单启动后默认让客户端自带的 GNOME 门户/PipeWire 助手共享，保留 X11 开会环境，关闭桥接像素注入和 XDamage 通知；原生门户 SelectSources 的光标模式改为 2，让 GNOME 将鼠标嵌入画面，用户测试已通过。彻底退出后，可用以下命令启动桥接模式作对照：

```bash
WEMEET_CAPTURE_MODE=x11-bridge /home/phrolova/Applications/TencentMeeting/wemeetapp
```

原生模式的路由日志在 `/tmp/wemeet-hook.log`（`revision=native-portal-v2`）；共享时应出现 `[cursor] native SelectSources cursor_mode=1 -> 2 (embedded)`。客户端每轮门户请求、响应和关闭事件记录在 `/tmp/wemeet-native-portal-进程号.log`，发出的 SelectSources 应为 cursor_mode=2。启动器会启动一个独立的只读诊断进程，仅记录对应客户端 PID；需要系统 Python 的 PyGObject，客户端退出后诊断结束。

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

依赖：`cmake`、`gcc-c++`、`libportal-devel`、`pipewire-devel`、`dbus-1-devel`、`libX11-devel`、`libXext-devel`、`libXrandr-devel`、`libXdamage-devel`、`libXfixes-devel`。不需要 OpenCV。

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

门户检查在独立 D-Bus 上模拟创建失败、选屏取消和请求未完成时停止；不会打开真实选屏或会议。X11 检查读取自建的 8×8 pixmap，验证加载钩子后的原函数转调，需要可访问的 X11 显示；无显示时跳过。像素检查验证首帧前不改写图像、颜色转换以及后续帧替换。模块路由检查使用自建共享库，验证屏幕共享模块读取的路由变量与开会环境分别保持预期值。如果安装了 `Xvfb`，还会在独立 X 服务器中检查每个新帧的全屏 XDamage 通知、重复序号过滤，以及通知前后桌面像素不变。上述检查不代替实际会议中的共享验证。

桥接模式的路由查询应出现 `[route] screen-share WEMEET_XWAYLAND=0`，随后钩子创建门户并记录 `[portal] first pipewire frame`、`[damage] PipeWire frame notifications enabled`；每五秒的 `[frames]` 记录已复制帧数、客户端读图次数、最后读到的帧序号和通知数。停止或取消后应出现 `[portal] session cleaned up`。原生模式查询值为 1，主要检查对应的门户诊断及客户端 ext_portal 启动/首帧记录。日志每条带时间和进程号。

此前 capture-v2 的首次实际共享走的是客户端自身 `ext_portal`，停止后重试未能启动采集。capture-v3 在屏幕共享模块的 PLT 上调整其门户选择变量，使共享进入 X11 截图钩子；实际日志已观察到多轮门户启动与清理，但用户反馈对端画面冻结。capture-v4 为已复制的 PipeWire 新帧补充 XDamage 全屏变化通知，让客户端重新取图；开会部分保留原启动设置。用户已验证连续画面与停止后再次共享正常，五项回归检查也全部通过。
