# PhoneCam

**[Magyar](#magyar) · [English](#english)**

Use your Android phone's camera as a low-latency webcam on Windows 11 (Discord, OBS, Teams,
Zoom, browsers). / Android telefon kamerája alacsony késésű webkameraként Windows 11-en.

## ⬇️ Letöltés / Download

[**Legfrissebb kiadás / Latest release**](../../releases/latest)

| Fájl / File | Hova / Where |
|---|---|
| `PhoneCam.exe` | Windows 11 PC – önálló, nem kell mellé semmi / standalone, nothing else needed |
| `PhoneCam.apk` | Android telefon/tablet (Android 11+) / Android phone or tablet |

---

## Magyar

A telefon natív kamerája hardveres H.264/H.265 kódolással megy a PC-re Wi-Fi-n vagy
USB-n. A PC-n három valódi Windows virtuális kamera jelenik meg (**PhoneCam**,
**PhoneCam 2**, **PhoneCam 3**, mind ugyanazt a képet adja).

### Használat
1. **PC:** töltsd le és indítsd el a `PhoneCam.exe`-t. Első indításkor egyszer rendszergazdai
   jóváhagyást kér (UAC → Igen): ekkor telepíti a virtuális kamerákat és tűzfalszabályt ad
   hozzá a 47801-es TCP porthoz. A Windows SmartScreen figyelmeztethet (nem aláírt program):
   „További információ” → „Futtatás mindenképp”.
2. **Telefon:** telepítsd a `PhoneCam.apk`-t (engedélyezd az ismeretlen forrásból való
   telepítést), indítsd el, engedélyezd a kamerát.
3. Ugyanazon a Wi-Fi-n pár másodpercen belül magától csatlakozik. Ha nem, a PC ablakában
   látod a gép IP címét: írd be a telefonon a Beállítások → „PC IP címe” mezőbe.
4. Discordban / OBS-ben válaszd a **PhoneCam** kamerát.

- A kamerák mindig léteznek (újraindítás után is), így az OBS indításkor is megtalálja őket.
  Ha a program nem fut vagy nincs telefon, a kamera ezt felirattal mutatja.
- **Több program egyszerre:** a Windows egy kamerát egyszerre csak egy programnak ad, ezért
  van három: pl. Discordban a „PhoneCam”, OBS-ben a „PhoneCam 2”.
- **USB mód** (legkisebb késés): telefonon Fejlesztői beállítások → USB hibakeresés be,
  kábel be, RSA kulcs elfogadása – a telefon magától USB-re vált.
- **Nyelv:** a PC ablak fejlécében HU/EN kapcsoló, a telefonon Beállítások → Nyelv.

### PC ablak
Élő előnézet, a telefon neve és akkuszintje, fps, bitráta, késés, hálózati RTT, dekódolás.
A telefon beállításai innen is állíthatók: kamera/lencse, felbontás, fps, kodek, bitráta,
stabilizálás, vaku és zoom. Tükrözés, 180°-os forgatás.

### Hibaelhárítás
- **Nem találja a PC-t:** a router kliens-izolációja blokkolhatja a keresést → írd be kézzel az IP-t.
- **Nem jelenik meg a kamera:** Windows 11 kell; Gépház → Adatvédelem → Kamera → asztali
  alkalmazások hozzáférése legyen bekapcsolva.
- **Eltávolítás:** rendszergazdai parancssorból `PhoneCam.exe --uninstall`.

---

## English

Your phone's native camera is encoded in hardware (H.264/H.265) and sent to the PC over Wi-Fi
or USB. The PC gets three real Windows virtual cameras (**PhoneCam**, **PhoneCam 2**,
**PhoneCam 3**, all showing the same picture).

### Usage
1. **PC:** download and run `PhoneCam.exe`. On the first start it asks for administrator
   approval once (UAC → Yes) to install the virtual cameras and a firewall rule for TCP port
   47801. Windows SmartScreen may warn (unsigned app): "More info" → "Run anyway".
2. **Phone:** install `PhoneCam.apk` (allow installing from unknown sources), open it and allow
   the camera.
3. On the same Wi-Fi it connects within a few seconds. If not, the PC window shows the PC's IP
   address: enter it on the phone under Settings → "PC IP address".
4. In Discord / OBS select the **PhoneCam** camera.

- The cameras always exist (also after a reboot), so OBS finds them at startup. When the
  program is not running or no phone is connected, the camera shows a message.
- **Several programs at once:** Windows gives a camera to one program at a time, that's why
  there are three: e.g. "PhoneCam" in Discord, "PhoneCam 2" in OBS.
- **USB mode** (lowest delay): enable USB debugging in the phone's Developer options, plug in
  the cable and accept the RSA key – the phone switches to USB by itself.
- **Language:** HU/EN switch in the header of the PC window; on the phone Settings → Language.

### PC window
Live preview, phone name and battery level, fps, bitrate, latency, network RTT, decoding.
The phone can be controlled from here: camera/lens, resolution, fps, codec, bitrate,
stabilization, flash and zoom. Mirror and 180° rotation.

### Troubleshooting
- **PC not found:** client isolation on the router can block discovery → enter the IP manually.
- **Camera missing:** Windows 11 is required; Settings → Privacy → Camera → let desktop apps
  access the camera.
- **Uninstall:** run `PhoneCam.exe --uninstall` from an administrator command prompt.

---

## Build / Fordítás
- Windows: `windows\build.bat` (Visual Studio 2022 Build Tools, Windows SDK 10.0.26100,
  Android platform-tools for the embedded adb). Self test without a phone:
  `windows\tools\build_selftest.cmd`, then `windows\obj\tools\selftest.exe`.
- Android: `android\gradlew.bat assembleRelease` (JDK 17+, Android SDK 35).

```
phone: Camera2 ──surface──> MediaCodec (HW H.264/H.265) ──TCP 47801──┐
                                                                       │  UDP 47800 beacon from the PC
PC: PhoneCam.exe: receive → MF decoder (D3D11) → NV12 → shared memory ┤
    PhoneCamVCam.dll (Media Foundation virtual camera source, loaded by the
    Windows Camera Frame Server) → Discord / OBS / Teams / browsers
```
