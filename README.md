# 5W LED-Treiber 22V – ATtiny402 Firmware

Firmware für eine akkubetriebene Lampe mit 5-W-COB-LED (Hongli **HL-LM002H384W-7B2C7**, 7s2p, ~22 V).
Der Boost-Treiber **LM3410XSD** wird vom **ATtiny402** über den DIM-Pin per PWM gedimmt.
Bedienung über einen Taster mit eigener Taster-LED.

---

## Hardware

| Baugruppe | Bauteil | Funktion |
|---|---|---|
| Akku | 1× Li-Ion, 3500 mAh | VCC_Batt (2,8 … 4,2 V) |
| Akkuschutz | DW01A + FS8205A | Über-/Tiefentladung, Überstrom |
| Laden | TP4056 über USB-C | 5 V → Akku, Status-LEDs D5 (grün) / D6 (rot) |
| µC | ATtiny402 | VDD = VCC_Batt direkt |
| LED-Treiber | LM3410XSD (1,6 MHz Boost) | Konstantstrom für COB |
| LED | COB 13,5 × 13,5 mm | VF 20–25 V @ 300 mA, max. 300 mA / 6,7 W, Ts max. 85 °C |

### Bauteilliste (soweit bekannt)

| Ref. | Bauteil | Wert / Typ | Bemerkung |
|---|---|---|---|
| U (µC) | ATtiny402 | SOIC-8 | megaTinyCore, 5 MHz intern |
| U (Treiber) | LM3410XSD | WSON-6, TI | Boost-LED-Treiber, VFB 190 mV, ISW-Limit min. 2,1 A, SW max. 24 V (abs. 26,5 V) |
| U (Laden) | TP4056 | SOP-8 | Li-Ion-Lader 4,2 V, über USB-C |
| U (Schutz) | DW01A + FS8205A | | Akkuschutz (Über-/Tiefentladung, Überstrom) |
| LED | Hongli HL-LM002H384W-7B2C7 | COB 7s2p, 13,5 × 13,5 mm | ~22 V, max. 300 mA |
| Rsense | 0,43 Ω + 0,33 Ω in Serie | = 0,76 Ω | → 250 mA LED-Strom (Schaltplan: 0,1 Ω) |
| R8 | 100 kΩ | | NTC-Teiler nach VCC_Batt |
| NTC | 100 kΩ | B = 3950 **angenommen** | auf GND-Pad neben LM3410 |
| R9 / R10 | – | **nicht bestückt** | U_messen; Akku wird intern gemessen |
| R11 | 100 kΩ | | Pull-down an DIM (LED aus solange µC nicht treibt) |
| R13 | 100 Ω | | Vorwiderstand Taster-LED D4 |
| D4 | LED | | Taster-LED an PA3 |
| D5 / D6 | LED grün / rot | | Ladestatus TP4056 (nicht am µC) |
| L1 | Speicherdrossel | Sättigungsstrom ≥ 2,5 A nötig | |
| C7 / C10 | Ausgangskondensatoren | ≥ 35 V nötig | |
| Akku | Li-Ion 18650 o. ä. | 3500 mAh | nutzbar bis 2,8 V (Bereich 3,0–2,8 V nur Stufe 1, ca. 20 mAh) |

### Pinbelegung ATtiny402

| Pin | Port | Signal | Verwendung |
|---|---|---|---|
| 1 | VDD | VCC_Batt | Versorgung |
| 2 | PA6 | taster | Taster gegen GND, interner Pull-up, Wake-up-Interrupt |
| 3 | PA7 | LED_PWM | → LM3410 DIM (TCA0 WO0, per PORTMUX auf PA7 umgelegt), R11 100k Pull-down |
| 4 | PA1 | U_messen | **nicht bestückt** – Eingang deaktiviert |
| 5 | PA2 | tmp | NTC 100k gegen GND, R8 100k gegen VCC_Batt |
| 6 | PA0 | UPDI | Programmierung |
| 7 | PA3 | taster_led | Taster-LED (aktiv high, D4 + R13 100 Ω) |
| 8 | GND | | |

### LED-Strom

Rsense = 0,43 Ω + 0,33 Ω in Serie = **0,76 Ω** (statt 0,1 Ω laut Schaltplan)

I_LED = V_FB / Rsense = 190 mV / 0,76 Ω ≈ **250 mA** → ca. 5,4 W an der COB (max. 300 mA erlaubt).

### Akkuspannung

R9/R10 (U_messen) sind nicht bestückt. Da VDD direkt am Akku hängt, misst der ATtiny die
**interne 1,1-V-Referenz gegen VDD** als ADC-Referenz:

```
U_Akku = 1,1 V × 1023 / ADC
```

### Temperatur

NTC 100k (angenommen B = 3950) sitzt auf dem **GND-Pad direkt neben dem LM3410**.
Er misst also die Platinentemperatur beim Treiber, **nicht** die COB.
Der Chip selbst ist bei ~0,5 W Verlust etwa 30 K wärmer als die Platine
(80 °C Platine ≈ 110 °C Sperrschicht, max. 125 °C).
Die Umrechnung ADC → °C erfolgt über eine Tabelle (0 … 100 °C in 10-°C-Schritten, linear interpoliert).

---

## Bedienung

| Eingabe | Aus | An (normal) | Strobe |
|---|---|---|---|
| **Doppelklick** | beim 2. Klick sofort ein – immer normal, letzte Stufe | aus | beim 2. Klick sofort aus |
| **3-fach-Klick** | beim 2. Klick ein, beim 3. Klick direkt Strobe | Strobe | aus, 3. Klick wird ignoriert |
| **kurzer Klick** (< 0,4 s) | nichts | heller | zurück auf normal |
| **mittlerer Druck** (0,4 – 2 s) | nichts | dunkler | zurück auf normal |
| **ganz lang** (≥ 2 s) | Akkustand | Akkustand | Akkustand |

- **Beim Einschalten** wird automatisch der Akkustand auf der Taster-LED angezeigt.
- **Die Helligkeitsstufe wird gemerkt** (solange der Akku drin ist – der µC schläft nur).
- Strobe wird nicht gemerkt: Einschalten per Doppelklick startet immer im normalen Licht.
- Im normalen Modus wird das Ausschalten per Doppelklick ca. 350 ms verzögert, weil abgewartet wird,
  ob noch ein 3. Klick (→ Strobe) kommt. Alle anderen Aktionen beim Doppelklick wirken sofort.

### Helligkeitsstufen

| Stufe | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|
| PWM | 3 % | 8 % | 20 % | 45 % | 100 % |

Startstufe beim ersten Einschalten nach Akkuwechsel: Stufe 3.

### Strobe

30 ms an / 50 ms aus (≈ 12,5 Hz), mit der höchsten erlaubten Stufe.

---

## Anzeigen auf der Taster-LED

### Akkustand

Ablauf: **1 s Pause → 1…6 kurze Blitzer (je 120 ms an / 180 ms aus) → 1 s Pause**,
danach wieder Temperaturanzeige. Die Blitzer sind deutlich kürzer an als aus – die Temperaturanzeige blinkt dagegen immer 50:50, so kann man beides auseinanderhalten.

| Blinker | 1 | 2 | 3 | 4 | 5 | 6 |
|---|---|---|---|---|---|---|
| Spannung | < 3,3 V | 3,3 – 3,5 V | 3,5 – 3,65 V | 3,65 – 3,8 V | 3,8 – 4,0 V | ≥ 4,0 V |

Bei eingeschaltetem Licht wird der Akku nur in der **Aus-Phase der PWM** gemessen (~100 µs nach dem
Abschalten der LED). Im PWM-Puls zieht der Booster ~2 A und macht Rippel auf VDD – eine Messung dort
wäre 0,2 – 0,3 V zu niedrig und hat in der ersten Version fälschlich die Drosselung ausgelöst.
Auf Stufe 5 (Dauer-high) gibt es keine Aus-Phase → pauschaler Zuschlag `LOAD_SAG_MV` = 150 mV (geschätzt).

### Temperatur (während das Licht an ist)

| Temperatur | Taster-LED |
|---|---|
| < 40 °C | leuchtet dauerhaft |
| 40 – 49 °C | blinkt langsam (1,6 s Periode) |
| 50 – 59 °C | 0,8 s |
| 60 – 69 °C | 0,4 s |
| ≥ 70 °C | sehr schnell (0,15 s) |

Wenn das Licht aus ist, ist die Taster-LED aus (außer bei Akkuanzeige und Ladeanzeige).

### Laden (im ausgeschalteten Zustand)

Die Status-Pins des TP4056 gehen nur auf D5/D6, **nicht an den ATtiny**. Darum erkennt die Firmware
das Laden über die Akkuspannung. Die RTC (PIT) weckt den ATtiny dazu **jede Sekunde** kurz auf.

| Ereignis | Erkennung |
|---|---|
| Ladegerät angesteckt | Spannung springt innerhalb 1 s um ≥ 30 mV **nach oben** (Ladestrom × Innenwiderstand) |
| Ladegerät abgesteckt | Spannung springt um ≥ 30 mV **nach unten** |
| Akku voll | Spannung ≥ 4,05 V und steigt **20 min lang nicht mehr** (CV-Phase bzw. Ladeende) |

Anzeige auf der Taster-LED:

| Zustand | Muster |
|---|---|
| lädt | **(Akkustand − 1) kurze Blitzer + 1 langer Blinker (0,7 s)**, 2 s Pause, wiederholt sich. Der lange Blinker heißt „lädt gerade“. Beispiel Akkustand 4: · · · ▬ |
| voll | **1 min Dauerlicht**, dann aus |

Einschränkungen:
- Bei vollem Akku fließt kaum noch Strom, das Abstecken ist dann nicht mehr erkennbar.
  Deshalb wird „voll“ nur 1 min angezeigt und danach abgeschaltet.
- Erkannt wird nur das **Anstecken im ausgeschalteten Zustand**. Nach dem Ausschalten wartet die Firmware 20 s, bis sich der Akku erholt hat.
  Wird die Lampe während des Ladens eingeschaltet, endet die Ladeanzeige. Nach dem Ausschalten erkennt sie das Laden erst beim nächsten Anstecken wieder.
- Während der Ladeanzeige schläft der ATtiny nicht (≈ 1–2 mA, kommt ja vom Ladegerät).
  Spätestens nach 10 h endet die Anzeige auf jeden Fall.
- Der Akkustand während des Ladens ist etwas zu hoch, weil die Ladespannung mitgemessen wird.

---

## Schutzfunktionen

| Bedingung | Reaktion |
|---|---|
| Akku < **3,3 V** für 2 s | Drosselung auf max. **Stufe 4** + **Haupt-LED Doppelblink** |
| Akku < **3,15 V** für 2 s | Drosselung auf max. **Stufe 3** + **Haupt-LED Doppelblink** |
| Akku < **3,0 V** für 2 s | Drosselung auf max. **Stufe 1** + **Haupt-LED Doppelblink** |
| Drosselung aktiv und Einschalten | Haupt-LED Doppelblink als Erinnerung |
| Akku ≥ **3,1 V** / **3,25 V** / **3,4 V** im ausgeschalteten Zustand (ohne Last) | jeweils ein Drosselschritt fällt weg (ab 3,4 V ganz aufgehoben) |
| Hochschalten über die erlaubte Stufe (Akku-Drosselung) | Haupt-LED Doppelblink, Stufe bleibt |
| Akku < **2,8 V** für 2 s | Licht aus |
| Platine ≥ **70 °C** | max. Stufe 3 (Freigabe unter 65 °C) |
| Platine ≥ **80 °C** | max. Stufe 1 (Freigabe unter 75 °C → Stufe 3) |
| **15 min** ununterbrochen auf Stufe 5 | automatisch 1 Stufe runter, manuell wieder hochschaltbar (Timer startet neu) |

**Warum die Drosselung erst bei 3,4 V und nur im Aus-Zustand aufgehoben wird:**
Unter Last (~1,9 A) bricht die Spannung ein. Wird gedrosselt, erholt sie sich um 0,1 – 0,2 V.
Ohne Hysterese würde die Lampe ständig zwischen gedrosselt und voll hin- und herspringen.

Einschalten ist immer möglich – die Schwellen betreffen nur die Drosselung.

---

## Stromsparen

Im ausgeschalteten Zustand geht der ATtiny in **Power-Down-Sleep**. ADC, Referenz und Timer sind aus,
Aufwachen per Pin-Interrupt am Taster (PA6). Der LM3410 ist über DIM = low (R11 Pull-down) im Shutdown (~80 nA).

Zusätzlich läuft die RTC (PIT, interner 1-kHz-Oszillator) und weckt jede Sekunde für ca. 2 ms zur Ladeerkennung (im Mittel wenige µA).

Ruhestrom ca. 25–30 µA, hauptsächlich der NTC-Teiler (R8 + NTC ≈ 20 µA) und der DW01A.

---

## Software-Aufbau

- **Takt:** 5 MHz (sicher über den gesamten Akkubereich)
- **Zeitbasis:** TCB0, 1-ms-Interrupt (Core-`millis()` ist abgeschaltet)
- **PWM:** TCA0 Single-Slope, 5 MHz / 8 / 1000 = **625 Hz** (Datenblatt LM3410 empfiehlt 200 Hz – 1 kHz)
- **ADC:** 10 bit, 16-fach aufsummiert, Referenz VDD; alle 125 ms abwechselnd Akku und NTC, gefiltert
- **Taster:** 25 ms Entprellung, Klick-Pakete mit 350 ms Pause; Aktionen teils sofort beim Klick
  (`onClickImmediate`), teils nach Paket-Ende (`onClicksDone`)
- Nicht blockierend: `loop()` ruft `buttonUpdate()`, `monitorUpdate()`, `outputUpdate()` und 1× pro Sekunde (RTC-PIT) `chargeTick()` auf

### Einstellbare Werte (oben in `src/main.cpp`)

| Konstante | Wert | Bedeutung |
|---|---|---|
| `LEVELS[]` | 30, 80, 200, 450, 1000 | Helligkeitsstufen in Promille |
| `START_LEVEL` | 2 | Startstufe (Index 0..4) |
| `CLICK_MAX_MS` | 400 | max. Dauer eines Klicks |
| `LONG_MS` | 2000 | ab hier Akkustand |
| `MULTICLICK_GAP_MS` | 350 | Pause zwischen Klicks |
| `FULL_TIMEOUT_S` | 900 | Vollgas-Timer (15 min) |
| `STROBE_ON_MS` / `STROBE_OFF_MS` | 30 / 50 | Strobe-Timing |
| `VREF_MV` | 1100 | interne Referenz – **zum Kalibrieren** |
| `LOWBAT_MV[]` / `LOWBAT_MAX[]` | 3300, 3150, 3000 / 3, 2, 0 | Drosselschritte: Schwelle → max. Stufe (Index → Stufe 4, 3, 1) |
| `LOWBAT_RESET_MV[]` | 3400, 3250, 3100 | Drosselschritt fällt im Aus-Zustand wieder weg |
| `CRITBAT_MV` | 2800 | Abschaltung |
| `BATT_STEPS_MV[]` | 3300 … 4000 | Schwellen der Akkuanzeige |
| `NTC_TAB[]` | | ADC-Werte für 0 … 100 °C (B = 3950) |
| `THERM_WARN_C` / `THERM_HOT_C` | 70 / 80 | thermische Drosselung |
| `TEMP_BLINK_PERIOD[]` | 1600, 800, 400, 150 | Blinkperioden Temperaturanzeige |
| `CHG_STEP_MV` | 30 | Spannungssprung = Ladegerät an/ab |
| `CHG_FULL_MIN_MV` / `CHG_FLAT_S` | 4050 / 1200 | „voll“: ab 4,05 V und 20 min kein Anstieg |
| `CHG_FULL_SHOW_S` | 60 | Dauer der „voll“-Anzeige |

### Akkumessung kalibrieren

Akkuspannung mit Multimeter messen, Anzeige vergleichen und anpassen:

```
VREF_MV = 1100 × U_echt / U_angezeigt
```

---

## Bauen und Flashen

PlatformIO, Plattform `atmelmegaavr`, Core **megaTinyCore**.

```ini
[env:ATtiny402]
platform = atmelmegaavr
board = ATtiny402
framework = arduino
board_build.f_cpu = 5000000L
build_flags = -DMILLIS_USE_TIMERNONE

upload_protocol = serialupdi
upload_port = COM7
upload_speed = 230400
```

Programmiert wird über **UPDI** (Pin 6 / PA0, Stecker „Prog“) mit einem SerialUPDI-Adapter
(CP210x USB-UART, TX/RX über Diode/Widerstand auf UPDI). Upload: `pio run -t upload`.
Falls der Adapter eine andere COM-Nummer bekommt, `upload_port` anpassen.

Speicherbedarf: ca. 3,3 kB von 4 kB Flash, 58 Byte RAM.

---

## Offene Punkte / Hinweise zur Hardware

1. **Überspannungsschutz fehlt:** Löst sich die COB, steigt die Ausgangsspannung bis zur Zerstörung des
   LM3410 (SW max. 26,5 V). Abhilfe: Z-Diode + Widerstand zum FB-Pin (Datenblatt Figure 18).
   Knapp, weil COB VF 20 – 25 V und LM3410 nur bis 24 V spezifiziert → echte VF der COB bei 250 mA messen (auch kalt).
2. **Ausgangskondensatoren C7/C10** müssen mindestens **35 V** Spannungsfestigkeit haben
   (Keramik verliert bei DC-Bias viel Kapazität).
3. **Spule L1:** Sättigungsstrom ≥ 2,5 A (Spitzenstrom bei leerem Akku > 2 A).
4. **Eingangsstrom:** gemessen ca. **2,3 A bei 3,5 V auf Stufe 5** (inkl. Verlust am Amperemeter) → Wirkungsgrad ca. 70 %. Spitzenstrom im Schalter ca. 2,5–2,8 A, Stromgrenze LM3410 min. 2,1 / typ. 2,7 A → an der Grenze; bei niedriger Akkuspannung kann die LED auf Stufe 5 etwas dunkler werden (harmlos). Prüfen: Spannung über Rsense soll 190 mV sein.
5. **COB-Temperatur wird nicht gemessen:** ausreichende Kühlung nötig (Ts max. 85 °C).
6. **NTC-B-Wert** (3950) ist angenommen – bei anderem NTC `NTC_TAB[]` neu berechnen:
   `R = 100k · exp(B · (1/T − 1/298,15))`, `ADC = 1023 · R / (R + 100k)`.
7. **Unterste Stufe (3 %)**: falls der LM3410 bei so kurzen DIM-Pulsen flackert, in `LEVELS[]` anheben.
8. **Teilweise getestet:** Flashen per SerialUPDI und Einschalten funktionieren. Die erste Version hat
   unter Last fälschlich „Akku schwach“ erkannt (siehe Akkustand) – die korrigierte Messung ist noch zu prüfen.

---

## Versionen

| Tag | Inhalt |
|---|---|
| `v1.0` | Grundfunktion: Bedienung, Stufen, Strobe, Akku-/Temperaturanzeige, Schutzfunktionen, Akkumessung in der PWM-Aus-Phase |
| `v1.1` | Schnellere Akkuanzeige (120 ms an / 180 ms aus, 1 s Pause davor/danach) |
| `v1.2` | Ladeerkennung über die Akkuspannung, Ladeanzeige auf der Taster-LED im Aus-Zustand |
| `v1.3` | Akku-Drosselung in 2 Schritten (3,3 V → max. Stufe 4, 3,15 V → max. Stufe 3), Doppelblink beim Hochschalten über die erlaubte Stufe |
| `v1.4` | 3. Drosselschritt: unter 3,0 V nur noch Stufe 1, Abschaltung erst bei 2,8 V |

Zurück auf eine Version: `git checkout v1.0` (danach `pio run -t upload`), zurück zum neuesten Stand: `git checkout main`.
