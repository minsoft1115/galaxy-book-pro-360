# Samsung Galaxy Book Pro 360 (NT950QDB) Linux Fingerprint Setup

이 저장소는 **Samsung Galaxy Book Pro 360 (`NT950QDB`)**에 탑재된 **EgisTec EH57E (`1c7a:057e`)** 지문 인식 센서를 Linux(Arch / Arch 계열) 환경에서 손쉽게 사용하기 위한 **원클릭 자동 빌드 및 격리 설치 환경**입니다.

기존 오픈소스 연구 및 드라이버 작업물을 기반으로, 최신 시스템 라이브러리와 충돌 없이 안전하게 드라이버를 빌드하고 systemd/udev 서비스에 통합하도록 구성되어 있습니다.

---

## 📁 저장소 구성

이 저장소에는 실제 설치 및 관리에 필요한 핵심 파일들만 포함되어 있습니다:

- [`driver/`](./driver/): 최신 환경에 맞춰 사전 최적화된 EgisTec EH57E 드라이버 소스 코드 (`egis057e.c`, `egis057e.h`)
- [`install.sh`](./install.sh): 필수 의존성 확인, 빌드용 업스트림 소스 자동 다운로드, 드라이버 코드 통합, 격리 빌드/설치, systemd/udev 설정을 일괄 수행하는 원클릭 설치 스크립트
- [`uninstall.sh`](./uninstall.sh): 설치된 전용 라이브러리와 서비스 오버라이드를 완전히 제거하고 시스템을 순정 상태로 복구하는 스크립트

> **💡 빌드 동작 원리:** 스크립트 실행 시 필요한 업스트림 libfprint 소스 트리 등은 빌드 과정에서 자동으로 임시 다운로드(git clone)되어 사용되며, 로컬 저장소 형상 관리(`.gitignore`)에서는 깔끔하게 제외됩니다.

---

## 🚀 설치 방법

터미널에서 아래 명령어를 실행합니다:

```bash
cd ~/minsoft/galaxy-book-pro-360
chmod +x install.sh uninstall.sh
./install.sh
```

### 🛡️ 안전한 격리 설치 원칙
시스템 패키지 매니저(`pacman`)의 기본 파일들을 덮어쓰지 않고 `/usr/local/lib/egis057e/` 경로에 전용 라이브러리를 설치한 뒤, `fprintd` systemd drop-in override를 통해 우선 로드되도록 구성됩니다. 시스템 업데이트 시에도 충돌이 발생하지 않습니다.

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

