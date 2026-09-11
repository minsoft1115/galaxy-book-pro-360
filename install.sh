#!/usr/bin/env bash
#
# Galaxy Book Pro 360 (NT950QDB) - EgisTec EH57E (1c7a:057e)
# Fingerprint Driver Build & Isolated Install Script
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIBFPRINT_SRC="$SCRIPT_DIR/libfprint-src"
DRIVER_SRC="$SCRIPT_DIR/egistec-eh57e-linux"
PROBE_TOOLS="$SCRIPT_DIR/eh57e-linux-driver"
BUILD_DIR="$SCRIPT_DIR/build"

DEST_DIR="/usr/local/lib/egis057e"
DROPIN_DIR="/etc/systemd/system/fprintd.service.d"
UDEV_RULE="/etc/udev/rules.d/99-egis057e.rules"

echo "============================================================"
echo " Samsung Galaxy Book Pro 360 지문 인식 드라이버 설치 스크립트"
echo "============================================================"

# 1. 필수 빌드 패키지 확인 및 설치
echo "[1/7] 필수 패키지 확인 중..."
MISSING_PKGS=()
for cmd in gcc pkg-config git python3; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        MISSING_PKGS+=("$cmd")
    fi
done

for cmd in meson ninja; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        MISSING_PKGS+=("$cmd")
    fi
done

if ! command -v glib-mkenums >/dev/null 2>&1; then
    MISSING_PKGS+=("glib2-devel")
fi

if [ ${#MISSING_PKGS[@]} -gt 0 ]; then
    echo "필요한 패키지가 누락되어 pacman으로 설치를 시도합니다: ${MISSING_PKGS[*]}"
    sudo pacman -S --needed --noconfirm "${MISSING_PKGS[@]}"
fi

# 2. 소스 코드 확인 및 클론 (누락된 경우)
echo "[2/7] 소스 코드 확인 중..."
if [ ! -d "$DRIVER_SRC" ]; then
    echo "  -> egistec-eh57e-linux 클론 중..."
    git clone https://github.com/ApexMene/egistec-eh57e-linux.git "$DRIVER_SRC"
fi

if [ ! -d "$PROBE_TOOLS" ]; then
    echo "  -> eh57e-linux-driver 클론 중..."
    git clone https://github.com/Cruise42/eh57e-linux-driver.git "$PROBE_TOOLS"
fi

if [ ! -d "$LIBFPRINT_SRC" ]; then
    echo "  -> libfprint 최신 소스 클론 중..."
    git clone --depth 1 https://gitlab.freedesktop.org/libfprint/libfprint.git "$LIBFPRINT_SRC"
fi

# 3. libfprint 소스 트리에 드라이버 통합
echo "[3/7] libfprint에 egis057e 드라이버 등록 중..."
if [ -d "$SCRIPT_DIR/driver" ]; then
    cp -f "$SCRIPT_DIR/driver/egis057e.c" "$LIBFPRINT_SRC/libfprint/drivers/"
    cp -f "$SCRIPT_DIR/driver/egis057e.h" "$LIBFPRINT_SRC/libfprint/drivers/"
else
    cp -f "$DRIVER_SRC/driver/egis057e.c" "$LIBFPRINT_SRC/libfprint/drivers/"
    cp -f "$DRIVER_SRC/driver/egis057e.h" "$LIBFPRINT_SRC/libfprint/drivers/"
fi

LIBFPRINT_SRC="$LIBFPRINT_SRC" python3 - <<'PY'
import os
import pathlib
import sys

base = pathlib.Path(os.environ["LIBFPRINT_SRC"])

for path, anchor, entry in (
    (base / "meson.build",
     "    'egis0570': {},",
     "    'egis057e': {},"),
    (base / "libfprint" / "meson.build",
     "    'egis0570' : files('drivers/egis0570.c'),",
     "    'egis057e' : files('drivers/egis057e.c'),"),
):
    p = pathlib.Path(path)
    s = p.read_text()
    if "egis057e" in s:
        print(f"  [이미 등록됨] {p.name}")
        continue
    if anchor not in s:
        sys.exit(f"오류: {path} 에서 기준 위치({anchor})를 찾을 수 없습니다.")
    p.write_text(s.replace(anchor, anchor + "\n" + entry, 1))
    print(f"  [등록 완료] {p.name}")
PY

# 4. 빌드 수행
echo "[4/7] 드라이버 빌드 중 (meson & ninja)..."
cd "$SCRIPT_DIR"
if [ -d "$BUILD_DIR" ] && [ -f "$BUILD_DIR/build.ninja" ]; then
    meson setup "$BUILD_DIR" "$LIBFPRINT_SRC" \
        -Ddrivers=egis057e -Ddoc=false -Dgtk-examples=false -Dintrospection=false --reconfigure
else
    rm -rf "$BUILD_DIR"
    meson setup "$BUILD_DIR" "$LIBFPRINT_SRC" \
        -Ddrivers=egis057e -Ddoc=false -Dgtk-examples=false -Dintrospection=false
fi
ninja -C "$BUILD_DIR"

# 5. 장치 지원 등록 검증
echo "[5/7] 빌드 결과 검증 중..."
if ! "$BUILD_DIR/libfprint/fprint-list-supported-devices" | grep -qi '057e'; then
    echo "오류: 빌드된 라이브러리에 1c7a:057e 장치가 등록되지 않았습니다."
    exit 1
fi
echo "  -> 1c7a:057e 장치가 정상적으로 드라이버에 인식되었습니다."

# 6. /usr/local/lib 격리 설치 및 udev / systemd 설정
echo "[6/7] 격리 경로에 라이브러리 설치 및 서비스 설정 중..."

sudo install -d -m 0755 "$DEST_DIR"
sudo install -m 0755 "$BUILD_DIR/libfprint/libfprint-2.so.2.0.0" "$DEST_DIR/"
sudo ln -sf libfprint-2.so.2.0.0 "$DEST_DIR/libfprint-2.so.2"
sudo ln -sf libfprint-2.so.2     "$DEST_DIR/libfprint-2.so"

# udev 룰 추가
sudo install -d -m 0755 /etc/udev/rules.d
cat <<'EOF' | sudo tee "$UDEV_RULE" >/dev/null
# EgisTec EH57E Fingerprint Sensor (Samsung Galaxy Book Pro 360)
SUBSYSTEM=="usb", ATTR{idVendor}=="1c7a", ATTR{idProduct}=="057e", TAG+="uaccess", MODE="0666"
EOF

sudo udevadm control --reload
sudo udevadm trigger

# systemd drop-in override 추가 (시스템 패키지를 덮어쓰지 않고 fprintd가 커스텀 libfprint를 우선 로드)
sudo install -d -m 0755 "$DROPIN_DIR"
cat <<'EOF' | sudo tee "$DROPIN_DIR/egis057e.conf" >/dev/null
[Service]
Environment=LD_LIBRARY_PATH=/usr/local/lib/egis057e
EOF

sudo systemctl daemon-reload

# 7. 센서 리셋 및 fprintd 재시작
echo "[7/7] USB 센서 리셋 및 fprintd 서비스 재시작 중..."
sudo systemctl stop fprintd.service 2>/dev/null || true
if command -v usbreset >/dev/null 2>&1; then
    sudo usbreset 1c7a:057e 2>/dev/null || true
fi
sudo systemctl restart fprintd.service
sleep 1

echo "============================================================"
echo " 설치가 완료되었습니다!"
echo "============================================================"
echo "장치 등록 테스트:"
echo "  1) 지문 등록: fprintd-enroll"
echo "     (주의: 전원 버튼을 세게 누르지 말고 살짝 손가락을 대었다 뗐다를 반복하세요)"
echo "  2) 지문 검증: fprintd-verify"
echo "  3) 등록된 지문 확인: fprintd-list \$USER"
echo
echo "원상복구가 필요할 때는 ./uninstall.sh 를 실행하세요."
echo "============================================================"
