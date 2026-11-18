#!/bin/sh
# ============================================================================
# package-addon.sh — 把编译产物打成 Kodi 可直接安装的插件 zip
#
# 用法：
#   sh tools/package-addon.sh [build_dir] [out_dir]
#   sh tools/package-addon.sh build dist          # CI 里的用法
#
# 产物：<out_dir>/<addonid>-<version>-linux-aarch64.zip
#       <out_dir>/<addonid>-<version>-linux-aarch64.zip.sha256
#
# zip 内部结构（Kodi 硬性要求，错了会报 "结构不正确"）：
#   inputstream.vivid/addon.xml
#   inputstream.vivid/inputstream.vivid.so
#   inputstream.vivid/LICENSE          (存在则带)
#   inputstream.vivid/resources/...    (存在则带)
#
# 设计要点：
#   - addon id / version 一律从 addon.xml 读取，不在脚本里硬编码，避免两处不一致
#   - 只用一个顶层目录；Kodi 安装时按「顶层目录名 == addon id」校验
#   - 用 python3 生成 zip（固定时间戳），不依赖 zip 命令是否存在
# ============================================================================
set -eu

BUILD_DIR=${1:-build}
OUT_DIR=${2:-dist}
ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)

cd "$ROOT_DIR"

PY=$(command -v python3 2>/dev/null || command -v python 2>/dev/null || true)
[ -n "$PY" ] || { echo "FAIL: 找不到 python3/python，无法读取 addon.xml 并打包"; exit 1; }

[ -f addon.xml ] || { echo "FAIL: 当前目录没有 addon.xml（$ROOT_DIR）"; exit 1; }

# ---------------------------------------------------------------- 读元信息 --
META=$("$PY" - addon.xml <<'EOF'
import sys, xml.etree.ElementTree as ET
r = ET.parse(sys.argv[1]).getroot()
print(r.get('id') or '')
print(r.get('version') or '')
EOF
)
ADDON_ID=$(echo "$META" | sed -n '1p')
VERSION=$(echo "$META" | sed -n '2p')

[ -n "$ADDON_ID" ] || { echo "FAIL: addon.xml 缺少 id"; exit 1; }
[ -n "$VERSION" ]  || { echo "FAIL: addon.xml 缺少 version"; exit 1; }

echo "== addon: $ADDON_ID  version: $VERSION"

# addon.xml 声明的库文件名，必须与打包进去的一致
LIB_NAME=$("$PY" - addon.xml <<'EOF'
import sys, xml.etree.ElementTree as ET
r = ET.parse(sys.argv[1]).getroot()
for e in r.findall('extension'):
    if e.get('point') == 'kodi.inputstream' and e.get('library_linux'):
        print(e.get('library_linux')); break
EOF
)
[ -n "$LIB_NAME" ] || { echo "FAIL: addon.xml 的 kodi.inputstream 扩展没有 library_linux 属性"; exit 1; }
echo "== 声明的库文件: $LIB_NAME"

# ------------------------------------------------------------------ 找产物 --
SO=$(find "$BUILD_DIR" -name "*.so" -type f 2>/dev/null | head -n1)
[ -n "$SO" ] || { echo "FAIL: $BUILD_DIR 下没有 .so，先编译：cmake -B build -S . && cmake --build build"; exit 1; }

SO_SIZE=$(wc -c < "$SO")
[ "$SO_SIZE" -gt 0 ] || { echo "FAIL: $SO 是空文件"; exit 1; }
[ "$SO_SIZE" -gt 10000 ] || echo "WARN: $SO 只有 $SO_SIZE 字节，疑似空壳"

# ELF 魔数校验，挡掉「误把文本/别的架构文件当产物」的情况
magic=$(head -c 4 "$SO" | od -An -tx1 | tr -d ' \n')
[ "$magic" = "7f454c46" ] || { echo "FAIL: $SO 不是 ELF 文件（magic=$magic）"; exit 1; }
echo "== 产物: $SO ($SO_SIZE 字节, ELF)"

# ------------------------------------------------------------------ 组装目录 --
STAGE=$(mktemp -d 2>/dev/null || echo "./.pkg-stage.$$")
mkdir -p "$STAGE"
trap 'rm -rf "$STAGE" 2>/dev/null || true' EXIT INT TERM
DEST="$STAGE/$ADDON_ID"
mkdir -p "$DEST"

cp addon.xml "$DEST/addon.xml"
cp "$SO" "$DEST/$LIB_NAME"
[ -f LICENSE ]   && cp LICENSE   "$DEST/LICENSE"
[ -f icon.png ]  && cp icon.png  "$DEST/icon.png"
[ -f fanart.jpg ] && cp fanart.jpg "$DEST/fanart.jpg"
if [ -d resources ]; then
  cp -R resources "$DEST/resources"
fi

echo "== 打包内容:"
find "$DEST" -type f | sed "s|$STAGE/|  |"

# --------------------------------------------------------------------- zip --
mkdir -p "$OUT_DIR"
ZIP_NAME="${ADDON_ID}-${VERSION}-linux-aarch64.zip"
ZIP="$OUT_DIR/$ZIP_NAME"
rm -f "$ZIP" "$ZIP.sha256"

# 固定时间戳，同样的内容产出同样的 zip 头，便于比对
"$PY" - "$STAGE" "$ZIP" <<'EOF'
import os, sys, zipfile

stage, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for root, dirs, files in os.walk(stage):
        dirs.sort()
        for f in sorted(files):
            full = os.path.join(root, f)
            rel = os.path.relpath(full, stage).replace(os.sep, '/')
            zi = zipfile.ZipInfo(rel, date_time=(1980, 1, 1, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = (0o644 & 0xFFFF) << 16
            with open(full, 'rb') as fh:
                z.writestr(zi, fh.read())
print('zip 写入完成:', out)
EOF

# ---------------------------------------------------------------- 校验结构 --
"$PY" - "$ZIP" "$ADDON_ID" "$LIB_NAME" "$VERSION" <<'EOF'
import sys, zipfile, xml.etree.ElementTree as ET

path, addon_id, lib, version = sys.argv[1:5]
z = zipfile.ZipFile(path)
names = z.namelist()

tops = {n.split('/')[0] for n in names}
if len(tops) != 1:
    print('FAIL: zip 顶层目录不止一个: %r' % sorted(tops)); sys.exit(1)
if tops.pop() != addon_id:
    print('FAIL: zip 顶层目录不是 addon id（%s）' % addon_id); sys.exit(1)

for need in ('addon.xml', lib):
    key = '%s/%s' % (addon_id, need)
    if key not in names:
        print('FAIL: zip 里缺少 %s' % key); sys.exit(1)

root = ET.fromstring(z.read('%s/addon.xml' % addon_id))
if root.get('version') != version:
    print('FAIL: zip 内 addon.xml 版本 %r != %r' % (root.get('version'), version)); sys.exit(1)

print('OK: 结构合法，%d 个文件' % len(names))
for n in names:
    print('   %10d  %s' % (z.getinfo(n).file_size, n))
EOF

SIZE=$(wc -c < "$ZIP")
SHA=$("$PY" - "$ZIP" <<'EOF'
import hashlib, sys
print(hashlib.sha256(open(sys.argv[1], 'rb').read()).hexdigest())
EOF
)
printf '%s  %s\n' "$SHA" "$ZIP_NAME" > "$ZIP.sha256"

echo ""
echo "== 打包完成"
echo "   $ZIP  ($SIZE 字节)"
echo "   sha256: $SHA"
echo ""
echo "== 安装方法（CoreELEC）"
echo "   Kodi 界面：设置 → 插件 → 从 zip 文件安装"
echo "   或 SSH（zip 内已含 $ADDON_ID/ 顶层目录，解到 addons 根目录即可）："
echo "     cd /storage/.kodi/addons && unzip -o /path/$ZIP_NAME"
echo "     systemctl restart kodi"
