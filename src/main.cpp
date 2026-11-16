/*
 * 5W LED-Treiber 22V  -  ATtiny402 Firmware
 *
 * Hardware (laut Schaltplan):
 *   PA6 (Pin 2) = Taster gegen GND (interner Pull-up)
 *   PA7 (Pin 3) = LED_PWM -> LM3410 DIM (TCA0 WO0, alternativer Pin)
 *   PA1 (Pin 4) = U_messen  -> NICHT bestueckt, Akku wird intern gemessen (VDD = VCC_Batt)
 *   PA2 (Pin 5) = tmp       -> NTC 100k gegen GND, R8 100k gegen VCC_Batt
 *   PA3 (Pin 7) = taster_led (aktiv high, D4 + R13)
 *   PA0 (Pin 6) = UPDI
 *
 *   LED-Strom: Rsense = 0.43 + 0.33 = 0.76 Ohm -> I = 190 mV / 0.76 = 250 mA
 *              (COB max. 300 mA -> passt)
 *
 * Bedienung:
 *   AUS:
 *     Doppelklick        : beim 2. Klick sofort ein, immer normal, mit letzter Stufe
 *     Dreifachklick      : beim 2. Klick ein, beim 3. Klick direkt Strobe
 *   AN (Dauerlicht):
 *     Doppelklick        : aus
 *     Dreifachklick      : Strobe
 *     kurzer Klick       : heller   (5 Stufen)
 *     mittellanger Druck : dunkler  (0.4 s .. 2 s)
 *     15 min auf voller Stufe -> automatisch 1 Stufe runter (manuell wieder hoch moeglich)
 *   STROBE:
 *     Doppelklick        : beim 2. Klick sofort aus, ein 3. Klick wird ignoriert
 *     Klick / mittellanger Druck : zurueck auf Dauerlicht
 *   immer:
 *     ganz langer Druck  : Akkustand auf der Taster-LED (>= 2 s, 1..6 Blinker, 6 = voll)
 *   Beim Einschalten wird der Akkustand automatisch auf der Taster-LED angezeigt.
 *
 * Schutz:
 *   Akku < 3.3 V (2 s lang)  -> Helligkeit drosseln + Haupt-LED Doppelblink
 *   Akku < 3.0 V (2 s lang)  -> Licht aus
 *   Temperatur               -> Taster-LED blinkt umso schneller je heisser,
 *                               ab 70 C / 80 C wird zusaetzlich gedrosselt
 *
 * Wenn aus: Power-Down-Sleep, Aufwachen per Pin-Interrupt am Taster.
 */

#include <Arduino.h>
#include <avr/sleep.h>
#include <util/atomic.h>

// ============================================================================
//  Konfiguration
// ============================================================================

// ---- Pins (alle auf PORTA) ----
#define BTN_bm   PIN6_bm
#define PWM_bm   PIN7_bm
#define TLED_bm  PIN3_bm

// ---- PWM fuer DIM: 5 MHz / 8 / (999+1) = 625 Hz (Datenblatt: 200 Hz..1 kHz ideal) ----
static const uint16_t PWM_TOP = 999;

// ---- Helligkeitsstufen in Promille (5 Stufen, grob logarithmisch) ----
static const uint8_t  NUM_LEVELS  = 5;
static const uint16_t LEVELS[NUM_LEVELS] = { 30, 80, 200, 450, 1000 };
static const uint8_t  START_LEVEL = 2;          // Stufe beim ersten Einschalten (0..4)

// ---- Taster-Timing [ms] ----
static const uint16_t DEBOUNCE_MS       = 25;
static const uint16_t CLICK_MAX_MS      = 400;   // kuerzer = Klick
static const uint16_t LONG_MS           = 2000;  // ab hier = Akkustand; dazwischen = dunkler
static const uint16_t MULTICLICK_GAP_MS = 350;   // Pause, nach der ein Klick-Paket ausgewertet wird

// ---- Sicherheit: nach so vielen Sekunden auf voller Stufe 1 Stufe runter ----
static const uint16_t FULL_TIMEOUT_S = 15 * 60;

// ---- Strobe [ms] ----
static const uint16_t STROBE_ON_MS  = 30;
static const uint16_t STROBE_OFF_MS = 50;

// ---- Akku [mV] ----
// Interne Referenz 1.1 V (Toleranz ein paar %). Wenn der angezeigte Wert nicht
// stimmt: VREF_MV = 1100 * U_echt / U_gemessen anpassen.
static const uint32_t VREF_MV          = 1100;
// Drosselung in Schritten (jeder Schritt mit Doppelblink der Haupt-LED).
// Schritt i wird aktiv, wenn der Akku 2 s unter LOWBAT_MV[i] liegt -> max. Stufe LOWBAT_MAX[i].
// Drossel aufheben: nur im Aus-Zustand (ohne Last), ab LOWBAT_RESET_MV[i] faellt
// Schritt i wieder weg. Ohne diese Hysterese wuerde die Lampe dauernd zwischen
// gedrosselt und voll hin und her springen (Akku erholt sich beim Drosseln).
static const uint8_t  LOWBAT_STEPS = 3;
static const uint16_t LOWBAT_MV[LOWBAT_STEPS]       = { 3300, 3150, 3000 };
static const uint8_t  LOWBAT_MAX[LOWBAT_STEPS]      = {    3,    2,    0 };  // Index -> Stufe 4, 3, 1
static const uint16_t LOWBAT_RESET_MV[LOWBAT_STEPS] = { 3400, 3250, 3100 };
static const uint16_t CRITBAT_MV        = 2800;  // abschalten (Zelle bis 2,8 V nutzbar)
static const uint16_t BAT_FILTER_MS     = 2000;  // so lange muss die Schwelle unterschritten sein

// Akkuanzeige: Anzahl Blinker = 1 + Anzahl ueberschrittener Schwellen (1..6)
// Bereich 3.0 V (leer) .. 4.2 V (voll):
//   1: < 3.3 V | 2: 3.3-3.5 | 3: 3.5-3.65 | 4: 3.65-3.8 | 5: 3.8-4.0 | 6: >= 4.0 V
static const uint16_t BATT_STEPS_MV[5] = { 3300, 3500, 3650, 3800, 4000 };

// ---- Temperatur ----
// NTC 100k, B = 3950 (ANNAHME!), R8 = 100k nach VCC.  ADC-Wert (10 bit) bei 0,10,...,100 C
// berechnet mit: R = 100k*exp(B*(1/T - 1/298.15)), ADC = 1023 * R/(R+100k)
static const uint16_t NTC_TAB[11] = { 788, 684, 569, 456, 354, 270, 204, 153, 115, 87, 67 };

// Thermische Drosselung (mit Hysterese)
// Der NTC sitzt auf dem GND-Pad direkt neben dem LM3410, misst also die Platine beim
// Treiber. Der Chip selbst ist bei ~0.5 W Verlust und ~60 K/W rund 30 K waermer
// -> bei 80 C Platine ca. 110 C Sperrschicht (max. 125 C). Die COB wird NICHT gemessen.
static const int8_t THERM_WARN_C  = 70;  // ab hier max. Stufe 3 (Index 2)
static const int8_t THERM_HOT_C   = 80;  // ab hier max. Stufe 1 (Index 0)
static const int8_t THERM_HYST_C  = 5;

// Taster-LED Blinkperiode je nach Temperatur
//   < 40 C: Dauerlicht, 40..49: langsam, 50..59, 60..69, >= 70: sehr schnell
static const uint16_t TEMP_BLINK_PERIOD[4] = { 1600, 800, 400, 150 };

// ---- Ladeerkennung (nur im Aus-Zustand) ----
// Die Status-Pins des TP4056 gehen nur auf D5/D6, nicht an den uC. Deshalb Erkennung
// ueber die Akkuspannung, gemessen 1x pro Sekunde (RTC-PIT weckt aus dem Sleep):
//  - Ladegeraet an:  Spannung springt um I_Lade * R_innen nach oben (typ. 50..100 mV)
//  - Ladegeraet ab:  Spannung springt um denselben Betrag nach unten
//  - voll:           Spannung steigt lange nicht mehr (CV-Phase / Ladeende)
// Alles relativ ausgewertet -> unabhaengig von der Toleranz der internen Referenz.
static const uint16_t CHG_STEP_MV     = 30;        // Sprung von Sekunde zu Sekunde
static const uint16_t CHG_SETTLE_S    = 20;        // nach dem Ausschalten erholt sich der Akku -> abwarten
static const uint16_t CHG_FULL_MIN_MV = 4050;      // "voll" erst ab dieser Spannung moeglich
static const uint16_t CHG_FLAT_MV     = 5;         // Anstieg kleiner als das gilt als "steigt nicht mehr"
static const uint16_t CHG_FLAT_S      = 20 * 60;   // so lange kein Anstieg -> voll
static const uint16_t CHG_FULL_SHOW_S = 60;        // "voll" so lange anzeigen, dann Anzeige aus
static const uint16_t CHG_MAX_S       = 10 * 3600; // Sicherheit: spaetestens dann Anzeige beenden
static const uint16_t CHG_LONG_MS     = 700;       // langer Blinker = "laedt gerade"
static const uint16_t CHG_GAP_MS      = 2000;      // Pause zwischen den Gruppen

// ============================================================================
//  Zeitbasis: TCB0, 1 ms Interrupt
// ============================================================================
static volatile uint16_t g_ms = 0;

ISR(TCB0_INT_vect) {
    TCB0.INTFLAGS = TCB_CAPT_bm;
    g_ms++;
}

static inline uint16_t now_ms() {
    uint16_t t;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { t = g_ms; }
    return t;
}

static inline uint16_t since(uint16_t t0, uint16_t now) { return (uint16_t)(now - t0); }

// RTC-PIT: 1-s-Takt, laeuft auch im Power-Down (Ladeerkennung)
static volatile bool g_pitTick = false;

ISR(RTC_PIT_vect) {
    RTC.PITINTFLAGS = RTC_PI_bm;
    g_pitTick = true;
}

// Pin-Change am Taster: nur zum Aufwachen
ISR(PORTA_PORT_vect) {
    PORTA.INTFLAGS = PORTA.INTFLAGS;
}

// ============================================================================
//  Hardware-Init
// ============================================================================
static void initTimers() {
    // 1-ms-Tick
    TCB0.CTRLA   = 0;
    TCB0.CCMP    = (F_CPU / 1000UL) - 1;
    TCB0.CTRLB   = TCB_CNTMODE_INT_gc;
    TCB0.INTCTRL = TCB_CAPT_bm;
    TCB0.CTRLA   = TCB_CLKSEL_CLKDIV1_gc | TCB_ENABLE_bm;

    // TCA0: Core-Konfiguration (Split-Mode) verwerfen, Single-Slope-PWM auf WO0 -> PA7
    TCA0.SINGLE.CTRLA   = 0;
    TCA0.SINGLE.CTRLESET = TCA_SINGLE_CMD_RESET_gc;
    TCA0.SINGLE.CTRLD   = 0;                       // kein Split-Mode
    PORTMUX.CTRLC       = PORTMUX_TCA00_bm;        // WO0 auf alternativen Pin PA7
    TCA0.SINGLE.PER     = PWM_TOP;
    TCA0.SINGLE.CMP0    = 0;
    TCA0.SINGLE.CTRLB   = TCA_SINGLE_WGMODE_SINGLESLOPE_gc;   // CMP0EN erst bei duty > 0
    TCA0.SINGLE.CTRLA   = TCA_SINGLE_CLKSEL_DIV8_gc | TCA_SINGLE_ENABLE_bm;

    // RTC-PIT: interner 1,024-kHz-Takt (OSCULP32K), Interrupt jede Sekunde
    while (RTC.STATUS > 0) {}
    RTC.CLKSEL = RTC_CLKSEL_INT1K_gc;
    while (RTC.PITSTATUS > 0) {}
    RTC.PITINTCTRL = RTC_PI_bm;
    RTC.PITCTRLA   = RTC_PERIOD_CYC1024_gc | RTC_PITEN_bm;
}

static void initAdc() {
    VREF.CTRLA   = VREF_ADC0REFSEL_1V1_gc;
    VREF.CTRLB   = VREF_ADC0REFEN_bm;
    ADC0.CTRLA   = 0;
    ADC0.CTRLB   = ADC_SAMPNUM_ACC16_gc;                                  // 16 Samples aufsummieren
    ADC0.CTRLC   = ADC_SAMPCAP_bm | ADC_REFSEL_VDDREF_gc | ADC_PRESC_DIV8_gc; // 625 kHz
    ADC0.CTRLD   = ADC_INITDLY_DLY32_gc;
    ADC0.SAMPCTRL = 10;                                                   // NTC-Teiler ist hochohmig
    ADC0.CTRLA   = ADC_ENABLE_bm;
}

static void initPins() {
    PORTA.DIRSET = PWM_bm | TLED_bm;
    PORTA.OUTCLR = PWM_bm | TLED_bm;
    PORTA.PIN6CTRL = PORT_PULLUPEN_bm | PORT_ISC_BOTHEDGES_gc;   // Taster, Wake-up
    PORTA.PIN2CTRL = PORT_ISC_INPUT_DISABLE_gc;                  // NTC (analog)
    PORTA.PIN1CTRL = PORT_ISC_INPUT_DISABLE_gc;                  // U_messen, unbestueckt
}

// ============================================================================
//  PWM / Ausgaenge
// ============================================================================
static uint16_t currentDuty = 0xFFFF;

static void setDuty(uint16_t permille) {
    if (permille == currentDuty) return;
    currentDuty = permille;
    if (permille == 0) {
        TCA0.SINGLE.CTRLB &= ~TCA_SINGLE_CMP0EN_bm;   // Pin wieder vom PORT gesteuert -> low
        PORTA.OUTCLR = PWM_bm;
    } else {
        // permille >= 1000 -> CMP0 > PER -> dauerhaft high
        TCA0.SINGLE.CMP0BUF = permille;
        TCA0.SINGLE.CTRLB |= TCA_SINGLE_CMP0EN_bm;
    }
}

static inline void setTasterLed(bool on) {
    if (on) PORTA.OUTSET = TLED_bm; else PORTA.OUTCLR = TLED_bm;
}

// ============================================================================
//  Messungen
// ============================================================================
static uint16_t adcConvert() {
    ADC0.INTFLAGS = ADC_RESRDY_bm;
    ADC0.COMMAND  = ADC_STCONV_bm;
    while (!(ADC0.INTFLAGS & ADC_RESRDY_bm)) {}
    return ADC0.RES;
}

static uint16_t adcRead(uint8_t muxpos) {
    ADC0.MUXPOS = muxpos;
    adcConvert();                               // erste Wandlung nach Mux-Wechsel verwerfen
    return adcConvert();                        // Summe von 16 Samples
}

// VDD = Vref * 1023 / ADC  (gemessen wird die interne 1.1 V gegen VDD als Referenz)
//
// Waehrend die LED-PWM laeuft, zieht der Booster im PWM-Puls ~2 A aus dem Akku und
// macht Rippel auf VDD -> Messung im Puls ist zu niedrig. Deshalb wird in der
// Aus-Phase der PWM gemessen (kurz nach dem Abschalten, Akku hat sich erholt).
// Auf voller Stufe (Dauer-high) gibt es keine Aus-Phase -> Lasteinbruch pauschal ausgleichen.
static const uint16_t VBAT_SETTLE_TICKS = 60;    // ~100 us nach LED-aus warten (1 Tick = 1.6 us)
static const uint16_t VBAT_WIN_TICKS    = 40;    // Startfenster fuer die Messung
static const uint16_t VBAT_CONV_TICKS   = 120;   // ~190 us fuer 4 Samples
static const uint16_t LOAD_SAG_MV       = 150;   // Zuschlag bei Messung unter Volllast

static uint16_t readVbatMv() {
    ADC0.MUXPOS = ADC_MUXPOS_INTREF_gc;
    adcConvert();                               // verwerfen (Mux/Referenz einschwingen)

    uint16_t acc;
    bool     sag   = false;
    bool     pwmOn = TCA0.SINGLE.CTRLB & TCA_SINGLE_CMP0EN_bm;
    uint16_t cmp   = TCA0.SINGLE.CMP0;

    if (pwmOn && cmp + VBAT_SETTLE_TICKS + VBAT_WIN_TICKS + VBAT_CONV_TICKS <= PWM_TOP) {
        uint16_t lo = cmp + VBAT_SETTLE_TICKS;
        uint16_t hi = lo + VBAT_WIN_TICKS;
        uint16_t c;
        ADC0.CTRLB = ADC_SAMPNUM_ACC4_gc;       // kurz genug fuer die Aus-Phase
        do { c = TCA0.SINGLE.CNT; } while (c < lo || c >= hi);
        acc = adcConvert() * 4;                 // auf 16er-Summe skalieren
        ADC0.CTRLB = ADC_SAMPNUM_ACC16_gc;
    } else {
        acc = adcConvert();
        sag = pwmOn;                            // Dauer-high -> unter Last gemessen
    }

    if (acc == 0) return 0xFFFF;
    uint16_t mv = (uint16_t)((VREF_MV * 1023UL * 16UL) / acc);
    if (sag) mv += LOAD_SAG_MV;
    return mv;
}

static int8_t ntcToCelsius(uint16_t adc) {
    if (adc >= NTC_TAB[0]) return 0;            // <= 0 C (oder NTC offen)
    for (uint8_t i = 1; i < 11; i++) {
        if (adc >= NTC_TAB[i]) {
            uint16_t hi = NTC_TAB[i - 1], lo = NTC_TAB[i];
            return (int8_t)((i - 1) * 10 + ((hi - adc) * 10) / (hi - lo));
        }
    }
    return 100;                                 // >= 100 C (oder NTC Kurzschluss)
}

static int8_t readTempC() {
    return ntcToCelsius(adcRead(ADC_MUXPOS_AIN2_gc) / 16);
}

// ============================================================================
//  Zustand
// ============================================================================
enum Mode : uint8_t { MODE_OFF, MODE_ON, MODE_STROBE };

static Mode     mode      = MODE_OFF;
static uint8_t  level     = START_LEVEL;
static uint16_t strobeT0  = 0;

static uint16_t vbatMv    = 4000;   // gefiltert
static int8_t   tempC     = 25;     // gefiltert
static uint8_t  lowStage  = 0;      // Akku-Drossel: 0 = keine, 1..3 = Schritt (max. Stufe 4 / 3 / 1)
static uint8_t  thermMax  = NUM_LEVELS - 1;
static uint16_t lowSince, critSince;
static uint16_t lastMeasT = 0;
static uint16_t secT;               // 1-s-Raster fuer den Vollgas-Timer
static uint16_t fullSec = 0;        // Sekunden ununterbrochen auf voller Stufe
static bool     measToggle = false;

// Doppelblink der Haupt-LED
static bool     blinkActive = false;
static uint16_t blinkT0;

// Akkuanzeige auf Taster-LED
static bool     battShowActive = false;
static uint16_t battShowT0;
static uint8_t  battShowCount;

// Kurze, knackige Blitzer (an deutlich kuerzer als aus) -> unterscheidbar von der
// Temperaturanzeige, die immer 50:50 blinkt. Die Dunkelpause davor/danach trennt zusaetzlich.
static const uint16_t BATT_PAUSE_MS = 1000;  // Pause davor und danach
static const uint16_t BATT_ON_MS    = 120;
static const uint16_t BATT_OFF_MS   = 180;

// Ladeerkennung
enum ChgState : uint8_t { CHG_NONE, CHG_ACTIVE, CHG_FULL };
static ChgState chgState  = CHG_NONE;
static uint16_t chgSettleS = 0;     // Sekunden im Aus-Zustand (Erholung abwarten)
static uint16_t chgRefMv;           // Messwert der letzten Sekunde
static uint16_t chgMaxMv;           // hoechster Wert waehrend der Ladung
static uint16_t chgS, chgFlatS;     // Ladedauer / Sekunden ohne Anstieg
static uint8_t  chgLevel;           // Akkustand 1..6 fuer die Anzeige
static uint16_t chgT0;              // Start der aktuellen Blink-Gruppe

// Nur im Aus-Zustand (Messung ohne Last): Drossel schrittweise aufheben
static void releaseLowStage() {
    while (lowStage && vbatMv >= LOWBAT_RESET_MV[lowStage - 1]) lowStage--;
}

static uint8_t maxLevel() {
    uint8_t m = thermMax;
    if (lowStage && LOWBAT_MAX[lowStage - 1] < m) m = LOWBAT_MAX[lowStage - 1];
    return m;
}

static void startDoubleBlink(uint16_t now) {
    blinkActive = true;
    blinkT0 = now;
}

static void setMode(Mode m, uint16_t now) {
    mode = m;
    blinkActive = false;
    strobeT0 = now;
    lowSince = critSince = now;
    secT = now;
    fullSec = 0;
    chgState = CHG_NONE;            // Ladeanzeige nur im Aus-Zustand
    chgSettleS = 0;                 // nach jedem Moduswechsel neu einschwingen lassen
    if (m != MODE_OFF && lowStage) startDoubleBlink(now);  // Erinnerung: Akku schwach
}

static uint8_t battLevel(uint16_t mv) {
    uint8_t n = 1;
    for (uint8_t i = 0; i < 5; i++)
        if (mv >= BATT_STEPS_MV[i]) n++;
    return n;
}

static void startBatteryDisplay(uint16_t now) {
    battShowCount  = battLevel(vbatMv);
    battShowT0     = now;
    battShowActive = true;
}

// ============================================================================
//  Taster-Aktionen
// ============================================================================
static void turnOn(Mode m, uint16_t now) {
    setMode(m, now);
    startBatteryDisplay(now);       // Akkustand auf der Taster-LED (Messung noch ohne Last)
}

// Modus beim ersten Klick eines Klick-Pakets
static Mode seqMode;

// Wird SOFORT bei jedem Klick aufgerufen (n = wievielter Klick im Paket)
static void onClickImmediate(uint8_t n, uint16_t now) {
    if (n == 1) seqMode = mode;

    switch (seqMode) {
    case MODE_OFF:
        if (n == 2)      turnOn(MODE_ON, now);      // 2. Klick: sofort ein, normal, letzte Stufe
        else if (n == 3) setMode(MODE_STROBE, now); // 3. Klick gleich hinterher: Strobe
        break;                                      // 1. und ab 4. Klick: nichts
    case MODE_STROBE:
        if (n == 2)      setMode(MODE_OFF, now);    // 2. Klick: sofort aus, 3. wird ignoriert
        break;
    case MODE_ON:
        if (n == 3)      setMode(MODE_STROBE, now); // 3-fach: Strobe
        break;
    }
}

// Wird aufgerufen, wenn das Klick-Paket fertig ist (Pause nach dem letzten Klick)
static void onClicksDone(uint8_t n, uint16_t now) {
    if (seqMode == MODE_ON) {
        if (n == 1) {
            if (level < maxLevel())  level++;       // heller
            else if (lowStage)       startDoubleBlink(now);  // gesperrt wegen Akku -> Doppelblink
        } else if (n == 2) {
            setMode(MODE_OFF, now);                 // aus (muss auf moeglichen 3. Klick warten)
        }
    } else if (seqMode == MODE_STROBE && n == 1) {
        setMode(MODE_ON, now);                      // Strobe -> normal
    }
}

static void onMediumPress(uint16_t now) {
    if (mode == MODE_ON) {
        if (level > maxLevel()) level = maxLevel();
        if (level > 0) level--;
    } else if (mode == MODE_STROBE) {
        setMode(MODE_ON, now);
    }
}

static void onLongPress(uint16_t now) {
    startBatteryDisplay(now);
}

// ============================================================================
//  Taster-Auswertung
// ============================================================================
static bool     btnRaw = false, btnStable = false, longFired = false;
static uint16_t btnChangeT, pressT0, releaseT;
static uint8_t  clicks = 0;

static void buttonUpdate(uint16_t now) {
    bool raw = !(PORTA.IN & BTN_bm);   // gedrueckt = low
    if (raw != btnRaw) { btnRaw = raw; btnChangeT = now; }

    if (raw != btnStable && since(btnChangeT, now) >= DEBOUNCE_MS) {
        btnStable = raw;
        if (btnStable) {                          // gedrueckt
            pressT0   = now;
            longFired = false;
        } else {                                  // losgelassen
            uint16_t dur = since(pressT0, now);
            if (longFired) {
                // langer Druck wurde schon ausgefuehrt
            } else if (dur < CLICK_MAX_MS) {
                if (clicks < 255) clicks++;
                releaseT = now;
                onClickImmediate(clicks, now);
            } else {
                clicks = 0;
                onMediumPress(now);
            }
        }
    }

    if (btnStable && !longFired && since(pressT0, now) >= LONG_MS) {
        longFired = true;
        clicks = 0;
        onLongPress(now);
    }

    if (!btnStable && clicks && since(releaseT, now) >= MULTICLICK_GAP_MS) {
        uint8_t n = clicks;
        clicks = 0;
        onClicksDone(n, now);
    }
}

// ============================================================================
//  Ueberwachung Akku / Temperatur
// ============================================================================
static void updateThermalLimit() {
    if (tempC >= THERM_HOT_C) {
        thermMax = 0;
    } else if (tempC >= THERM_WARN_C) {
        if (thermMax > 2) thermMax = 2;
        if (thermMax == 0 && tempC < THERM_HOT_C - THERM_HYST_C) thermMax = 2;
    } else {
        if (tempC < THERM_WARN_C - THERM_HYST_C) thermMax = NUM_LEVELS - 1;
        else if (thermMax == 0) thermMax = 2;
    }
}

static void monitorUpdate(uint16_t now) {
    // alle 125 ms abwechselnd Akku und Temperatur messen
    if (since(lastMeasT, now) >= 125) {
        lastMeasT = now;
        measToggle = !measToggle;
        if (measToggle) {
            uint16_t v = readVbatMv();
            vbatMv = (uint16_t)(((uint32_t)vbatMv * 3 + v) / 4);
        } else {
            int8_t t = readTempC();
            tempC = (int8_t)(((int16_t)tempC * 3 + t + 2) / 4);
            updateThermalLimit();
        }
    }

    // Vollgas-Timer: nur Dauerlicht und nur wenn wirklich die volle Stufe leuchtet
    if (since(secT, now) >= 1000) {
        secT += 1000;
        if (mode == MODE_ON && level >= NUM_LEVELS - 1 && maxLevel() >= NUM_LEVELS - 1) {
            if (++fullSec >= FULL_TIMEOUT_S) {
                level = NUM_LEVELS - 2;     // 1 Stufe runter, manuell wieder hoch moeglich
                fullSec = 0;
            }
        } else {
            fullSec = 0;
        }
    }

    if (mode != MODE_OFF) {
        // Akku schwach -> naechsten Drossel-Schritt + Doppelblink
        if (lowStage < LOWBAT_STEPS && vbatMv < LOWBAT_MV[lowStage]) {
            if (since(lowSince, now) >= BAT_FILTER_MS) {
                lowStage++;
                lowSince = now;
                startDoubleBlink(now);
            }
        } else {
            lowSince = now;
        }
        // Akku leer -> aus
        if (vbatMv < CRITBAT_MV) {
            if (since(critSince, now) >= BAT_FILTER_MS) {
                setMode(MODE_OFF, now);
            }
        } else {
            critSince = now;
        }
    } else {
        lowSince = critSince = now;
        releaseLowStage();
    }
}

// ============================================================================
//  Ladeerkennung: 1x pro Sekunde im Aus-Zustand aufrufen
// ============================================================================
static void chargeTick(uint16_t now) {
    if (mode != MODE_OFF) return;
    uint16_t v = readVbatMv();                  // ungefiltert, PWM ist aus -> ohne Last

    switch (chgState) {
    case CHG_NONE:
        if (chgSettleS < CHG_SETTLE_S) {
            chgSettleS++;                       // Akku erholt sich noch vom Betrieb
        } else if (v >= chgRefMv + CHG_STEP_MV) {
            chgState = CHG_ACTIVE;              // Sprung nach oben -> Ladegeraet angesteckt
            chgS = chgFlatS = 0;
            chgMaxMv = v;
            chgT0 = now;
        }
        break;

    case CHG_ACTIVE:
        if (v + CHG_STEP_MV <= chgRefMv) {      // Sprung nach unten -> abgesteckt
            chgState = CHG_NONE;
            chgSettleS = 0;
            break;
        }
        if (v > chgMaxMv + CHG_FLAT_MV) { chgMaxMv = v; chgFlatS = 0; }
        else                              chgFlatS++;
        if (chgMaxMv >= CHG_FULL_MIN_MV && chgFlatS >= CHG_FLAT_S) {
            chgState = CHG_FULL;                // steigt nicht mehr -> voll
            chgS = 0;
        } else if (++chgS >= CHG_MAX_S) {
            chgState = CHG_NONE;                // Notbremse, falls "voll" nie erkannt wird
            chgSettleS = 0;
        }
        break;

    case CHG_FULL:
        // Wann abgesteckt wird, ist bei vollem Akku nicht erkennbar (kaum Strom
        // -> kein Sprung). Deshalb "voll" nur eine Zeit lang zeigen, dann aus.
        if (++chgS >= CHG_FULL_SHOW_S) {
            chgState = CHG_NONE;
            chgSettleS = 0;
        }
        break;
    }
    chgRefMv = v;
    chgLevel = battLevel(v);
}

// ============================================================================
//  Ausgaenge berechnen
// ============================================================================
static void outputUpdate(uint16_t now) {
    uint8_t  lim  = maxLevel();
    uint8_t  lvl  = level < lim ? level : lim;
    uint16_t duty = 0;

    if (mode == MODE_ON) {
        duty = LEVELS[lvl];
    } else if (mode == MODE_STROBE) {
        uint16_t ph = since(strobeT0, now) % (STROBE_ON_MS + STROBE_OFF_MS);
        duty = (ph < STROBE_ON_MS) ? LEVELS[lim] : 0;
    }

    // Doppelblink: aus-an-aus-an-aus, je 120 ms
    if (blinkActive) {
        uint16_t e = since(blinkT0, now);
        if (e >= 600) blinkActive = false;
        else          duty = ((e / 120) & 1) ? LEVELS[lvl] : 0;
    }
    setDuty(duty);

    // Taster-LED
    bool tled = false;
    if (battShowActive) {
        uint16_t e = since(battShowT0, now);
        uint16_t blinkLen = battShowCount * (BATT_ON_MS + BATT_OFF_MS);
        if (e < BATT_PAUSE_MS) {
            tled = false;                                         // Pause davor
        } else if (e < BATT_PAUSE_MS + blinkLen) {
            tled = ((e - BATT_PAUSE_MS) % (BATT_ON_MS + BATT_OFF_MS)) < BATT_ON_MS;
        } else if (e < BATT_PAUSE_MS + blinkLen + BATT_PAUSE_MS) {
            tled = false;                                         // Pause danach
        } else {
            battShowActive = false;
        }
    } else if (mode == MODE_OFF && chgState == CHG_ACTIVE) {
        // Laden: (Akkustand-1) kurze Blitzer + 1 langer Blinker ("laedt"), Pause, wiederholen
        uint16_t shortP = BATT_ON_MS + BATT_OFF_MS;
        uint16_t shorts = (uint16_t)(chgLevel - 1) * shortP;
        uint16_t cyc    = shorts + CHG_LONG_MS + CHG_GAP_MS;
        uint16_t e      = since(chgT0, now);
        if (e >= cyc) { chgT0 = now; e = 0; }   // neue Gruppe (neuer Akkustand wird uebernommen)
        if (e < shorts) tled = (e % shortP) < BATT_ON_MS;
        else            tled = (e - shorts) < CHG_LONG_MS;
    } else if (mode == MODE_OFF && chgState == CHG_FULL) {
        tled = true;                            // voll: Dauerlicht
    } else if (mode != MODE_OFF) {
        // Temperaturanzeige
        if (tempC < 40) {
            tled = true;
        } else {
            uint8_t idx = (tempC >= 70) ? 3 : (uint8_t)((tempC - 40) / 10);
            uint16_t p = TEMP_BLINK_PERIOD[idx];
            tled = (now % p) < (p / 2);
        }
    }
    setTasterLed(tled);
}

// ============================================================================
//  Sleep
// ============================================================================
static bool canSleep() {
    return mode == MODE_OFF && !btnRaw && !btnStable && clicks == 0 &&
           !battShowActive && !blinkActive && chgState == CHG_NONE;
}

static void goToSleep() {
    setDuty(0);
    setTasterLed(false);
    ADC0.CTRLA = 0;
    VREF.CTRLB = 0;
    TCB0.CTRLA = 0;

    set_sleep_mode(SLEEP_MODE_PWR_DOWN);
    cli();
    if (PORTA.IN & BTN_bm) {          // Taster nicht gedrueckt -> schlafen
        sleep_enable();
        sei();
        sleep_cpu();                  // ... wacht per PORTA-Interrupt auf
        sleep_disable();
    }
    sei();

    TCB0.CTRLA = TCB_CLKSEL_CLKDIV1_gc | TCB_ENABLE_bm;
    initAdc();
    if (g_pitTick && (PORTA.IN & BTN_bm)) return;   // nur RTC-Wecker: chargeTick misst selbst

    vbatMv = readVbatMv();            // frischer Wert (z.B. fuer Akkuanzeige aus dem Aus-Zustand)
    tempC  = readTempC();
    updateThermalLimit();
    releaseLowStage();
}

// ============================================================================
//  Arduino
// ============================================================================
void setup() {
    initPins();
    initTimers();
    initAdc();
    sei();

    vbatMv = readVbatMv();
    tempC  = readTempC();
    updateThermalLimit();

    uint16_t t = now_ms();
    lowSince = critSince = lastMeasT = btnChangeT = t;
}

void loop() {
    uint16_t now = now_ms();
    buttonUpdate(now);
    monitorUpdate(now);
    if (g_pitTick) {                  // 1-s-Takt der RTC
        g_pitTick = false;
        chargeTick(now);
    }
    outputUpdate(now);

    if (canSleep()) goToSleep();
}
