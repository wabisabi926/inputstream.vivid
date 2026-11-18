# 实机测试说明（CoreELEC 22 / Amlogic-no / aarch64）

一次性把能自动化的都做完了，剩下只有「装到盒子上跑一遍」。按本文做完，把两样东西发回来即可。

## 0. 先明确这一版能做到哪

| 层级 | 现象 | 这一版 |
|---|---|---|
| L1 插件被加载 | `/storage/inputstream.vivid-diag.txt` 出现 | ✅ 目标 |
| L2 视频能播、能拖 | 画面正常 | ✅ 能，但解码是 Kodi 原生 amcodec，不是插件 |
| L3 HDR Vivid 生效 | 电视进入 Vivid 模式 | ❓ 待测，见第 6 节 |

**注意**：Vivid 后向兼容，不支持时会退回 HDR10/HLG 正常播放。所以「能播」不等于「Vivid 生效」，必须看第 6 节的证据。

## 1. 安装

把 `inputstream.vivid-<version>-linux-aarch64.zip` 拷到盒子（U 盘 / SMB 均可）。

方式一，Kodi 界面：设置 → 插件 → 从 zip 文件安装 → 选该 zip。

方式二，SSH（zip 内已有 `inputstream.vivid/` 顶层目录，解到 addons 根目录）：

```bash
cd /storage/.kodi/addons && unzip -o /storage/inputstream.vivid-<version>-linux-aarch64.zip
ls -l /storage/.kodi/addons/inputstream.vivid/    # 应看到 addon.xml 与 inputstream.vivid.so
systemctl restart kodi
```

## 2. 启用插件

设置 → 插件 → 我的插件 → 输入流（Inputstream）→ **HDR Vivid 硬件解码器** → 启用。

> 没启用的话，Kodi 不会选中它，播放会退回内置 demuxer（也不会有诊断报告）。

## 3. 直接播放即可（2.2.0 起）

插件在 `addon.xml` 里声明了 `extension="mkv|m2ts"`，Kodi 会**自动**把这些容器交给它，不需要 `.strm`、也不需要入库：

- 本地 `.mkv` / `.m2ts`：直接播就行；
- SMB / NFS / HTTP 上的 `.mkv`：同样会经过插件（插件内部走 Kodi VFS 读取，不是 POSIX 文件句柄）。

如果播了却没有诊断报告，说明插件没被选中，可以用 `.strm` 精确指定单文件来兜底：

```bash
cat > /storage/test-vivid.strm <<'EOF'
#KODIPROP:inputstream=inputstream.vivid
/storage/videos/你的片源.mkv
EOF
```

把第二行换成真实路径，然后：视频 → 文件 → 添加视频源 → `/storage` → 选 `test-vivid.strm` 播放。

> 副作用要清楚：**启用期间，这台机器上所有 `.mkv` / `.m2ts` 都会经过插件**。不想接管时把插件禁用即可恢复原生路径；`.iso` 没有放进去，是因为 ISO 要走 Kodi 的蓝光导航，被接管会让菜单/章节失效。

## 4. 确认 L1：插件是否被加载

```bash
ls -l /storage/inputstream.vivid-diag.txt
grep -i "inputstream.vivid" /storage/.kodi/temp/kodi.log | tail -20
```

- 看到报告文件 → **L1 通过**，`.so` 架构正确、Kodi 22 接受并创建了实例。
- 没有报告、日志里出现 `undefined symbol` / `not loaded` / `failed to load` → 把那段日志发回。

> 报告是在插件**第一次被使用**时生成的。所以如果播放前没有，先做第 3 步再回来看。

## 5. 确认 L2：能不能播

```bash
grep -iE "opened .*bytes|ReadStream|Open 失败" /storage/.kodi/temp/kodi.log | tail -10
```

出现 `opened /storage/... (N bytes)` 说明字节流已经交给 Kodi。此时画面应正常、可拖动进度。

## 6. 确认 L3：Vivid 到底有没有生效

三条证据，任一条有正面结果就是「Vivid 链路存在」：

1. 诊断报告第 5 节（`amhdmitx` 的 `hdr_cap` / `dv_cap`）里是否出现 Vivid 相关字段；
2. 播放时电视/投影的 HDR 标识：显示 `HDR10` 说明退回静态 HDR，显示 `HDR Vivid` / `菁彩` 才是真的；
3. 播放时执行：

```bash
dmesg | tail -50 | grep -iE "vivid|hdr|2084"
cat /sys/class/amhdmitx/amhdmitx0/hdr_cap
```

## 7. 回传什么（两样就够）

```bash
# 1) 诊断报告
cat /storage/inputstream.vivid-diag.txt

# 2) 播放之后的日志片段
grep -iE "inputstream.vivid|amcodec|AMLCodec|vivid|hdr" /storage/.kodi/temp/kodi.log | tail -60
```

再加一句：设备型号 + 电视型号 + 用的哪个片源。

## 8. 卸载 / 回滚

```bash
rm -rf /storage/.kodi/addons/inputstream.vivid
rm -f /storage/inputstream.vivid-diag.txt
systemctl restart kodi
```

只删插件目录，不动 Kodi 自带组件。

## 9. 报告到手后我会怎么定

| 报告特征 | 结论 | 下一步 |
|---|---|---|
| `aml_vid_*` 全 no、`codec_*` 全 yes、`/proc/self/maps` 里有 amcodec | 插件自解码这条路不通，Kodi 原生已在用 amcodec | 改 Kodi 的 `AMLCodec.cpp` 加 Vivid 通路（C2） |
| 完全没有 `libamcodec.so` | NO 分支换了新解码栈 | 按新接口重写解码层 |
| 居然有 `aml_vid_*` | 与公开资料不符 | 重新评估，可能是我调研的 API 版本不对 |
| `hdr_cap` 已含 Vivid 且电视显示 Vivid | 原生已支持 | 本插件无必要，改用原生播放 |
