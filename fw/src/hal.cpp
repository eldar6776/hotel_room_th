#include "hal.h"
#include "settings.h"
#include "debug_logger.h"
#include <Arduino_GFX_Library.h>
#include <ESP32_4848S040.h>
#include <Wire.h>
#include <Touch_GT911.h>
#include <lvgl.h>

// ── Display (ST7701S 480×480 RGB) ────────────────────────────────────────────
// Using the correct two-object approach from the verified working example:
//   1. Arduino_ESP32SPI  — drives the ST7701 SPI configuration bus
//   2. Arduino_ESP32RGBPanel — the RGB parallel data bus (with correct 1,1 polarity)
//   3. Arduino_RGB_Display — combines both and applies the device-specific init table
static Arduino_ESP32SPI *s_spi_bus = new Arduino_ESP32SPI(
    GFX_NOT_DEFINED /* DC */, 39 /* CS */, 48 /* SCK */, 47 /* MOSI */, GFX_NOT_DEFINED /* MISO */);

static Arduino_ESP32RGBPanel *s_rgb_panel = new Arduino_ESP32RGBPanel(
    18 /* DE  */, 17 /* VSYNC */, 16 /* HSYNC */, 21 /* PCLK */,
    11 /* R0 */, 12 /* R1 */, 13 /* R2 */, 14 /* R3 */,  0 /* R4 */,
     8 /* G0 */, 20 /* G1 */,  3 /* G2 */, 46 /* G3 */,  9 /* G4 */, 10 /* G5 */,
     4 /* B0 */,  5 /* B1 */,  6 /* B2 */,  7 /* B3 */, 15 /* B4 */,
    1 /* hsync_polarity */, 10 /* hsync_front_porch */, 8 /* hsync_pulse_width */, 50 /* hsync_back_porch */,
    1 /* vsync_polarity */, 10 /* vsync_front_porch */, 8 /* vsync_pulse_width */, 20 /* vsync_back_porch */);

static Arduino_RGB_Display *s_gfx = new Arduino_RGB_Display(
    480 /* width */, 480 /* height */, s_rgb_panel, 0 /* rotation */, true /* auto_flush */,
    s_spi_bus, GFX_NOT_DEFINED /* RST */,
    st7701_4848s040_init_operations, sizeof(st7701_4848s040_init_operations));

// ── Touch (GT911: SDA=IO19, SCL=IO45 — I2C0) ────────────────────────────────
static Touch_GT911 s_touch(PIN_TOUCH_SDA, PIN_TOUCH_SCL,
                           -1 /* INT – not connected */,
                           -1 /* RST – not connected */,
                           480, 480);

// ── I2C1 za GPIO Expander (IO2=SDA, IO40=SCL) ───────────────────────────────
// Nezavisni hardverski I2C kontroler — nema konflikta sa Touch (I2C0)
static TwoWire s_extI2C(1);

// ── Expander state tracking ──────────────────────────────────────────────────
// PCF8574AN: nema odvojeni output register — moramo pratiti stanje softverski
// PCA9554ADH: ima output register ali pratimo stanje za hal_relay_is_on()
// ⚠️ INICIJALNO: 0x00 = svi biti su 0 = svi releji OFF (ulaz LOW na ULN2003)
static uint8_t s_exp_output_state = 0x00;  // Trenutni output latch — svi releji OFF
static uint8_t s_exp_config       = 0x00;  // 0=PCF8574AN, 1=PCA9554ADH
static bool    s_relay_fault      = false; // true = mismatch softver/hardver

// ── PCF8574AN registeri ──────────────────────────────────────────────────────
// PCF8574AN ima samo jedan register — read mijenja pinove u input mode
// Write postavlja izlaze (open-drain: 0=active LOW, 1=inactive HIGH)
#define PCF8574_REG           0x00

// ── PCA9554ADH registeri ─────────────────────────────────────────────────────
#define PCA_REG_INPUT         0x00
#define PCA_REG_OUTPUT        0x01
#define PCA_REG_POLARITY      0x02
#define PCA_REG_CONFIG        0x03

// PCA9554 konfiguracija: P0-P3=OUTPUT(0), P4-P7=INPUT(1)
#define PCA_CONFIG_VALUE      0xF0  // 0b11110000

// ── Relay bit maske (isti za oba expandera) ──────────────────────────────────
#define RELAY1_BIT            (1 << EXP_PIN_RELAY1)       // 0x01
#define RELAY2_BIT            (1 << EXP_PIN_RELAY2)       // 0x02
#define RELAY3_BIT            (1 << EXP_PIN_RELAY3)       // 0x04
#define WINDOW_BIT            (1 << EXP_PIN_WINDOW_SENSOR) // 0x40

// ============================================================================
//  I2C EXPANDER — INTERNI FUNKCIJE
// ============================================================================

// ── PCF8574AN — Pisanje output stanja ────────────────────────────────────────
// PCF8574AN: open-drain izlazi sa weak pull-up
//   0 = pin pulled LOW (relej ON — aktivira drajver)
//   1 = pin HIGH-Z + pull-up (relej OFF — pull-down otpornik drži LOW)
// ⚠️ VAŽNO: PCF8574AN koristi INVERZNU logiku zbog open-drain arhitekture!
//
// ⚠️ KRITIČNO PRAVILO IZ DATASHEETA (NXP PCA8574/PCF8574 §10.1):
//   "During a write, the input ports must be written as HIGH so the external
//    devices fully control the input ports."
//   P4-P7 su konfigurisani kao INPUT → bitovi 4-7 MORAJU biti 1 pri svakom
//   pisanju. Ako se napiše 0, aktivira se interni weak pull-up koji ometa
//   eksterne uređaje (npr. window sensor na P6).
//   Maska 0xF0 garantuje da input pinovi uvijek ostaju HIGH.
#define PCF8574_INPUT_PIN_MASK  0xF0  // P4-P7 = input, uvijek HIGH pri write

static void pcf8574_write(uint8_t value)
{
    // Prisili input pinove (P4-P7) na HIGH — datasheet zahtjev
    value |= PCF8574_INPUT_PIN_MASK;

    s_extI2C.beginTransmission(EXPANDER_I2C_ADDR);
    s_extI2C.write(value);
    s_extI2C.endTransmission();
}

// ── PCF8574AN — Čitanje input pinova ─────────────────────────────────────────
// ⚠️ PCF8574AN: read operacija privremeno postavlja SVE pinove u input mode!
// Nakon čitanja, moramo odmah vratiti output stanje.
static uint8_t pcf8574_read(void)
{
    uint8_t result = 0xFF;
    s_extI2C.requestFrom(EXPANDER_I2C_ADDR, 1);
    if (s_extI2C.available()) {
        result = s_extI2C.read();
    }
    // Odmah vrati output stanje — PCF8574AN je promijenio smjer pinova
    pcf8574_write(s_exp_output_state);
    return result;
}

// ── PCA9554ADH — Pisanje output registera sa verifikacijom ────────────────────
// Vraća true ako je write uspješan, false ako je I2C greška detektovana
static bool pca9554_write_output(uint8_t value)
{
    uint8_t i2c_err = 0;
    static uint8_t s_last_diag_value = 0xFF;
    
    // KORAK 1: Piši Output register
    s_extI2C.beginTransmission(EXPANDER_I2C_ADDR);
    s_extI2C.write(PCA_REG_OUTPUT);
    s_extI2C.write(value);
    i2c_err = s_extI2C.endTransmission();
    
    if (i2c_err != 0) {
        LOG_ERROR("[HAL] pca9554_write_output FAILED! value=0x%02X, I2C err=%d", value, i2c_err);
        return false;
    }
    
    // KORAK 2: Čitaj Output register da se verificira što je napisano
    delay(2);
    s_extI2C.beginTransmission(EXPANDER_I2C_ADDR);
    s_extI2C.write(PCA_REG_OUTPUT);
    i2c_err = s_extI2C.endTransmission(false);  // Repeated start
    
    if (i2c_err != 0) {
        LOG_ERROR("[HAL] pca9554_write_output: read verify failed, err=%d", i2c_err);
        return false;
    }
    
    s_extI2C.requestFrom(EXPANDER_I2C_ADDR, 1);
    if (s_extI2C.available()) {
        uint8_t read_back = s_extI2C.read();
        if (read_back != value) {
            LOG_ERROR("[HAL] Output write MISMATCH! Wrote 0x%02X but read back 0x%02X", value, read_back);
        }
    } else {
        LOG_ERROR("[HAL] pca9554_write_output: no read response");
        return false;
    }

    // KORAK 3: dijagnostika stvarnog nivoa pinova i trenutne konfiguracije.
    uint8_t cfg = 0xFF;
    uint8_t pins = 0xFF;

    s_extI2C.beginTransmission(EXPANDER_I2C_ADDR);
    s_extI2C.write(PCA_REG_CONFIG);
    i2c_err = s_extI2C.endTransmission(false);
    if (i2c_err != 0) {
        LOG_ERROR("[HAL] pca9554_write_output: config read failed, err=%d", i2c_err);
        return false;
    }
    s_extI2C.requestFrom(EXPANDER_I2C_ADDR, 1);
    if (s_extI2C.available()) {
        cfg = s_extI2C.read();
    } else {
        LOG_ERROR("[HAL] pca9554_write_output: no config response");
        return false;
    }

    s_extI2C.beginTransmission(EXPANDER_I2C_ADDR);
    s_extI2C.write(PCA_REG_INPUT);
    i2c_err = s_extI2C.endTransmission(false);
    if (i2c_err != 0) {
        LOG_ERROR("[HAL] pca9554_write_output: input read failed, err=%d", i2c_err);
        return false;
    }
    s_extI2C.requestFrom(EXPANDER_I2C_ADDR, 1);
    if (s_extI2C.available()) {
        pins = s_extI2C.read();
    } else {
        LOG_ERROR("[HAL] pca9554_write_output: no input response");
        return false;
    }

    uint8_t output_mask = (RELAY1_BIT | RELAY2_BIT | RELAY3_BIT | (1 << EXP_PIN_RESERVE_OUT));
    uint8_t expected_high = value & output_mask;
    uint8_t actual_high = pins & output_mask;
    uint8_t bad_mask = expected_high & (uint8_t)(~actual_high);

    if (bad_mask != 0 || cfg != PCA_CONFIG_VALUE || value != s_last_diag_value) {
        LOG_INFO("[HAL] PCA9554 diag: latch=0x%02X pins=0x%02X cfg=0x%02X badMask=0x%02X",
                 value, pins, cfg, bad_mask);
        s_last_diag_value = value;
    }

    if (bad_mask != 0) {
        LOG_ERROR("[HAL] PCA9554 output pin-level mismatch (expected HIGH not seen) mask=0x%02X", bad_mask);
    }

    return true;
}

// ── PCA9554ADH — Čitanje input registera s error handling ─────────────────────
static uint8_t pca9554_read_inputs(void)
{
    s_extI2C.beginTransmission(EXPANDER_I2C_ADDR);
    s_extI2C.write(PCA_REG_INPUT);
    uint8_t i2c_err = s_extI2C.endTransmission(false);  // Repeated start
    
    if (i2c_err != 0) {
        LOG_ERROR("[HAL] PCA9554 read_inputs: write phase failed, err=%d", i2c_err);
        return 0xFF;  // Safe default: all inputs HIGH
    }
    
    s_extI2C.requestFrom(EXPANDER_I2C_ADDR, 1);
    if (s_extI2C.available()) {
        return s_extI2C.read();
    }
    LOG_ERROR("[HAL] PCA9554 read_inputs: no data from slave");
    return 0xFF;  // Safe default: all inputs HIGH
}

// ── PCA9554ADH — Konfiguracija ───────────────────────────────────────────────
static void pca9554_configure(void)
{
    uint8_t verify_val = 0;
    uint8_t retries = 3;

    // KORAK 1: Polarity Inversion = 0 (normalna logika, bez invertovanja)
    s_extI2C.beginTransmission(EXPANDER_I2C_ADDR);
    s_extI2C.write(PCA_REG_POLARITY);
    s_extI2C.write(0x00);
    s_extI2C.endTransmission();
    delay(5);

    // KORAK 1.5: PRVO — Postavi Output Port register na 0x00 (svi output pinovi LOW za ULN2003)
    // PCA9554 power-on default za output register je 0xFF. Moramo ga postaviti na 0x00
    // PRIJE nego što pinove prebacimo u izlazni mod kako bismo izbjegli tranzicijski klik!
    s_extI2C.beginTransmission(EXPANDER_I2C_ADDR);
    s_extI2C.write(PCA_REG_OUTPUT);
    s_extI2C.write(0x00);
    s_extI2C.endTransmission();
    delay(5);
    s_exp_output_state = 0x00;  // Sync local copy

    // KORAK 2: Configuration register — postavlja smjer pinova
    // P0-P3 output (0), P4-P7 input (1) = 0xF0
    s_extI2C.beginTransmission(EXPANDER_I2C_ADDR);
    s_extI2C.write(PCA_REG_CONFIG);
    s_extI2C.write(PCA_CONFIG_VALUE);
    s_extI2C.endTransmission();
    delay(5);

    // KORAK 3: Verifikuj da je Configuration register pravilno postavljen
    while (retries > 0) {
        s_extI2C.beginTransmission(EXPANDER_I2C_ADDR);
        s_extI2C.write(PCA_REG_CONFIG);
        s_extI2C.endTransmission(false);  // Repeated start
        s_extI2C.requestFrom(EXPANDER_I2C_ADDR, 1);
        if (s_extI2C.available()) {
            verify_val = s_extI2C.read();
            if (verify_val == PCA_CONFIG_VALUE) {
                LOG_DEBUG("[HAL] PCA9554 Configuration verified: 0x%02X", verify_val);
                break;
            }
        }
        retries--;
        delay(2);
    }

    if (verify_val != PCA_CONFIG_VALUE) {
        LOG_ERROR("[HAL] PCA9554 Configuration MISMATCH! Expected 0x%02X, got 0x%02X",
                  PCA_CONFIG_VALUE, verify_val);
    }
}

// ============================================================================
//  I2C EXPANDER — INICIJALIZACIJA
// ============================================================================

static void expander_init(void)
{
    LOG_INFO("[HAL] Step 1a: I2C1 init (SDA=%d, SCL=%d)...", PIN_I2C_SDA, PIN_I2C_SCL);

    // Inicijalizacija I2C1 kontrolera na 400 kHz (Fast mode)
    s_extI2C.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
    delay(10);  // Kratka pauza za stabilizaciju I2C bus-a

#if EXPANDER_TYPE == EXPANDER_TYPE_PCF8574
    // ─── PCF8574AN INICIJALIZACIJA ───────────────────────────────────────
    // ⚠️ UPOZORENJE: PCF8574AN NIJE glitch-free!
    // Pri power-on: pinovi su INPUT sa internim pull-up HIGH (~100kΩ)
    // Output latch = 0xFF (svi HIGH). Releji mogu nakratko okinuti!
    // Hardverski 10kΩ pull-down na P0-P2 je OBAVEZAN!

    LOG_INFO("[HAL] Step 1b: PCF8574AN init (addr=0x%02X)...", EXPANDER_I2C_ADDR);

    // Verify I2C communication before proceeding
    s_extI2C.beginTransmission(EXPANDER_I2C_ADDR);
    s_extI2C.write(0xFF);
    uint8_t i2c_err = s_extI2C.endTransmission();
    if (i2c_err != 0) {
        LOG_ERROR("[HAL] PCF8574AN NOT FOUND at 0x%02X! I2C error=%d (0=OK, 1=data too long, 2=NACK addr, 3=NACK data, 4=other)",
                  EXPANDER_I2C_ADDR, i2c_err);
    } else {
        LOG_INFO("[HAL] PCF8574AN I2C ACK OK at 0x%02X", EXPANDER_I2C_ADDR);
    }

    // Postavi sve izlaze na HIGH (releji OFF — open-drain inactive)
    // PCF8574AN: HIGH = open-drain OFF, LOW = active (pulls pin to GND)
    s_exp_output_state = 0xFF;  // Svi pinovi HIGH (releji OFF)
    pcf8574_write(s_exp_output_state);

    // Verify write succeeded
    s_extI2C.beginTransmission(EXPANDER_I2C_ADDR);
    s_extI2C.write(0xFF);
    i2c_err = s_extI2C.endTransmission();
    if (i2c_err != 0) {
        LOG_ERROR("[HAL] PCF8574AN WRITE FAILED! err=%d", i2c_err);
    }

    s_exp_config = 0;

    LOG_INFO("[HAL] PCF8574AN OK — ⚠️ pull-down otpornici OBAVEZNI!");

#else
    // ─── PCA9554 INICIJALIZACIJA ─────────────────────────────────────────
    // ✅ GLITCH-FREE: Svi pinovi su INPUT (high-Z) pri power-on
    // Output latch = 0x00. Nema izlaznog signala dok se ne konfigurira.

    LOG_INFO("[HAL] Step 1b: PCA9554 init (addr=0x%02X)...", EXPANDER_I2C_ADDR);

    // Provjeri I2C komunikaciju prije nastavka
    s_extI2C.beginTransmission(EXPANDER_I2C_ADDR);
    uint8_t i2c_err = s_extI2C.endTransmission();
    if (i2c_err != 0) {
        LOG_ERROR("[HAL] PCA9554 NOT FOUND at 0x%02X! I2C error=%d (0=OK, 1=data too long, 2=NACK addr, 3=NACK data, 4=other)",
                  EXPANDER_I2C_ADDR, i2c_err);
    } else {
        LOG_INFO("[HAL] PCA9554 I2C ACK OK at 0x%02X", EXPANDER_I2C_ADDR);
    }

    // GLITCH-FREE SEKVENCA (ispravljen redoslijed):
    // KORAK 1: PRVO — postaviti Configuration register (P0-P3 output, P4-P7 input)
    // Trebalo je ovo raditi PRIJE nego pisanja Output registra!
    // Trebam čitati datasheet sekvenciju inicijalizacije!
    pca9554_configure();
    s_exp_config = 1;
    delay(5);

    // KORAK 2: TEK SADA — Postavi Output Port register na 0x00 (svi LOW — releji OFF za ULN2003)
    // ULN2003 open-collector: 0=transistor OFF, 1=transistor ON
    s_exp_output_state = 0x00;  // Svi izlazi OFF (transistori OFF)
    pca9554_write_output(s_exp_output_state);
    delay(5);

    // KORAK 3: Čitaj sve output pinove da se provjeri je li inicijalizacija bila uspješna
    uint8_t outputs_read = pca9554_read_inputs();
    LOG_DEBUG("[HAL] PCA9554 output read after init: 0x%02X (expect P4-P7 from inputs)", outputs_read);

    // KORAK 4: DEBUG — Čitaj sve registre da se vidi stanje na chip-u
    uint8_t reg_input = 0xFF, reg_output = 0, reg_polarity = 0, reg_config = 0;
    
    // Čitaj Input register (reg 0)
    s_extI2C.beginTransmission(EXPANDER_I2C_ADDR);
    s_extI2C.write(0x00);
    s_extI2C.endTransmission(false);
    s_extI2C.requestFrom(EXPANDER_I2C_ADDR, 1);
    if (s_extI2C.available()) reg_input = s_extI2C.read();
    
    // Čitaj Output register (reg 1)
    s_extI2C.beginTransmission(EXPANDER_I2C_ADDR);
    s_extI2C.write(0x01);
    s_extI2C.endTransmission(false);
    s_extI2C.requestFrom(EXPANDER_I2C_ADDR, 1);
    if (s_extI2C.available()) reg_output = s_extI2C.read();
    
    // Čitaj Polarity register (reg 2)
    s_extI2C.beginTransmission(EXPANDER_I2C_ADDR);
    s_extI2C.write(0x02);
    s_extI2C.endTransmission(false);
    s_extI2C.requestFrom(EXPANDER_I2C_ADDR, 1);
    if (s_extI2C.available()) reg_polarity = s_extI2C.read();
    
    // Čitaj Config register (reg 3)
    s_extI2C.beginTransmission(EXPANDER_I2C_ADDR);
    s_extI2C.write(0x03);
    s_extI2C.endTransmission(false);
    s_extI2C.requestFrom(EXPANDER_I2C_ADDR, 1);
    if (s_extI2C.available()) reg_config = s_extI2C.read();
    
    LOG_INFO("[HAL] PCA9554 REGISTERS: Input=0x%02X Output=0x%02X Polarity=0x%02X Config=0x%02X",
             reg_input, reg_output, reg_polarity, reg_config);

    LOG_INFO("[HAL] PCA9554 OK — ✅ glitch-free inicijalizacija");
#endif
}

// ============================================================================
//  I2C EXPANDER — PUBLIC ADAPTER API
// ============================================================================

void hal_relay_set(uint8_t relay_id, bool on)
{
    if (relay_id < 1 || relay_id > 3) return;

    uint8_t bit = (1 << (relay_id - 1));  // relay 1→bit0, 2→bit1, 3→bit2

#if EXPANDER_TYPE == EXPANDER_TYPE_PCF8574
    // PCF8574AN: open-drain, 1=OFF (high-Z), 0=ON (pulls LOW)
    if (on) {
        s_exp_output_state &= ~bit;  // 0 = relej ON
    } else {
        s_exp_output_state |= bit;   // 1 = relej OFF
    }
    pcf8574_write(s_exp_output_state);
#else
    // PCA9554ADH + ULN2003 open-collector: 1=ON (transistor ON, izlaz LOW), 0=OFF (transistor OFF, izlaz HIGH)
    uint8_t new_state = s_exp_output_state;
    if (on) {
        new_state |= bit;   // 1 = relej ON (ULN2003 transistor ON)
    } else {
        new_state &= ~bit;  // 0 = relej OFF (ULN2003 transistor OFF)
    }
    s_exp_output_state = new_state;
    bool ok = pca9554_write_output(s_exp_output_state);
    if (ok) {
        s_relay_fault = false;
    } else {
        s_relay_fault = true;
        LOG_ERROR("[HAL] Relay %d write FAILED — fault flag set", relay_id);
    }
#endif
}

bool hal_relay_is_on(uint8_t relay_id)
{
    if (relay_id < 1 || relay_id > 3) return false;
    uint8_t bit = (1 << (relay_id - 1));

#if EXPANDER_TYPE == EXPANDER_TYPE_PCF8574
    // PCF8574AN: 0 = ON (inverzna logika)
    return (s_exp_output_state & bit) == 0;
#else
    // PCA9554ADH + ULN2003: 1 = ON (direktna logika open-collector)
    return (s_exp_output_state & bit) != 0;
#endif
}

bool hal_relay_verify_on(uint8_t relay_id)
{
    if (relay_id < 1 || relay_id > 3) return false;
    uint8_t bit = (1 << (relay_id - 1));

#if EXPANDER_TYPE == EXPANDER_TYPE_PCF8574
    uint8_t inputs = pcf8574_read();
    // PCF8574AN: 0 = ON (inverzna logika)
    return (inputs & bit) == 0;
#else
    // PCA9554: čitamo input register — output pinovi se vide kao njihovi stvarni nivoi
    uint8_t inputs = pca9554_read_inputs();
    // PCA9554 + ULN2003: 1 = ON (ULN2003 transistor ON → izlaz LOW, ali expander pin HIGH)
    return (inputs & bit) != 0;
#endif
}

bool hal_relay_has_fault(void)
{
    return s_relay_fault;
}

void hal_relay_all_off(void)
{
    // PCF8574AN: 1 = OFF (all HIGH)
    // PCA9554 + ULN2003: 0 = OFF (all LOW — transistori OFF, izlazi floats HIGH)
#if EXPANDER_TYPE == EXPANDER_TYPE_PCF8574
    s_exp_output_state |= (RELAY1_BIT | RELAY2_BIT | RELAY3_BIT);  // Svi HIGH = OFF
    pcf8574_write(s_exp_output_state);
#else
    uint8_t new_state = s_exp_output_state & ~(RELAY1_BIT | RELAY2_BIT | RELAY3_BIT);
    s_exp_output_state = new_state;
    bool ok = pca9554_write_output(s_exp_output_state);
    if (ok) {
        s_relay_fault = false;
    } else {
        s_relay_fault = true;
        LOG_ERROR("[HAL] Relay all_off FAILED — fault flag set");
    }
#endif
}

void hal_relay_sync(void)
{
    // Forsira pisanje trenutnog s_exp_output_state na hardver.
    // Ovo osigurava da se I2C ekspander vrati u ispravno stanje ako se desio glitch ili restart.
#if EXPANDER_TYPE == EXPANDER_TYPE_PCF8574
    pcf8574_write(s_exp_output_state);
#else
    bool ok = pca9554_write_output(s_exp_output_state);
    if (ok) {
        s_relay_fault = false;
    } else {
        s_relay_fault = true;
        LOG_ERROR("[HAL] Relay sync FAILED — fault flag set");
    }
#endif
}

bool hal_window_is_open(void)
{
    uint8_t inputs;

#if EXPANDER_TYPE == EXPANDER_TYPE_PCF8574
    inputs = pcf8574_read();
#else
    inputs = pca9554_read_inputs();
#endif

    // Window sensor: Active LOW na P6
    // 0 = prozor otvoren, 1 = prozor zatvoren
    return (inputs & WINDOW_BIT) == 0;
}

uint8_t hal_expander_read_inputs(void)
{
#if EXPANDER_TYPE == EXPANDER_TYPE_PCF8574
    return pcf8574_read();
#else
    return pca9554_read_inputs();
#endif
}

// ── LVGL draw buffer (single buffer in internal SRAM for speed) ──────────────
static lv_disp_draw_buf_t s_draw_buf;
static lv_color_t        *s_buf1 = nullptr;
static bool              s_touch_was_pressed = false;
#define DISP_BUF_LINES  80

// ── LVGL flush callback (v8 API) ─────────────────────────────────────────────
static void disp_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    uint32_t w = (uint32_t)(area->x2 - area->x1 + 1);
    uint32_t h = (uint32_t)(area->y2 - area->y1 + 1);
    s_gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)color_p, w, h);
    lv_disp_flush_ready(drv);
}

// ── LVGL touch read callback (v8 API) ────────────────────────────────────────
static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    s_touch.read();
    if (s_touch.isTouched && s_touch.touches > 0) {
        if (!s_touch_was_pressed) {
            inactivity_touch_event();
        }
        s_touch_was_pressed = true;
        data->point.x = 480 - s_touch.points[0].x;
        data->point.y = 480 - s_touch.points[0].y;
        data->state   = LV_INDEV_STATE_PRESSED;
    } else {
        s_touch_was_pressed = false;
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// ── Backlight (analogWrite, 0-255) ───────────────────────────────────────────
void hal_backlight_set(uint16_t level)
{
    static uint16_t s_current_level = UINT16_MAX;  // force write on first call
    if (level == s_current_level) return;
    s_current_level = level;
    // Map 0-1023 (PWM duty cycle range) → 0-255 (analogWrite range)
    uint8_t pwm = (level > 1023) ? 255 : (level * 255 / 1023);
    analogWrite(PIN_LCD_BL, pwm);
    LOG_DEBUG("Backlight: %u → PWM %u", level, pwm);
}

// ── LVGL tick task (stub — LV_TICK_CUSTOM=1 handles timing) ──────────────────
void hal_lvgl_tick_task(void *pv)
{
    (void)pv;
    vTaskDelete(nullptr);
}

// ── Main init ────────────────────────────────────────────────────────────────
void board_hal_init(void)
{
    // ── KORAK 1: I2C1 + GPIO Expander ───────────────────────────────────
    // Inicijalizacija se radi PRIJE displeja jer releji moraju biti
    // postavljeni na OFF što je ranije moguće (~410ms od power-on).
    expander_init();

    // ── KORAK 2: RS485 DE pin (IO41) ────────────────────────────────────
    // RS485 Driver Enable — dedicated pin, bez sharinga
    LOG_INFO("[HAL] Step 2: RS485 DE (IO41)...");
    pinMode(PIN_RS485_DE, OUTPUT);
    digitalWrite(PIN_RS485_DE, LOW);  // Receive mode (safe default)

    // ── KORAK 3: Backlight (analogWrite) ────────────────────────────────
    LOG_INFO("[HAL] Step 3: Backlight (analogWrite)...");
    pinMode(PIN_LCD_BL, OUTPUT);
    analogWriteFrequency(25000);  // 25 kHz backlight PWM, before first analogWrite
    hal_backlight_set(128);  // ~50% until NVS value is loaded

    // ── KORAK 4: Display init (gfx->begin) ──────────────────────────────
    LOG_INFO("[HAL] Step 4: gfx->begin()...");
    if (!s_gfx->begin()) {
        LOG_ERROR("[HAL] Display begin() FAILED");
    }
    LOG_INFO("[HAL] Step 4 done.");

    // ── KORAK 5: LVGL init ──────────────────────────────────────────────
    LOG_INFO("[HAL] Step 5: lv_init()...");
    lv_init();

    // ── KORAK 6: LVGL draw buffer ───────────────────────────────────────
    LOG_INFO("[HAL] Step 6: SRAM alloc...");
    uint32_t buf_px = 480 * DISP_BUF_LINES;
    s_buf1 = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t),
                                             MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_buf1) {
        LOG_ERROR("[HAL] Cannot allocate LVGL draw buffer in SRAM!");
    }
    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, NULL, buf_px);

    // ── KORAK 7: LVGL display driver ────────────────────────────────────
    LOG_INFO("[HAL] Step 7: register LVGL display driver (v8)...");
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = 480;
    disp_drv.ver_res  = 480;
    disp_drv.flush_cb = disp_flush_cb;
    disp_drv.draw_buf = &s_draw_buf;
    lv_disp_drv_register(&disp_drv);

    // ── KORAK 8: Touch init (I2C0: SDA=IO19, SCL=IO45) ─────────────────
    LOG_INFO("[HAL] Step 8: touch init (SDA=%d, SCL=%d)...", PIN_TOUCH_SDA, PIN_TOUCH_SCL);
    // Wire.begin() is called inside s_touch.begin() - no need to call twice!
    s_touch.begin();

    // ── KORAK 9: LVGL input driver ──────────────────────────────────────
    LOG_INFO("[HAL] Step 9: register LVGL input driver (v8)...");
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_read_cb;
    lv_indev_drv_register(&indev_drv);

    LOG_INFO("[HAL] Init OK");
}
