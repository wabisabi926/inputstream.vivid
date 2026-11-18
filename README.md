# inputstream.vivid

CoreELEC 专用 HDR Vivid 输入流插件。

## 适用范围（重要）

**只适用于 CoreELEC（Amlogic-no 系，Linux + aarch64）。** 不能装到 Android、Windows、macOS 版 Kodi 上，也不适用于 x86 LibreELEC：

- 产物是 **Linux aarch64 的 .so**，`addon.xml` 里只声明 `library_linux` + `platform=linux-aarch64`，别的平台 Kodi 直接判定不兼容；
- 解码路径依赖 Amlogic 的 `libamcodec`，其它平台没有这个库；
- CoreELEC 之外就算架构碰巧相同（比如 aarch64 的 LibreELEC / 树莓派），也没有 amcodec，插件只能加载、无法硬解。

需要说明的是：**打包格式仍然遵循 Kodi 插件规范**（`addon.xml`、`kodi.inputstream` 扩展点、装到 `/storage/.kodi/addons`）。这不是"写成 Kodi 通用"，而是因为 CoreELEC 的插件系统本身就是 Kodi 的——没有单独的 "CoreELEC 插件格式"。所以"给 CoreELEC 用"和"按 Kodi 插件规范打包"是同一件事，而"能不能装"由架构和平台声明决定。

## Kodi 如何选中本插件（自动接管 .mkv / .m2ts）

Kodi 选择 inputstream 插件只有三条路，全部在 `CDVDFactoryInputStream::CreateInputStream()` → `CInputStreamAddon::Supports()` 里判定：

1. 条目属性 `inputstream` 命中插件 ID —— 来自 `.strm` 里的 `#KODIPROP:inputstream=inputstream.vivid`，或视频插件/PVR 主动设置；
2. 插件声明了匹配的 `protocols`（如 `smb|nfs`）；
3. **插件声明了匹配的 `extension`**。

本插件用的是第 3 条：

```xml
<extension point="kodi.inputstream" extension="mkv|m2ts" library_linux="inputstream.vivid.so"/>
```

所以**启用插件后，本地和网络上的 `.mkv` / `.m2ts` 都会自动经过本插件，不需要 `.strm`、也不需要入库**。这也是为什么 `Open`/`ReadStream` 走 `kodi::vfs::CFile` 而不是 POSIX `open()`——接管是全局的，只支持本地路径会让 SMB/NFS 播放直接失败。

两个刻意保留的限制：

- **不含 `iso`**：ISO 要走 Kodi 的蓝光导航（`CDVDInputStreamNavigator`），被插件接管会丢菜单和章节。
- **不含 `ts`**：避免把 PVR 录像接管进去，那套有额外的流细节处理。
- 想恢复原生路径：禁用插件即可；也可以删掉 `extension` 属性重新打包，退化成「只认 `#KODIPROP`」。

## 当前目标平台

| 项 | 值 |
|---|---|
| 系统 | CoreELEC 22.0-Piers（Amlogic-no / New Order） |
| 架构 | aarch64（armv8，64 位） |
| Kodi | 22 (Piers) |
| 内核 | 5.15 |

## 2.0.0 状态：架构适配验证版

本版本是第一版面向 aarch64 / Kodi 22 的构建，目标是先打通「能被 Kodi 22 加载并正常读写」这条链路，**不代表已实现 HDR Vivid 硬解**。

已实现：

- 使用 Kodi 22 真实插件接口 `kodi::addon::CInstanceInputStream`
  （旧代码里的 `kodi::inputstream::CInputStream` 在任何 Kodi 版本中都不存在）
- aarch64 交叉编译，CI 校验产物 ELF Class/Machine
- MKV / M2TS 的字节流读取、Seek、Position、Length（走 Kodi VFS，本地与 SMB/NFS 通用）
- 关键路径判空与 `kodi::Log` 日志，避免旧版 `vid_open` 空指针崩溃

尚未实现：

- **HDR Vivid 硬件解码**。当前为「字节流透传」：插件只提供原始字节，解封装与解码交给 Kodi 内置 demuxer + amcodec 播放器。
- 旧版基于 `libamcodec.so` 的 `aml_vid_*` / `aml_aud_*` 接口未接入。原因有两点：
  1. 这些符号在真实 `libamcodec.so` 中并不存在，真实导出的是 `codec_init` / `codec_write` / `codec_close` 等 `codec_*` 系列；
  2. amcodec 是零拷贝直显架构，解码结果直接进显示层，不向用户态返回解码帧，无法用「解码后取帧再交给 Kodi」的方式实现。

## 自诊断（2.1.0 起，不用 SSH 也能定位问题）

插件一被 Kodi 加载，就会自动做一次环境诊断，把结果写到：

```
/storage/inputstream.vivid-diag.txt
```

同时 `kodi.log` 里会留一行 `自诊断完成，报告: <路径>`。

报告内容包括：构建架构与工具链、`/etc/os-release` 与设备型号、逐个 `dlopen` 候选解码库并打印 `dlsym` 结果（`aml_vid_*` 与真实 `codec_*` 两套对照）、`/proc/self/maps` 里 Kodi 自己已加载的多媒体库、`amhdmitx` 的 HDR/Vivid 能力节点、`/dev` 下的解码节点，最后给出一句结论。

**关键点：能写出这份报告，本身就已经证明 `.so` 架构正确、Kodi 22 愿意加载它。** 如果报告文件不存在，说明插件压根没被加载（看 `kodi.log` 里的 `undefined symbol` / `not loaded`）。

## 构建

由 GitHub Actions 自动完成（`.github/workflows/build.yml`）：

- 交叉工具链：`aarch64-linux-gnu-gcc/g++`
- Kodi 源码：`xbmc/xbmc` 分支 `Piers`，取 `xbmc/addons/kodi-dev-kit/include` 作为 SDK 头文件
- 产物：`inputstream.vivid-<version>-linux-aarch64.zip`，Kodi 可直接安装

本地构建：

```bash
cmake -B build -S . -DKODI_SOURCE_DIR=/path/to/xbmc \
  -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
  -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++
cmake --build build -j
```

## 打包

```bash
sh tools/package-addon.sh build dist
```

产出 `dist/inputstream.vivid-<version>-linux-aarch64.zip` 与同名 `.sha256`。脚本会：

- 从 `addon.xml` 读取 `id` / `version` / `library_linux`，不在脚本里硬编码，避免两处不一致
- 校验 `.so` 是 ELF、非空，并把文件名改成 `addon.xml` 声明的那个名字
- 按 Kodi 要求打成**单个顶层目录** `inputstream.vivid/`，内含 `addon.xml` + `.so`（有 `LICENSE` / `resources/` / `icon.png` 会自动带上）
- 打包后回读 zip 校验结构与版本号，固定时间戳保证同样内容产出同样字节

发布：推 `v*` tag 会自动跑构建并把 zip 挂到 GitHub Release。

```bash
git tag v2.1.0 && git push origin v2.1.0
```

## 安装（CoreELEC）

```bash
# 方式一：Kodi 界面 → 设置 → 插件 → 从 zip 文件安装
# 方式二：SSH（zip 内已有 inputstream.vivid/ 顶层目录，解到 addons 根目录）
cd /storage/.kodi/addons && unzip -o /storage/inputstream.vivid-2.1.0-linux-aarch64.zip
systemctl restart kodi
ls -l /storage/inputstream.vivid-diag.txt   # 有这份报告 = 插件已被加载
```

## 实机验证

完整步骤见 [TESTING.md](TESTING.md)（含「如何强制让播放走本插件」这一步，直接播 MKV 是测不到的）。速查：

```bash
# 1) 插件是否被加载、是否创建了实例
grep -i "inputstream.vivid" ~/.kodi/temp/kodi.log

# 2) 产物架构是否为 64 位 aarch64
file /storage/.kodi/addons/inputstream.vivid/inputstream.vivid.so

# 3) 目标机上 libamcodec 是否存在、导出哪些符号
ls -l /usr/lib/libamcodec.so*
nm -D /usr/lib/libamcodec.so 2>/dev/null | grep -E "codec_init|codec_write|aml_vid" | head
```

播放一个 MKV / ISO，观察日志中出现 `opened ... (N bytes)` 即说明插件链路已通。

## HDR Vivid 在各平台的支持情况

HDR Vivid（菁彩影像，UWA/CUVA 标准）**不是 Kodi 的功能开关**，它要求「芯片解码 → 内核/驱动 → HDMI/EDID」整条链路都支持。Kodi 22 宣传的"HDR 端到端"指的是 HDR10 / HLG 静态元数据，不包含 Vivid 的动态元数据。

| 平台 | Vivid 可能吗 | 说明 |
|---|---|---|
| CoreELEC（Amlogic） | 有条件 | 晶晨芯片本身在支持列表内，取决于内核/amcodec 是否把 Vivid 元数据送到 HDMI；这正是本项目要验证的 |
| Android Kodi | 有条件，但通常退化 | 走 MediaCodec，芯片（海思/联发科/晶晨/高通）支持时理论可行；但 Kodi 自己不解析 Vivid 元数据，多数情况下按 HDR10 输出。国产电视/盒子"原生播放器支持 Vivid" ≠ Kodi 支持 |
| Windows Kodi | 不行 | D3D11 只处理 HDR10；Windows 系统层面没有 Vivid 通路 |
| x86 LibreELEC / 树莓派 | 不行 | 无对应硬件与驱动 |

另外，**Vivid 是后向兼容的**：设备不支持时会丢弃动态元数据，按 HDR10 / HLG 正常播放，不会黑屏或花屏。所以"能播、画面正常"并不能证明 Vivid 生效——要确认必须看设备是否进入 Vivid 模式（HDMI 信息、显示器的 HDR 标识、内核日志）。

## 后续工作

要让 HDR Vivid 真正硬解，需在以下方向择一推进：

1. 先确认 CoreELEC-no 22 的 Kodi/amcodec 是否已原生支持 Vivid（若已支持，本插件无必要）；
2. 若原生不支持，在 Kodi 的 amcodec 播放器驱动层（`AMLCodec.cpp`）增加 Vivid 通路；
3. 若仍保留 inputstream 形态，插件只做 demux，解码交给 Kodi 播放器。

## 与旧版的关系

旧版仅面向 CoreELEC-ng 21.3 / armv7 / Kodi 21，与新版本（no 22 / aarch64 / Kodi 22）ABI、API、解码栈均不兼容，需分别构建与分发。
