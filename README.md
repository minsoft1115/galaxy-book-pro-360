# Samsung Galaxy Book Pro 360 (NT950QDB) Linux Fingerprint Setup

이 디렉토리는 **Samsung Galaxy Book Pro 360 (`NT950QDB`)**에 탑재된 **EgisTec EH57E (`1c7a:057e`)** 지문 인식 센서를 Linux에서 사용하기 위한 소스 코드 및 자동 빌드/설치 환경입니다.

---

## 📁 디렉토리 구조

- [`install.sh`](./install.sh): 의존성 설치, 드라이버 통합, 빌드, 격리 설치, systemd/udev 설정을 한 번에 수행하는 스크립트
- [`uninstall.sh`](./uninstall.sh): 설치된 라이브러리 및 설정을 깔끔하게 삭제하고 시스템 기본 상태로 되돌리는 스크립트
- [`driver/`](./driver/): 사전 최적화된 EH57E 드라이버 소스 (`egis057e.c`, `egis057e.h`)
- `egistec-eh57e-linux/`: 최신 EgisTec EH57E libfprint 드라이버 소스 ([ApexMene 저장소](https://github.com/ApexMene/egistec-eh57e-linux))
- `eh57e-linux-driver/`: 역공학 분석 및 USB 프로브 도구 ([Cruise42 저장소](https://github.com/Cruise42/eh57e-linux-driver))
- `libfprint-src/`: 업스트림 libfprint 소스 트리 ([freedesktop.org 저장소](https://gitlab.freedesktop.org/libfprint/libfprint))

---

## 🚀 설치 방법

터미널에서 아래 명령어를 실행합니다:

```bash
cd ~/minsoft/galaxy-book-pro-360
chmod +x install.sh uninstall.sh
./install.sh
```

> **안전한 격리 설치 원칙:**
> 시스템 패키지 매니저(`pacman`)의 기본 파일들을 덮어쓰지 않고 `/usr/local/lib/egis057e/` 경로에 전용 라이브러리를 설치한 뒤, `fprintd` systemd drop-in override를 통해 우선 로드되도록 구성됩니다. 시스템 업데이트 시에도 충돌이 발생하지 않습니다.

---

## 🖐️ 지문 등록 및 검증 사용법

### 1. 지문 등록 (Enroll)
```bash
fprintd-enroll
```
- 지문 등록 프롬프트가 뜨면 센서(전원 버튼)에 손가락을 **살짝 대었다 뗐다를 반복**합니다.
- **주의:** 전원 버튼을 세게 '딸깍' 누르면 절전 모드로 들어갈 수 있으니, 살짝 터치만 하세요.

### 2. 지문 검증 (Verify)
```bash
fprintd-verify
```
- 등록된 손가락을 대면 `verify-match (swipe/touch)` 성공 메시지가 출력됩니다.

### 3. sudo 및 락스크린 연동 (선택)
지문 인증이 정상 작동하면 PAM 설정을 통해 sudo 및 화면 잠금 해제에 지문을 사용할 수 있습니다.
(Arch/Omarchy 환경에서는 `pam-auth-update` 또는 `/etc/pam.d/system-auth`에 `pam_fprintd.so` 연동)

---

## 🔄 원상복구 (삭제)

```bash
cd ~/minsoft/galaxy-book-pro-360
./uninstall.sh
```
설치된 라이브러리와 서비스 오버라이드가 완전히 제거되고 순정 상태로 복구됩니다.

---

## 🔗 참고 자료 및 크레딧 (References & Credits)

본 프로젝트의 드라이버 빌드 및 설치 자동화 환경은 아래 오픈소스 프로젝트와 기여자분들의 연구 및 작업물을 바탕으로 구성되었습니다:

- **드라이버 구현체:** [ApexMene/egistec-eh57e-linux](https://github.com/ApexMene/egistec-eh57e-linux)
  - EgisTec EH57E 센서를 위한 libfprint 드라이버 (`egis057e.c`, `egis057e.h`) 구현 및 패치
- **역공학 및 프로토콜 분석:** [Cruise42/eh57e-linux-driver](https://github.com/Cruise42/eh57e-linux-driver)
  - Windows 드라이버 USB 트래픽 캡처, EH57E 초기화 시퀀스 역공학 및 파이썬 프로브 도구
- **업스트림 라이브러리:** [freedesktop.org libfprint](https://gitlab.freedesktop.org/libfprint/libfprint)
  - Linux 공식 지문 인식 프레임워크 라이브러리 (빌드 베이스)

