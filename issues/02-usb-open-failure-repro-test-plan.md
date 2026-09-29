# EH57E USB open 실패 정리 회귀 테스트 계획

## 목적

USB open은 성공했지만 EH57E 드라이버 OPEN이 실패할 때, 확보한 리소스를 해제하고
같은 libfprint 장치 객체에서 다음 OPEN이 다시 진행되는지 확인한다. sudo-pop 취소가
최초 실패를 유발한다고 전제하지 않는다.

## 검증 조건

- 원래 OPEN 오류가 호출자에게 전달된다.
- 인터페이스를 확보한 경우 release가 한 번 호출된다.
- USB handle close가 한 번 호출된다.
- 실패한 OPEN 뒤 장치가 `is_open == FALSE`로 남는다.
- 인터페이스 확보·초기화 전송·배경 캡처 오류 뒤 같은 객체를 재시도하면 새 OPEN이 진행된다.
- 정상 OPEN/CLOSE에서 기존 정리가 유지되고 중복 해제가 발생하지 않는다.
- 취소 또는 장치 제거가 비동기 전송 완료와 겹쳐도 해제 후 callback/use-after-free 문제가 없다.

## 오류 주입 시나리오

각 시나리오는 실제 EH57E USB 장치 대신 USB 동작을 계측할 수 있는 테스트 seam 또는
가짜 GUsb backend로 수행한다. 실패 지점만 주입하고 cleanup과 재시도는 같은 프로세스,
같은 `FpDevice` 객체에서 관찰한다.

### A. 인터페이스 확보 실패

1. 공통 코드의 USB open을 성공시킨다.
2. `g_usb_device_claim_interface()`가 지정한 오류로 실패하도록 한다.
3. OPEN callback이 지정 오류를 받는지 확인한다.
4. 인터페이스 release는 호출하지 않고 USB close는 한 번 호출됐는지 확인한다.
5. 주입을 끄고 같은 객체에서 다시 OPEN해 드라이버 callback까지 도달하는지 확인한다.

### B. 초기화 전송 실패

1. USB open과 인터페이스 확보를 성공시킨다.
2. 초기화 SSM의 전송 하나가 오류를 반환하게 한다.
3. 원래 전송 오류가 전달되고 인터페이스 release와 USB close가 각각 한 번 호출되는지 확인한다.
4. 같은 객체에서 OPEN을 재시도해 USB open과 인터페이스 확보가 다시 성공하는지 확인한다.

### C. 배경 이미지 캡처 실패

1. USB open, 인터페이스 확보, 초기화 SSM을 성공시킨다.
2. `PH_BACKGROUND` 캡처가 오류를 반환하게 한다.
3. 원래 캡처 오류와 리소스 해제 횟수를 확인한다.
4. 같은 객체에서 OPEN 재시도가 성공하는지 확인한다.

### D. 정상 및 경쟁 조건

- 정상 OPEN → CLOSE에서 interface release/USB close가 각각 한 번인지 확인한다.
- 초기화/배경 전송 callback이 완료되기 전에 취소를 요청하고, 전송 완료 후 오류 정리가 한 번만 수행되는지 확인한다.
- 장치 제거와 OPEN 오류가 겹칠 때 원래 오류 보존, 안전한 callback 종료, 재연결 뒤 새 객체의 OPEN 성공을 확인한다.

## 실장치 회귀 확인

`tests/run-usb-open-retry.sh LIBFPRINT_LIBRARY_DIRECTORY`는 libfprint를 직접 로드하는
테스트 프로세스에서 20ms 뒤 첫 OPEN을 취소하고 같은 `FpDevice` 객체로 재시도한다.
예를 들어 수정 빌드로 실행할 때:

```sh
tests/run-usb-open-retry.sh build/libfprint
```

설치본을 기준선으로 비교하려면 `LIBFPRINT_LIBRARY_DIRECTORY`에
`/usr/local/lib/egis057e`를 전달한다. 이 도구는 fprintd를 거치지 않지만 같은 libfprint
USB open 및 EH57E driver 경로를 사용한다. 오류 주입 테스트와 별개로, sudo-pop/fprintd
연동 회귀도 확인한다. fprintd 검증 중에는 서비스를 재시작하지 않아 동일 프로세스의 상태를
유지한다.

1. `fprintd-verify lmh`를 실행해 지문 대기 및 취소 동작을 확인한다.
2. 실패 주입을 지원하는 빌드에서 OPEN 초기화 또는 배경 캡처 오류를 한 번 발생시킨다.
3. 주입을 끈 뒤 즉시 같은 fprintd 프로세스에서 `fprintd-verify lmh`를 재실행한다.
4. 재시도가 `Device 1c7a:057e is already open` 없이 장치 OPEN 단계에 도달하는지 확인한다.
5. 정상 지문, 불일치, 타임아웃, 취소 후 재요청의 PAM 동작을 확인한다.

sudo-pop 취소와 드라이버 OPEN 실패의 직접 인과관계는 별도 로그가 확보되기 전까지
미확정으로 기록한다. 기존 `test_driver_open_error()`는 가상 장치만 사용하므로 이 계획의
USB handle 검증을 대체하지 않는다.

## 현재 실행 기록

- `ninja -C build`: 통과.
- `meson test -C build --suite unit-tests --print-errorlogs`: 3개 단위 테스트 모두 통과.
- `build/tests/test-fpi-device -p /driver/open/error`: 통과. 가상 장치 오류 경로만 확인한다.
- 실장치 baseline `/usr/local/lib/egis057e`: 20ms OPEN 취소가 `Operation was cancelled`로
  반환된 뒤 같은 객체의 retry가 `Device 1c7a:057e is already open`으로 실패했다.
- 수정 빌드 `build/libfprint`: 동일 시나리오를 연속 두 번 실행해 두 번 모두 첫 OPEN 취소
  후 같은 객체의 retry OPEN과 CLOSE가 성공했다.
- 수정 라이브러리를 `/usr/local/lib/egis057e`에 설치했고 설치본 SHA-256이 빌드 결과와
  일치한다: `9d82e400825efec272ccd107176089a74d2443b15b0f3099aa1b049c92537190`.
- 설치 후 `fprintd-verify lmh`가 두 번 모두 `Verify started` / `Verifying`까지 도달했다.
  첫 요청을 8초 제한으로 종료한 뒤 재요청도 `already open` 없이 시작됐다. 지문은 대지 않아
  두 실행 모두 검증 대기 시간 초과로 끝났다.
- 전체 `meson test -C build --print-errorlogs`: appstream 검증은 네트워크 DNS 실패,
  udev-hwdb 검증은 현재 `egis057e` 단일 드라이버 구성에서 발생한 fatal warning으로 실패했다.
- sandbox 안에서는 libusb 초기화가 차단됐으나, 승인된 실장치 테스트에서는 EH57E를 열거하고
  취소/재시도 비교를 완료했다.
- 초기화 전송과 배경 캡처의 개별 실패 주입, sudo-pop/PAM 경유 검증, 지문 일치/불일치
  회귀는 아직 수행하지 않았다.
