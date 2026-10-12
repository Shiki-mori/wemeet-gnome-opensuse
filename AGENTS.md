# 给后续修改的约束

本仓库是 openSUSE Tumbleweed、GNOME Wayland 上的个人启动器和屏幕共享钩子。不要把腾讯会议安装包、官方客户端或任何腾讯专有二进制加进来。

## 必须保持的链路

从应用菜单启动 `wemeetapp`。进程始终走 X11 开会，屏幕共享只走这条路：

1. `WEMEET_XWAYLAND=1`，`XDG_SESSION_TYPE=x11`，`QT_QPA_PLATFORM=xcb`。
2. 钩子在构造函数里记下 `WAYLAND_DISPLAY`，之后整个进程都看不到 Wayland 会话。
3. 客户端用 X11 截图（`XGetImage` / `XShmGetImage` / `XShmAttach`）。
4. 钩子弹出 GNOME 选屏，选完后把 PipeWire 画面填进截图。
5. 停止共享时拆掉这次门户会话，下次再共享要重新选屏。

`libhook.so` 必须和 `wemeetapp` 放在同一目录。启动器找不到它就退出。

## 不要再做的事

- 不要对屏幕共享模块隐藏 `WEMEET_XWAYLAND`，也不要让它看到 `WAYLAND_DISPLAY`。那样会立刻显示「正在共享」、不弹选屏、画面全黑。
- 不要把 `XDamageQueryExtension` 或 `XDamageCreate` 固定返回失败。这个客户端会因此放弃截图初始化。
- 不要把 PLT 里尚未绑定的跳板地址当成原函数再调用。`XShmQueryVersion` 会因此死循环，并把日志写满。
- 原函数用 `dlsym(RTLD_NEXT)` 取到的 libX11 / libXext 符号。
- 在 PipeWire 还没有送出第一帧之前，不要把截图像素涂黑。
- 不要在钩子里拦截所有 `dlopen`。`QtWebEngineProcess` 会因此收到 SIGTRAP。构造函数里跳过该进程。
- 不要改回 `XDG_SESSION_TYPE=wayland` 或清掉 `WEMEET_XWAYLAND` 来开会。这台机器上快速会议会在 `libwemeet_framework.so` 里 SIGSEGV。
- 不要启用 `xwaylandvideobridge`。它会把 GNOME 桌面涂黑。

## 改完钩子之后

```bash
cd wemeet-wayland-screenshare
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
install -m 0755 build/libhook.so /path/next/to/wemeetapp/libhook.so
```

正在运行的腾讯会议不会加载新的 `libhook.so`。先彻底退出（包括托盘），再从应用菜单打开。

调试日志在 `/tmp/wemeet-hook.log`。正常共享应出现 `[chain] x11-capture + gnome-portal`，点共享后出现 `[portal] session created` 和 `[portal] first pipewire frame`。只有 `[ctor]` 或 `[chain]`、没有 `[portal]`，说明这次点击没有走到 X11 截图。

## 许可

修改钩子时保留 `wemeet-wayland-screenshare/LICENSE`（上游 MIT，David Xu，2024）、`plthook` 文件头里的 BSD 声明，以及 `stb/stb_image_resize2.h` 的公有领域声明。不要把完整的 stb 仓库再加进来，只要这一份头文件。
