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

## 当前启动设置与钩子设计（capture-v4，本机已验证可正常运行）

2026-10-12 用户实测确认：GNOME 选屏正常、选屏后画面持续更新、停止后再次共享能够重新选屏并正常更新。将此版本标记为本机可正常运行的基线，Git 标签为 `capture-v4-working`。对应运行库 SHA-256 为 `7767319c37e9915d5b4b95188990cd533db61f4ccc6e7c6572b332e659a59a99`。

从应用菜单启动 `wemeetapp`。启动器目前把客户端环境设置为 X11，钩子的目标链路如下；客户端是否实际进入这条链路，需要按每次运行的日志确认。历史记录中也有客户端自身 `ext_portal` 路径成功送帧的情况。

1. `WEMEET_XWAYLAND=1`，`XDG_SESSION_TYPE=x11`，`QT_QPA_PLATFORM=xcb`。
   这个版本的屏幕共享模块用 `WEMEET_XWAYLAND == "1"` 选择客户端自身门户。capture-v3 起单独替换 `libscreen_share_module.so` 的 `getenv` PLT，使该模块读取此变量为 `"0"`；开会和平台部分仍读取 `"1"`。实际日志已观察到多轮钩子门户启动、首帧和停止清理，但 capture-v3 对端画面冻结。
2. 钩子在构造函数里保存并清除 `WAYLAND_DISPLAY`，同时拦截 `getenv` 保持上述环境设置。当前门户代码不再恢复这个全进程变量；其他会话检测方式仍需独立分析。
3. 客户端调用被拦截的 X11 截图函数时，钩子尝试启动门户。目前图像尺寸判断是宽度至少 640、高度至少 360；XShm 段跟踪也参与触发。
4. 钩子在自己的 `GMainContext` 中创建门户会话、弹出 GNOME 选屏，等真实 PipeWire 首帧到达后填入 XImage 或共享内存图像。当前屏幕图像分支会直接填充画面，其他原函数正常转调。
5. capture-v4 用独立 X11 连接对每个已复制的新帧调用 `XDamageAdd`，报告整个 X11 根窗口区域发生变化，使客户端再次取图并更新编码区域。当前只通知根窗口，不向任意外部窗口或 pixmap 发送通知，也不绘制桌面。
6. 相关 XShm 段分离、XDamage 对象销毁或采集 Display 关闭会请求清理；门户外部关闭和错误也会结束会话。capture-v3 实际日志已有多轮 XDamage 销毁触发清理、再次共享重新选屏的记录；不能据此确认画面刷新正常。

`libhook.so` 必须和 `wemeetapp` 放在同一目录。启动器找不到它就退出。

## 遇到过的问题

以下是此前在这台机器上记录的问题与原因判断。现象相同时可先参考这些记录，再结合当前证据重新分析；相似现象不保证原因相同，也不排除尝试其他采集路径。

- **官方脚本的 X11 设置。** 此前检查到它设置 `WEMEET_XWAYLAND=1`、`XDG_SESSION_TYPE=x11`，清除 `WAYLAND_DISPLAY`。当次麦克风、摄像头正常，原生 X11 截图看不到 Wayland 窗口，也未出现 GNOME 选屏。这是当次观察，不代表所有启动方式都会采取同一采集路径。
- **Wayland 设置下的快速会议崩溃。** 2026-08-31 用 `XDG_SESSION_TYPE=wayland` 且不设置 `WEMEET_XWAYLAND` 时，曾成功开会和共享。BIOS 升级并重启后，多次 SIGSEGV 记录指向 `libwemeet_framework.so +0x13d4300`，当时 X11 设置能开会。时间先后关系不能证明 BIOS 是原因，也不能证明 Wayland 路径永远不可用。
- **视频桥实验曾出现桌面变黑。** 当时选屏不稳定，未最大化窗口周围的桌面变黑。当前 `~/.config/autostart/org.kde.xwaylandvideobridge.desktop` 通过 `Hidden=true` 禁用自启。是否能通过配置或实现调整解决，未完成验证。
- **普通预加载未覆盖当次 X11 调用。** `libxcast.so` 的 PLT 绑定曾需要用 `plthook` 替换。只有启动日志不足以确定是 PLT 未补上；还可能是客户端没启动采集、调用了其他分支，或钩子在更早的查询中返回失败。需要同时检查替换结果、原函数地址和客户端日志。
- **扩大 `dlopen` 拦截实验时出现网页进程崩溃。** 曾记录 `QtWebEngineProcess` 收到 SIGTRAP。当前实现只对指定屏幕共享库做 PLT 替换，并跳过网页进程；历史崩溃的具体触发条件需要结合当次代码和栈分析。
- **扩展查询失败的影响曾被过度概括。** 旧记录认为 `XDamageQueryExtension` 返回 0 必然让初始化退出。2026-10-12 18:40 日志却同时记录扩展不可用警告、`init.leave.succ` 和后续 `device.busy`。查询失败、XDamage 创建失败、图像读取失败应分别分析，不能据此断言客户端不会尝试其他截图方式。
- **把 PLT 跳板当成原函数会让 `XShmQueryVersion` 死循环。** 补丁读到的旧地址是 `plt+6`，尾调用又回到钩子自己。CPU 打满，`/tmp/wemeet-hook.log` 曾写到约 1.9 GB。原函数必须用 `dlsym(RTLD_NEXT)`。
- **隐藏 `WEMEET_XWAYLAND` 的实验曾出现空白共享。** 当时客户端显示「正在共享」，没有选屏和画面，日志包含 `external:true`，重试也未弹选屏。但外部采集本身并非失败证据：10 月 8 日的 `ext_portal` 路径有首帧和编码记录。需要区分当次环境改动、采集启动和实际帧流向。
- **门户主上下文未被正确驱动。** 历史实验中，采集停在 `init.enter`、门户回调未执行；将会话创建和启动放入持续驱动的专用 `GMainContext` 后改变了现象。当前代码沿用该设计。选屏不出现仍需检查请求、响应、取消和超时日志。
- **选屏成功但共享空白有多个候选原因。** 包括钩子画面未进入客户端正在编码的路径、首帧前错误清空像素、PipeWire 连接或格式失败，以及节点列表与 fd 发布顺序错误。此前有 `pipewire_node_ids.at(0)` 异常的记录；当前代码先验证节点和 fd，再发布状态。不能仅凭「已选屏」确认已采集或已送出画面。
- **同一安装中出现多条采集路径。** 10 月 8 日日志有 `ext_portal` 首帧和编码记录，无 X11 截图初始化。10 月 12 日较早实验有 X11 初始化，也有 `ext_portal` 启动；19:24 的运行仅配置 `Display_0`，没有采集启动记录。最后一种现象表明日志停在启动之前，原因尚未确认；不能排除钩子更早的影响，也不能断言补丁绝未被覆盖。同一日志文件可能包含多轮运行，应按时间和进程区分。
- **菜单入口与 `TryExec`。** 此前添加 `TryExec` 后曾遇到图标消失，启动器的可执行状态与 GNOME 缓存被怀疑相关，但缓存机制未独立验证。当前菜单文件不设置 `TryExec`。

## 此前总结的避坑建议（可以重新评估）

- 以当前 X11 启动设置作为对照基线。改变 `WEMEET_XWAYLAND`、`WAYLAND_DISPLAY` 或会话类型时，记录实际环境、采集路径和首帧结果，避免把几个同时改变的条件归结为一个原因。
- 扩展查询和创建函数默认如实转调系统实现。若实验改变返回值，分别检查初始化、读图和送帧阶段。
- 原函数优先用 `dlsym(RTLD_NEXT)` 解析，必要时从明确的系统库句柄解析；确认类型和实际指向，不调用未绑定的 PLT 跳板。
- 首帧前保持原有像素；当前屏幕图像分支等待首帧或返回失败。记录 PipeWire 的状态和格式，不能把占位黑图当成有效首帧。
- 扩大动态库拦截范围时关注网页子进程。当前实现跳过 `QtWebEngineProcess`，只补指定库的 PLT。
- 重试 Wayland 设置或视频桥有探索价值，但要保留能回滚的启动方式，并对照曾出现的崩溃和桌面变黑现象。

## 改完钩子之后

```bash
cd /home/phrolova/Applications/TencentMeeting/wemeet-wayland-screenshare
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
install -m 0755 build/libhook.so /home/phrolova/Applications/TencentMeeting/libhook.so
```

正在运行的腾讯会议不会加载新的 `libhook.so`。先彻底退出（包括托盘），再从应用菜单打开。

调试日志在 `/tmp/wemeet-hook.log`，当前日志带时间和进程号，新启动记录包含 `revision=capture-v4`。只有启动或补丁记录，表示尚未观察到钩子的门户启动；需与同次客户端日志交叉检查，不能单凭日志缺失判断根因。

- `[portal] session created`：门户会话已创建，尚不代表已选屏或已采集。
- `[route] screen-share getenv PLT replace result=0`：模块局部路由替换成功；`[route] screen-share WEMEET_XWAYLAND=0` 表明该模块实际查询了路由变量。
- `[portal] first pipewire frame`：钩子已复制一个通过格式与缓冲区检查的帧，尚不代表客户端已编码或对端收到。
- `[portal] stream ... source_type=1` 表示门户报告显示器来源，`2` 表示窗口来源，`0` 表示没有提供该属性。
- `[damage] PipeWire frame notifications enabled`：独立 X11 连接已启用根窗口变化通知。
- `[frames] accepted=... image_reads=... last_read=... damage_notifications=...`：分别是已复制帧数、客户端成功读图次数、最后读到的帧序号和已发送通知数，每五秒记录；结合变化量区分收帧、通知、读图是否停滞。不能把 PipeWire 处理回调次数直接等同于已复制帧数。
- `[portal] session cleaned up`：工作线程已结束采集并执行清理；仍应核实下一轮能否重新选屏。
- `cancelled callback still pending`：取消后等待回调达到清理时限，回调所有权暂被保留以避免访问已销毁对象；不能把这条记录当成所有异步资源均已释放的证据。

客户端自身 `ext_portal` 成功时可能没有钩子的门户日志。实际成功应结合客户端首帧、编码记录和对端画面判断。

## 2026-10-12 capture-v2 修复及测试反馈

- 原函数包装保存的是函数指针，历史代码却用 `std::function::target<函数类型>()` 取目标，11 个拦截函数都取到空值。安装库的反汇编确认扩展查询直接返回失败；独立系统探针确认本机 XDamage 和 XShm 实际可用。现改为直接调用 `dlsym` 得到的类型化指针，不使用 PLT 旧地址。
- 18:40 的旧日志同时出现扩展不可用警告和 `init.leave.succ`，随后不断 `device.busy`。因此，历史「扩展查询失败必然让初始化退出」结论并不普遍成立，需按具体版本和调用阶段判断。
- 会话改由一个工作线程管理；取消、外部关闭、初始化失败和首帧超时均进入清理。停止信号包括相关 XShm 段分离、XDamage 对象销毁和采集 Display 关闭，以覆盖普通 XGetImage 路径。截图读取持有会话所有权并与帧写入互斥。
- 本机 Xwayland 根窗口的直接 XGetImage 探针收到 BadMatch。屏幕截图路径改为等待真实 PipeWire 首帧，再填入普通 XImage/共享内存图像；小图像和其他原函数仍正常转调。不再在门户线程里向全进程环境恢复 WAYLAND_DISPLAY。
- 日志带时间、进程号和 `revision=capture-v2`。可选回归检查见 README；它们不能证明实际会议已恢复，也不能解决客户端根本没有发起采集的情况。
- capture-v2 已在实际运行目录编译安装，安装库与构建产物 SHA-256 一致。三个回归检查覆盖模拟门户取消/重试、私有 8×8 pixmap 的原函数转调，以及首帧前像素保护和颜色转换；未覆盖真实门户成功送帧、共享停止信号及客户端编码的完整链路。
- 用户随后反馈：首次共享出现 GNOME 选屏、对端能看见画面；停止后再次共享不选屏且失败。21:09:36 客户端日志记录 `ext_portal` 启动及编码首帧，21:10:26 停止该设备，21:11:03 第二次仅配置 `Display_0`，没有再次启动采集。同次钩子日志只有加载和 PLT 记录，没有门户或 X11 查询记录。首次成功走客户端自身门户，不能据此声称 capture-v2 的截图/清理链路已通过会议验证。
- 修改前的源码和库保存在 `/home/phrolova/Applications/TencentMeeting/backups/20261012-203242-before-capture-v2`。不要把旧备份与当前运行版本的日志混为一轮实验。

## 2026-10-12 capture-v3 路由调整

- 对当前已安装屏幕共享模块的只读检查显示，名为 `IsUseXDGDesktopPortal` 的日志对应函数读取 `WEMEET_XWAYLAND` 并比较 `"1"`；相关调用分支在真值时进入原生 Wayland 选屏助手。此前「全进程保持此变量为 1 就能保证共享走 X11 钩子」的推断不成立。
- 新代码仅在屏幕共享模块 PLT 上替换该变量的读取，显式返回 `"0"`，使其选择 X11 截图；其他模块仍为 `"1"`，会话类型仍为 x11，WAYLAND_DISPLAY 不恢复。新方案与此前在其他范围隐藏变量的实验不能直接等同。
- 新增自建共享库回归检查：即使使用 `RTLD_DEEPBIND`，模块局部 PLT 路由也应在多次查询中保持生效，而模块外的开会环境保持原值。该检查不载入或收录腾讯二进制，也不证明实际会议中的停止/重启行为。
- capture-v3 已同步到实际运行目录、在那里编译并安装，四个回归检查通过；随后会议反馈及日志见下一节。capture-v2 源码和库的备份位于 `/home/phrolova/Applications/TencentMeeting/backups/20261012-212754-before-capture-v3`。

## 2026-10-12 capture-v3 冻结反馈与 capture-v4 刷新修复

- 用户明确选择整块显示器，反馈对端画面停在 GNOME 选屏窗口淡出的瞬间，光标仅在 Codex 区域更新。不能将这个现象解释成选择了单个窗口。
- PID 130753 的 21:32—21:42 日志有多轮钩子门户首帧；21:33、21:36、21:40 记录 XDamage 销毁触发会话清理，随后能再次创建门户。重复共享清理已观察到，画面冻结仍未解决。
- 同次日志中 PipeWire 处理计数持续增加，但此前计数在缓冲检查前增加，因此只证明处理回调持续执行；客户端屏幕更新率约 0.49 fps，独立 X11 光标线程有时约 20 fps。X11 光标位置不覆盖所有 Wayland 窗口；门户已请求内嵌光标，恢复完整新帧刷新后仍需验证鼠标显示效果。
- 对当前 libxcast.so 的只读检查显示，首帧后根据 XDamage 事件及其脏区域决定是否取图。钩子此前只替换截图像素，没有向该机制报告 PipeWire 的变化。21:47 的三秒探针用独立 X11 连接对根窗口发送全屏 `XDamageAdd`，自身收到 60 次完整区域通知；客户端同一十秒统计窗口由 0.49 升到 1.59 fps，随后回到 0.49。这支持缺少变化通知的原因判断，但不是对端恢复的证明。
- capture-v4 在成功复制缓冲后增加帧序号，由门户工作线程为新序号发送全屏 XDamage 通知；重复序号和首帧前不发送。连接和区域随会话清理。保留扩展查询和创建的真实返回值，不通过禁用 XDamage 改变路径。
- 增加接受帧/图像读取/通知计数及门户 source_type 日志。新增独立 Xvfb 检查完整脏区域、重复序号过滤和桌面像素不变；像素检查覆盖后续帧替换。用户随后确认实际会议可选屏、画面持续更新，停止后再次共享也能重新选屏并正常更新；跨窗口光标没有单独反馈，不扩大验证结论。
- capture-v4 已在实际运行目录编译、通过五项回归检查并原子安装。安装库与构建产物 SHA-256 均为 `7767319c37e9915d5b4b95188990cd533db61f4ccc6e7c6572b332e659a59a99`；capture-v3 源码与库备份位于 `/home/phrolova/Applications/TencentMeeting/backups/20261012-215400-before-capture-v4`。运行中的旧进程需要完全退出后重新启动。
- 2026-10-12 按用户验证结果将 capture-v4 标记为可正常运行版本，Git 标签 `capture-v4-working`。后续排查可用该版本作为本机对照基线；历史失败观察和未覆盖场景仍保留，不能据此保证其他机器或未来客户端版本的行为。

## 许可

修改钩子时保留 `wemeet-wayland-screenshare/LICENSE`（上游 MIT，David Xu，2024）、`plthook` 文件头里的 BSD 声明，以及 `stb/stb_image_resize2.h` 的公有领域声明。不要把完整的 stb 仓库再加进来，只要这一份头文件。
