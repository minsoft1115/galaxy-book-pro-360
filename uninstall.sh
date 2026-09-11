#!/usr/bin/env bash
#
# Galaxy Book Pro 360 (NT950QDB) - EgisTec EH57E (1c7a:057e)
# Fingerprint Driver Uninstall & Rollback Script
#
set -euo pipefail

DEST_DIR="/usr/local/lib/egis057e"
DROPIN_CONF="/etc/systemd/system/fprintd.service.d/egis057e.conf"
DROPIN_DIR="/etc/systemd/system/fprintd.service.d"
UDEV_RULE="/etc/udev/rules.d/99-egis057e.rules"

echo "============================================================"
echo " Samsung Galaxy Book Pro 360 지문 드라이버 제거 (원상복구)"
echo "============================================================"

echo "1. fprintd 서비스 중지..."
sudo systemctl stop fprintd.service 2>/dev/null || true

echo "2. 격리 설치된 libfprint 라이브러리 삭제..."
if [ -d "$DEST_DIR" ]; then
    sudo rm -rf "$DEST_DIR"
    echo "   -> $DEST_DIR 삭제 완료"
fi

echo "3. systemd drop-in 설정 삭제..."
if [ -f "$DROPIN_CONF" ]; then
    sudo rm -f "$DROPIN_CONF"
    echo "   -> $DROPIN_CONF 삭제 완료"
fi
# 폴더가 비어있으면 폴더도 정리
if [ -d "$DROPIN_DIR" ] && [ -z "$(ls -A "$DROPIN_DIR" 2>/dev/null)" ]; then
    sudo rmdir "$DROPIN_DIR" 2>/dev/null || true
fi

echo "4. udev 규칙 삭제..."
if [ -f "$UDEV_RULE" ]; then
    sudo rm -f "$UDEV_RULE"
    echo "   -> $UDEV_RULE 삭제 완료"
fi

echo "5. 시스템 설정 리로드 및 서비스 재시작..."
sudo udevadm control --reload
sudo udevadm trigger
sudo systemctl daemon-reload
sudo systemctl restart fprintd.service 2>/dev/null || true

echo "============================================================"
echo " 드라이버가 완전히 제거되었으며, 시스템 기본 상태로 복구되었습니다."
echo "============================================================"
