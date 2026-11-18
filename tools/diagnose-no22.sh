#!/bin/sh
# ============================================================================
# diagnose-no22.sh — CoreELEC 22 (Amlogic-no / aarch64) 硬解与插件诊断
#
# 用法（在 CoreELEC 设备上，SSH 或串口）：
#   sh /storage/diagnose-no22.sh
#
# 输出：/storage/diagnose-<时间戳>.txt （同时打印到屏幕）
#
# 设计原则：
#   - 只用 POSIX sh + busybox 命令，不依赖 bash / file / nm / readelf
#   - 每条检查都容错，缺文件不中断
#   - 结尾自动给出结论，避免"看了一堆输出不知道该看啥"
# ============================================================================

OUT="/storage/diagnose-$(date +%Y%m%d-%H%M%S).txt"
ADDON_DIR="${1:-/storage/.kodi/addons/inputstream.vivid}"
KODI_LOG="/storage/.kodi/temp/kodi.log"

# ---------------------------------------------------------------- 工具函数 --
section() {
  echo ""
  echo "=============================================================="
  echo "## $*"
  echo "=============================================================="
}

elf_info() {
  # 只读 ELF 头前 20 字节，判断位宽与机器类型（不依赖 file/readelf）
  f="$1"
  if [ ! -f "$f" ]; then echo "  [缺失] $f"; return; fi
  sz=$(wc -c < "$f" 2>/dev/null)
  pm=$(ls -l "$f" 2>/dev/null | sed 's/.* -> //')
  cls_mach=$(head -c 20 "$f" 2>/dev/null | od -An -tu1 -v | awk '
    { for (i=1;i<=NF;i++) b[++n]=$i }
    END {
      if (n < 20) { print "unknown/unknown"; exit }
      cls = (b[5]==2) ? "ELF64" : ((b[5]==1) ? "ELF32" : "ELF?")
      m = b[19] + b[20]*256
      if (m==183) mach="aarch64"
      else if (m==40) mach="armv7(32)"
      else if (m==62) mach="x86_64"
      else if (m==3) mach="i386"
      else mach="machine" m
      print cls "/" mach
    }')
  echo "  [存在] $f"
  echo "         大小: ${sz:-?} 字节   架构: ${cls_mach:-?}"
  [ -n "$pm" ] && [ "$pm" != "$f" ] && echo "         软链指向: $pm"
}

# 在二进制里找符号名字符串（.dynstr 里会保留导出名，等价于 nm 的弱替代）
has_sym() {
  f="$1"; s="$2"
  grep -aq "$s" "$f" 2>/dev/null && echo "yes" || echo "no"
}

sym_report() {
  f="$1"
  [ -f "$f" ] || { echo "  (库不存在，跳过)"; return; }
  echo "  --- 插件代码 dlsym 的那套符号（是否存在） ---"
  for s in aml_vid_new aml_vid_open aml_vid_decode aml_vid_get aml_vid_close \
           aml_aud_new aml_aud_open aml_aud_decode aml_aud_get aml_aud_close; do
    printf "    %-16s %s\n" "$s" "$(has_sym "$f" "$s")"
  done
  echo "  --- Amlogic libamcodec 真实 API（codec_* 系列） ---"
  for s in codec_init codec_write codec_read codec_close codec_checkin_pts \
           codec_set_cntl_mode codec_reset codec_alloc codec_free; do
    printf "    %-20s %s\n" "$s" "$(has_sym "$f" "$s")"
  done
}

# ------------------------------------------------------------------- 主流程 --
{
echo "CoreELEC 硬解 / inputstream.vivid 诊断报告"
echo "生成时间: $(date)"
echo "主机名:   $(hostname 2>/dev/null)"

# --- 1. 系统 ---------------------------------------------------------------
section "1. 系统与平台"
[ -f /etc/os-release ] && cat /etc/os-release
echo ""
echo "内核: $(uname -a)"
echo "用户空间位数: $(getconf LONG_BIT 2>/dev/null || echo '?')"
if [ -f /proc/device-tree/model ]; then
  echo "设备型号: $(tr -d '\0' < /proc/device-tree/model)"
fi
if [ -f /proc/device-tree/compatible ]; then
  echo "compatible: $(tr -d '\0' < /proc/device-tree/compatible | tr '\0' ' ')"
fi
echo ""
echo "--- CPU ---"
grep -E "^(processor|model name|Hardware|CPU part|Features)" /proc/cpuinfo 2>/dev/null | head -20

echo ""
echo "--- Kodi 版本 ---"
for p in /usr/lib/kodi/kodi.bin /usr/bin/kodi; do
  [ -x "$p" ] && { echo "  $p"; "$p" --version 2>/dev/null | head -3; break; }
done
grep -m1 -iE "starting kodi|kodi.*version" "$KODI_LOG" 2>/dev/null | tail -2

# --- 2. 硬解库 -------------------------------------------------------------
section "2. Amlogic 解码库"
FOUND_LIB=""
for p in /usr/lib/libamcodec.so /usr/lib/libamcodec.so.1 /usr/lib/libamadec.so \
         /usr/lib/libamavutils.so /usr/lib/libamcodec.so.*; do
  [ -e "$p" ] || continue
  elf_info "$p"
  FOUND_LIB="$p"
  break
done
[ -z "$FOUND_LIB" ] && echo "  [!!] 未找到 libamcodec.so —— NO 分支可能已换用其他解码路径"

# 全盘兜底搜索（限制深度，避免卡住）
if [ -z "$FOUND_LIB" ]; then
  echo "  --- 在常见目录里兜底搜索 amcodec/amvdec 相关库 ---"
  find /usr/lib /usr/lib64 /storage -maxdepth 3 \( -name "*amcodec*" -o -name "*amvdec*" -o -name "*amav*" \) 2>/dev/null | head -20
fi

if [ -n "$FOUND_LIB" ]; then
  echo ""
  sym_report "$FOUND_LIB"
fi

# --- 3. Kodi 自身是否用 amcodec 硬解 ---------------------------------------
section "3. Kodi 播放器驱动的硬解路径"
KODI_BIN=""
for p in /usr/lib/kodi/kodi.bin /usr/bin/kodi.bin /usr/lib/kodi/kodi; do
  [ -f "$p" ] && { KODI_BIN="$p"; break; }
done
if [ -n "$KODI_BIN" ]; then
  echo "  Kodi 二进制: $KODI_BIN"
  elf_info "$KODI_BIN"
  echo "  是否引用 libamcodec:  $(has_sym "$KODI_BIN" libamcodec)"
  echo "  是否引用 AMLCodec:    $(has_sym "$KODI_BIN" AMLCodec)"
  echo "  是否引用 libamvdec:   $(has_sym "$KODI_BIN" libamvdec)"
  echo "  是否引用 libv4l2/dma: $(has_sym "$KODI_BIN" dma_buf)"
  if command -v ldd >/dev/null 2>&1; then
    echo "  --- ldd 中的多媒体库 ---"
    ldd "$KODI_BIN" 2>/dev/null | grep -iE "amcodec|amav|amvdec|v4l|drm" || echo "    (无匹配)"
  fi
else
  echo "  [!!] 没找到 kodi.bin"
fi

# --- 4. 内核侧 -----------------------------------------------------------------
section "4. 内核侧：模块与设备节点"
echo "--- 相关内核模块 ---"
lsmod 2>/dev/null | grep -iE "amvdec|amcodec|amvpu|v4l2|dvb|aml" | head -30 || echo "  (lsmod 不可用或无匹配)"

echo ""
echo "--- 设备节点 ---"
ls -l /dev/am* /dev/video* /dev/dvb/adapter0/* /dev/dma_heap 2>/dev/null | head -40 || echo "  (无相关节点)"

echo ""
echo "--- dmesg 中的解码器信息 ---"
dmesg 2>/dev/null | grep -iE "amvdec|amcodec|codec|v4l2" | tail -25 || echo "  (dmesg 不可读，可能需要 root)"

# --- 5. HDR / Vivid 能力 --------------------------------------------------
section "5. HDR / HDR Vivid 能力（内核与 HDMI）"
for f in /sys/class/amhdmitx/amhdmitx0/hdr_cap \
         /sys/class/amhdmitx/amhdmitx0/dv_cap \
         /sys/class/amhdmitx/amhdmitx0/disp_cap \
         /sys/class/amhdmitx/amhdmitx0/hdr_cap_vivid; do
  if [ -f "$f" ]; then
    echo "--- $f ---"
    cat "$f" 2>/dev/null | head -30
    echo ""
  fi
done
echo "--- 搜 sysfs 里出现 vivid 的节点 ---"
grep -ril "vivid" /sys/class/amhdmitx /sys/module 2>/dev/null | head -10 || echo "  (未发现)"

echo "--- 内核是否支持 HDR Vivid（搜索启动日志）---"
dmesg 2>/dev/null | grep -iE "vivid|hdr10|hdr_vivid|dolby" | tail -15 || echo "  (无匹配)"

# --- 6. 插件安装状态 -------------------------------------------------------
section "6. inputstream.vivid 插件状态"
echo "插件目录: $ADDON_DIR"
if [ -d "$ADDON_DIR" ]; then
  ls -l "$ADDON_DIR" 2>/dev/null
  echo ""
  if [ -f "$ADDON_DIR/addon.xml" ]; then
    echo "--- addon.xml 关键字段 ---"
    grep -E "^\s*<(addon|import|extension)|library_linux|platform>|version=" "$ADDON_DIR/addon.xml" | sed 's/^/  /'
  fi
  SO=$(ls "$ADDON_DIR"/*.so 2>/dev/null | head -1)
  if [ -n "$SO" ]; then
    elf_info "$SO"
  else
    echo "  [!!] 目录里没有 .so"
  fi
else
  echo "  [!!] 插件目录不存在（还没装，或路径不对）"
fi

echo ""
echo "--- 其他已安装的 inputstream 类插件（参考）---"
ls -d /storage/.kodi/addons/inputstream.* 2>/dev/null | head -20 || echo "  (无)"

echo ""
echo "--- 插件是否被启用（Kodi 21+ 用 Addons 数据库记录）---"
if [ -f /storage/.kodi/userdata/Database/Addons33.db ]; then
  echo "  Addons33.db 存在（启用状态需在 Kodi 界面或数据库里查）"
else
  ls /storage/.kodi/userdata/Database/Addons*.db 2>/dev/null | sed 's/^/  /'
fi

# --- 7. Kodi 日志 ----------------------------------------------------------
section "7. Kodi 日志（最近相关片段）"
if [ -f "$KODI_LOG" ]; then
  echo "日志: $KODI_LOG  大小: $(wc -l < "$KODI_LOG") 行"
  echo ""
  echo "--- 插件加载相关 ---"
  grep -iE "inputstream\.vivid|VividDecoder|creating inputstream instance" "$KODI_LOG" 2>/dev/null | tail -30 || echo "  (无记录 —— 说明插件还没被 Kodi 尝试加载)"
  echo ""
  echo "--- 硬解 / amcodec 相关 ---"
  grep -iE "amcodec|AMLCodec|CAMLCodec|aml_vid|codec_init" "$KODI_LOG" 2>/dev/null | tail -30 || echo "  (无记录)"
  echo ""
  echo "--- HDR / Vivid 相关 ---"
  grep -iE "vivid|hdr10|hdr10\+|dolby|bt2020|2084" "$KODI_LOG" 2>/dev/null | tail -30 || echo "  (无记录)"
  echo ""
  echo "--- 最近 30 条 ERROR ---"
  grep -iE "error|failed|cannot|undefined symbol" "$KODI_LOG" 2>/dev/null | tail -30 || echo "  (无)"
else
  echo "  [!!] 找不到 $KODI_LOG"
  ls -l /storage/.kodi/temp/ 2>/dev/null | head
fi

# --- 8. 自动结论 -----------------------------------------------------------
section "8. 自动结论"

VERDICT=""
if [ -n "$FOUND_LIB" ]; then
  A=$(has_sym "$FOUND_LIB" aml_vid_open)
  B=$(has_sym "$FOUND_LIB" codec_init)
  echo "  libamcodec: $FOUND_LIB"
  echo "    含 aml_vid_open（插件期望的符号）: $A"
  echo "    含 codec_init （真实 amcodec API）: $B"
  if [ "$A" = "no" ] && [ "$B" = "yes" ]; then
    VERDICT="插件 dlsym 的 aml_vid_* 在本机库里不存在 → VividDecoder 的解码路径不可能生效（已判空，不会崩）。硬解只能交给 Kodi 原生 amcodec。"
  elif [ "$A" = "yes" ]; then
    VERDICT="库里居然真有 aml_vid_* 符号，与之前调研的公开 amcodec API 不符，请把报告发回，需要重新评估。"
  fi
else
  VERDICT="本机没有 libamcodec.so，NO 分支的解码路径需按新接口重写。"
fi
echo ""
echo "  判定: $VERDICT"

echo ""
if grep -q "creating inputstream instance" "$KODI_LOG" 2>/dev/null; then
  echo "  插件加载: ✅ 成功（Kodi 已创建插件实例）"
elif [ -d "$ADDON_DIR" ]; then
  echo "  插件加载: ⚠️ 已安装但日志无创建记录 —— 可能未启用，或加载失败（看第 7 节的 ERROR）"
else
  echo "  插件加载: ⏸ 尚未安装，跳过"
fi

echo ""
echo "=============================================================="
echo "报告已保存: $OUT"
echo "把整个文件发回即可（如果方便，也附上播放一个 Vivid 片源后的 kodi.log）"
echo "=============================================================="
} 2>&1 | tee "$OUT"
