# 项目背景与阶段性探索记录

## 如何使用这份记录

本文件是此前 AI 解决问题过程中形成的阶段性记录，不是一份必须严格遵守的技术约束。历史结论可能不完整，甚至可能存在错误；下文技术方案中的「必须」「不要」「只走这条路」反映当时的实验与判断，不代表其他方向绝对不可行。核心目标是解决问题。

- **独立思考**：结合当前源码、运行环境和实际现象重新分析，不直接接受此前 AI 的所有结论。
- **允许重新尝试**：对曾被禁止或判定不可行的技术路线，有合理尝试价值时可以重新评估并实验。
- **鼓励多方向探索**：可以提出新假设、采用不同方法，也可以用证据推翻旧结论。
- **重视实验与证据**：理解此前失败的条件与原因，判断调整条件或实现方式后是否可行；区分观察结果与推断，并记录新的实验条件和结果。
- **保留安全边界**：上述灵活性针对技术方案和探索策略，不授权随意执行破坏性操作、删除重要数据或引入不可逆修改。保留必要的许可声明，不将腾讯专有二进制加入仓库。

本仓库是 openSUSE Tumbleweed、GNOME Wayland 上的个人启动器和屏幕共享钩子。不要把腾讯会议安装包、官方客户端或任何腾讯专有二进制加进来。

## 本机实际运行目录

- 实际在用的是 `/home/phrolova/Applications/TencentMeeting`（`~/Applications/TencentMeeting`），不是 Git 仓库。
- 应用菜单执行 `/home/phrolova/Applications/TencentMeeting/wemeetapp`。这个脚本把同目录的 `libhook.so` 放进 `LD_PRELOAD`，再启动 `/usr/libexec/wemeet/bin/wemeetapp`。
- `~/.local/bin/wemeetapp` 也转调上述启动脚本。
- 实际使用的钩子源码在 `/home/phrolova/Applications/TencentMeeting/wemeet-wayland-screenshare`。修改后必须在这里编译，把生成的 `libhook.so` 安装到 `/home/phrolova/Applications/TencentMeeting/libhook.so`，下次启动才会生效。
- `/home/phrolova/Applications/wemeet-gnome-opensuse` 是推到 GitHub 的副本，启动器不会读取它。只更新这里的源码不会改变实际运行的腾讯会议；需要让运行目录中的源码包含相应修改，再在那里编译、安装并重启。

## 此前采用的链路（阶段性方案）

从应用菜单启动 `wemeetapp`。进程始终走 X11 开会，屏幕共享只走这条路：

1. `WEMEET_XWAYLAND=1`，`XDG_SESSION_TYPE=x11`，`QT_QPA_PLATFORM=xcb`。
2. 钩子在构造函数里记下 `WAYLAND_DISPLAY`，之后整个进程都看不到 Wayland 会话。
3. 客户端用 X11 截图（`XGetImage` / `XShmGetImage` / `XShmAttach`）。
4. 钩子弹出 GNOME 选屏，选完后把 PipeWire 画面填进截图。
5. 停止共享时拆掉这次门户会话，下次再共享要重新选屏。

`libhook.so` 必须和 `wemeetapp` 放在同一目录。启动器找不到它就退出。

## 遇到过的问题

以下是此前在这台机器上记录的问题与原因判断。现象相同时可先参考这些记录，再结合当前证据重新分析；相似现象不保证原因相同，也不排除尝试其他采集路径。

- **官方启动脚本在 Wayland 下伪装成 X11。** 它设置 `WEMEET_XWAYLAND=1`、`XDG_SESSION_TYPE=x11`，并清掉 `WAYLAND_DISPLAY`。麦克风和摄像头正常，屏幕共享走 X11 截图，看不见 Wayland 窗口，也没有 GNOME 选屏。
- **保留 Wayland 会话可以共享，但重启后快速会议闪退。** 2026-08-31 用 `XDG_SESSION_TYPE=wayland` 且不设置 `WEMEET_XWAYLAND` 时，快速会议、麦克风、摄像头和 GNOME 选屏都可用。BIOS 升级并重启后，快速会议在 `libwemeet_framework.so` 里 SIGSEGV，偏移每次都是 `+0x13d4300`。之后只有官方 X11 路径能开会。
- **`xwaylandvideobridge` 会把桌面涂黑。** 选屏不稳定，未最大化窗口周围的桌面变黑。自启必须用 `~/.config/autostart/org.kde.xwaylandvideobridge.desktop` 里的 `Hidden=true` 关掉。
- **普通 `LD_PRELOAD` 拦不住 `libxcast.so`。** 它是 `libwemeet_platform.so` 的依赖，X11 函数从它自己的 PLT 绑定。日志里只有 `[ctor]`、没有截屏调用时，就是没补上 PLT。要用 `plthook` 替换 `libxcast.so` 的 PLT。
- **拦截全部 `dlopen` 会让网页进程崩。** `QtWebEngineProcess` 收到 SIGTRAP。构造函数发现可执行文件名里有 `QtWebEngineProcess` 时必须直接返回。
- **把 `XDamageQueryExtension` 固定返回 0 会让截图初始化退出。** 这个版本不会因此改走 `XGetImage`。`XDamageCreate` 返回空也一样。
- **把 PLT 跳板当成原函数会让 `XShmQueryVersion` 死循环。** 补丁读到的旧地址是 `plt+6`，尾调用又回到钩子自己。CPU 打满，`/tmp/wemeet-hook.log` 曾写到约 1.9 GB。原函数必须用 `dlsym(RTLD_NEXT)`。
- **对屏幕共享模块隐藏 `WEMEET_XWAYLAND` 会得到空白共享。** 客户端立刻显示「正在共享」，不弹 GNOME 选屏。日志里 `Display_0` 被设成 `external:true`，没有画面送出。第二次再共享也不会再弹选屏。
- **门户请求发到错误的线程上时，选屏不会出现。** 采集停在 `screen_capture_linux.cc` 的 `init.enter`，没有 `init.leave`。会话要在钩子自己的 `GMainContext` 里创建，创建完成后再 `xdp_session_start`。
- **选完屏幕后对方仍看到空白。** 一种情况是客户端已经改走外部采集，X11 截图不再被编码。另一种是钩子在 PipeWire 第一帧到达前把截图涂成黑色。`pipewire_fd` 必须在节点号填完之后再发布，否则 `pipewire_node_ids.at(0)` 会抛异常，会话留在半初始化状态，下一次共享不再弹选屏。
- **同一次安装会走出不同的路。** 2026-10-08 面试时走的是官方 `ext_portal`，约 5 秒后开始有画面，X11 截图线程没有启动。2026-10-12 点共享后只把 `Display_0` 配到「已配置、未开始」，没有 `init.enter`，也没有切到 `ext_portal`，钩子日志里只有补丁记录。这不是补丁被冲掉，是闭源模块当次没有调用截图函数。
- **应用菜单里找不到腾讯会议。** 桌面文件加了 `TryExec`，GNOME 在启动器还不可执行时检查过一次，就把图标从列表里拿掉，之后不会自动恢复。菜单文件不要写 `TryExec`。

## 此前总结的避坑建议（可以重新评估）

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
cd /home/phrolova/Applications/TencentMeeting/wemeet-wayland-screenshare
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
install -m 0755 build/libhook.so /home/phrolova/Applications/TencentMeeting/libhook.so
```

正在运行的腾讯会议不会加载新的 `libhook.so`。先彻底退出（包括托盘），再从应用菜单打开。

调试日志在 `/tmp/wemeet-hook.log`。正常共享应出现 `[chain] x11-capture + gnome-portal`，点共享后出现 `[portal] session created` 和 `[portal] first pipewire frame`。只有 `[ctor]` 或 `[chain]`、没有 `[portal]`，说明这次点击没有走到 X11 截图。

## 许可

修改钩子时保留 `wemeet-wayland-screenshare/LICENSE`（上游 MIT，David Xu，2024）、`plthook` 文件头里的 BSD 声明，以及 `stb/stb_image_resize2.h` 的公有领域声明。不要把完整的 stb 仓库再加进来，只要这一份头文件。
