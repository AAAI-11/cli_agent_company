#!/bin/zsh
# 素材分区构建+烧录：tools/assets_png/*.png → asset 分区（2MB FAT，长文件名）。
#
# 用法（在固件工程根目录，先 source esp-idf export.sh）：
#   ./tools/mkassets.sh              # 生成 FAT 镜像并烧录到 asset 分区
#   ./tools/mkassets.sh --image-only # 只生成镜像不烧录（build/asset.img）
#
# 前置：tools/gen_test_pngs.py 先跑过，或美术素材已放进 tools/assets_png/
# （规格 352x352 透明背景，见 virtual_device/assets/README.md）。
set -euo pipefail

ROOT="${0:a:h:h}"
SRC="$ROOT/tools/assets_png"
IMG="$ROOT/build/asset.img"
IDF="${IDF_PATH:-$HOME/.espressif/v6.1/esp-idf}"
PORT="${PORT:-/dev/cu.usbmodem5B910634541}"
IMAGE_ONLY=0
[[ "${1:-}" == "--image-only" ]] && IMAGE_ONLY=1

[[ -d "$SRC" ]] || { echo "缺 $SRC（先跑 python3 tools/gen_test_pngs.py）"; exit 1; }
ls "$SRC"/*.png >/dev/null

FATFSGEN="$IDF/components/fatfs/fatfsgen.py"
PARTTOOL="$IDF/components/partition_table/parttool.py"
[[ -x "$FATFSGEN" ]] || FATFSGEN=python3" $FATFSGEN"

# --long_name_support：waiting_confirm.png 超 8.3；--partition_size 与分区表 2MB 对齐
rm -f "$IMG"
"$FATFSGEN" "$SRC" --long_name_support --partition_size 0x200000 --output_file "$IMG"
echo "镜像 $IMG ($(stat -f%z "$IMG") bytes)"

(( IMAGE_ONLY )) && exit 0

# 烧录前让 daemon 松开串口（ESPTOOL 独占）
DAEMON=127.0.0.1:7331
PAUSED=0
curl -s -m 2 -X POST "http://$DAEMON/usb/pause" >/dev/null 2>&1 && PAUSED=1 && sleep 1

python3 "$PARTTOOL" -p "$PORT" write_partition --partition-name asset --input "$IMG"

(( PAUSED )) && curl -s -m 2 -X POST "http://$DAEMON/usb/resume" >/dev/null
echo "asset 分区烧录完成，复位设备后 pet_assets_init 自动挂载"
