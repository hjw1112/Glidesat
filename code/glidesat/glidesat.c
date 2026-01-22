#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/spi.h"
#include "hardware/adc.h"
#include "hardware/pwm.h"
#include "hardware/irq.h"

// ---------------- Pins (from schematic) ----------------
#define LED_BLUE_PIN        19
#define LED_RED_PIN         20

// I2C shared by MPU6050 + BMP280
#define I2C_PORT            i2c0
#define I2C_SDA_PIN         16
#define I2C_SCL_PIN         17
#define I2C_BAUD            400000

// Battery ADC
#define BAT_ADC_GPIO        26
#define BAT_ADC_INPUT       0   // ADC0

// RFM95W (SX127x) on SPI1
#define LORA_SPI            spi1
#define LORA_SCK_PIN        10
#define LORA_MOSI_PIN       11
#define LORA_MISO_PIN       12
#define LORA_NSS_PIN        9
#define LORA_RST_PIN        13
#define LORA_DIO0_PIN       14
#define LORA_DIO1_PIN       15
#define LORA_DIO2_PIN       3

// CM4 comm (UART1)
#define CM4_UART            uart1
#define CM4_UART_BAUD       115200
#define CM4_TX_PIN          4
#define CM4_RX_PIN          5
#define CM4_TRIGGER_PIN     6
#define CM4_RST_PIN         7

// Buzzer PWM
#define BUZZER_PIN          2

// Optional GPS_TX net (you labeled GPIO24)
#define GPS_TX_PIN          24

// ---------------- I2C devices ----------------
#define MPU6050_ADDR        0x68
#define BMP280_ADDR         0x76

// ---------------- Utility: GPIO ----------------
static inline void led_blue(bool on) { gpio_put(LED_BLUE_PIN, on); }
static inline void led_red (bool on) { gpio_put(LED_RED_PIN,  on); }

// ---------------- I2C helpers ----------------
static bool i2c_write_reg(uint8_t addr, uint8_t reg, const uint8_t *data, size_t len) {
    uint8_t buf[1 + 32];
    if (len > 32) return false;
    buf[0] = reg;
    memcpy(&buf[1], data, len);
    int r = i2c_write_blocking(I2C_PORT, addr, buf, 1 + len, false);
    return r == (int)(1 + len);
}

static bool i2c_write_u8(uint8_t addr, uint8_t reg, uint8_t v) {
    return i2c_write_reg(addr, reg, &v, 1);
}

static bool i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *out, size_t len) {
    int r1 = i2c_write_blocking(I2C_PORT, addr, &reg, 1, true);
    if (r1 != 1) return false;
    int r2 = i2c_read_blocking(I2C_PORT, addr, out, len, false);
    return r2 == (int)len;
}

// ---------------- MPU6050 ----------------
// Minimal init + read accel/gyro/temp raw.
static bool mpu6050_init(void) {
    // WHO_AM_I = 0x75 should read 0x68
    uint8_t who = 0;
    if (!i2c_read_reg(MPU6050_ADDR, 0x75, &who, 1)) return false;
    if (who != 0x68) return false;

    // PWR_MGMT_1 (0x6B): wake up (clear sleep)
    if (!i2c_write_u8(MPU6050_ADDR, 0x6B, 0x00)) return false;

    // CONFIG (0x1A): DLPF cfg (e.g. 3)
    i2c_write_u8(MPU6050_ADDR, 0x1A, 0x03);

    // GYRO_CONFIG (0x1B): ±500 dps -> 0x08
    i2c_write_u8(MPU6050_ADDR, 0x1B, 0x08);

    // ACCEL_CONFIG (0x1C): ±4g -> 0x08
    i2c_write_u8(MPU6050_ADDR, 0x1C, 0x08);

    return true;
}

typedef struct {
    int16_t ax, ay, az;
    int16_t temp;
    int16_t gx, gy, gz;
} mpu_raw_t;

static bool mpu6050_read_raw(mpu_raw_t *out) {
    uint8_t buf[14];
    if (!i2c_read_reg(MPU6050_ADDR, 0x3B, buf, 14)) return false;
    out->ax   = (int16_t)((buf[0] << 8) | buf[1]);
    out->ay   = (int16_t)((buf[2] << 8) | buf[3]);
    out->az   = (int16_t)((buf[4] << 8) | buf[5]);
    out->temp = (int16_t)((buf[6] << 8) | buf[7]);
    out->gx   = (int16_t)((buf[8] << 8) | buf[9]);
    out->gy   = (int16_t)((buf[10] << 8) | buf[11]);
    out->gz   = (int16_t)((buf[12] << 8) | buf[13]);
    return true;
}

// Convert MPU temp raw -> °C
static float mpu6050_temp_c(int16_t raw) {
    // Datasheet: Temp(°C) = raw/340 + 36.53
    return ((float)raw / 340.0f) + 36.53f;
}

// ---------------- BMP280 ----------------
// Compensation structs/vars (from datasheet; using 32-bit int math)
typedef struct {
    uint16_t dig_T1; int16_t dig_T2; int16_t dig_T3;
    uint16_t dig_P1; int16_t dig_P2; int16_t dig_P3; int16_t dig_P4;
    int16_t dig_P5;  int16_t dig_P6; int16_t dig_P7; int16_t dig_P8; int16_t dig_P9;
    int32_t t_fine;
} bmp280_cal_t;

static bmp280_cal_t bmp_cal;

static bool bmp280_read_calibration(void) {
    uint8_t c[24];
    if (!i2c_read_reg(BMP280_ADDR, 0x88, c, 24)) return false;

    bmp_cal.dig_T1 = (uint16_t)(c[1] << 8 | c[0]);
    bmp_cal.dig_T2 = (int16_t)(c[3] << 8 | c[2]);
    bmp_cal.dig_T3 = (int16_t)(c[5] << 8 | c[4]);

    bmp_cal.dig_P1 = (uint16_t)(c[7] << 8 | c[6]);
    bmp_cal.dig_P2 = (int16_t)(c[9] << 8 | c[8]);
    bmp_cal.dig_P3 = (int16_t)(c[11] << 8 | c[10]);
    bmp_cal.dig_P4 = (int16_t)(c[13] << 8 | c[12]);
    bmp_cal.dig_P5 = (int16_t)(c[15] << 8 | c[14]);
    bmp_cal.dig_P6 = (int16_t)(c[17] << 8 | c[16]);
    bmp_cal.dig_P7 = (int16_t)(c[19] << 8 | c[18]);
    bmp_cal.dig_P8 = (int16_t)(c[21] << 8 | c[20]);
    bmp_cal.dig_P9 = (int16_t)(c[23] << 8 | c[22]);

    return true;
}

static bool bmp280_init(void) {
    // ID register 0xD0 should be 0x58 for BMP280
    uint8_t id = 0;
    if (!i2c_read_reg(BMP280_ADDR, 0xD0, &id, 1)) return false;
    if (id != 0x58) return false;

    if (!bmp280_read_calibration()) return false;

    // ctrl_meas (0xF4): temp oversampling x2 (010), pressure oversampling x16 (101), normal mode (11)
    // osrs_t=2, osrs_p=16, mode=normal => 0b010 101 11 = 0x57
    i2c_write_u8(BMP280_ADDR, 0xF4, 0x57);

    // config (0xF5): standby 500ms (100), filter x16 (100), spi3w off
    // 0b100 100 0 0 = 0x90
    i2c_write_u8(BMP280_ADDR, 0xF5, 0x90);

    return true;
}

static bool bmp280_read_raw(int32_t *adc_T, int32_t *adc_P) {
    uint8_t b[6];
    if (!i2c_read_reg(BMP280_ADDR, 0xF7, b, 6)) return false;

    int32_t p = (int32_t)((b[0] << 12) | (b[1] << 4) | (b[2] >> 4));
    int32_t t = (int32_t)((b[3] << 12) | (b[4] << 4) | (b[5] >> 4));
    *adc_T = t;
    *adc_P = p;
    return true;
}

// Returns temperature in 0.01°C and pressure in Pa (int32)
static void bmp280_compensate(int32_t adc_T, int32_t adc_P, int32_t *temp_c_x100, int32_t *press_pa) {
    // Temperature compensation (datasheet)
    int32_t var1, var2;
    var1 = ((((adc_T >> 3) - ((int32_t)bmp_cal.dig_T1 << 1))) * ((int32_t)bmp_cal.dig_T2)) >> 11;
    var2 = (((((adc_T >> 4) - ((int32_t)bmp_cal.dig_T1)) * ((adc_T >> 4) - ((int32_t)bmp_cal.dig_T1))) >> 12) *
            ((int32_t)bmp_cal.dig_T3)) >> 14;
    bmp_cal.t_fine = var1 + var2;
    int32_t T = (bmp_cal.t_fine * 5 + 128) >> 8;
    *temp_c_x100 = T;

    // Pressure compensation (datasheet, 64-bit intermediate)
    int64_t v1, v2, p;
    v1 = ((int64_t)bmp_cal.t_fine) - 128000;
    v2 = v1 * v1 * (int64_t)bmp_cal.dig_P6;
    v2 = v2 + ((v1 * (int64_t)bmp_cal.dig_P5) << 17);
    v2 = v2 + (((int64_t)bmp_cal.dig_P4) << 35);
    v1 = ((v1 * v1 * (int64_t)bmp_cal.dig_P3) >> 8) + ((v1 * (int64_t)bmp_cal.dig_P2) << 12);
    v1 = (((((int64_t)1) << 47) + v1) * (int64_t)bmp_cal.dig_P1) >> 33;

    if (v1 == 0) { *press_pa = 0; return; } // avoid div0

    p = 1048576 - adc_P;
    p = (((p << 31) - v2) * 3125) / v1;
    v1 = (((int64_t)bmp_cal.dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    v2 = (((int64_t)bmp_cal.dig_P8) * p) >> 19;
    p = ((p + v1 + v2) >> 8) + (((int64_t)bmp_cal.dig_P7) << 4);
    *press_pa = (int32_t)(p >> 8);
}

// Approx altitude from pressure (Pa), sea-level default 101325 Pa
static float altitude_m(float press_pa, float sea_level_pa) {
    // barometric formula (simple)
    return 44330.0f * (1.0f - powf(press_pa / sea_level_pa, 0.1903f));
}

// ---------------- Battery ADC ----------------
static float read_battery_voltage(void) {
    // ADC reading: 0..4095 maps to 0..3.3V (approx; uses ADC reference)
    const float vref = 3.3f;
    uint16_t raw = adc_read();

    float v_adc = (raw * vref) / 4095.0f;

    // Divider: Vadc = Vbat * (Rbot/(Rtop+Rbot)) with Rtop=10k, Rbot=22k => factor = 22/32 = 0.6875
    // So Vbat = Vadc / 0.6875 = Vadc * 1.454545...
    float v_bat = v_adc * (32.0f / 22.0f);
    return v_bat;
}

// ---------------- Buzzer (PWM) ----------------
static void buzzer_init(void) {
    gpio_set_function(BUZZER_PIN, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(BUZZER_PIN);
    pwm_set_enabled(slice, false);
}

static void buzzer_beep(uint freq_hz, uint ms) {
    uint slice = pwm_gpio_to_slice_num(BUZZER_PIN);

    // Very simple PWM tone setup:
    // f_pwm = sys_clk / (clkdiv * (wrap+1))
    // Choose wrap=1000 and compute clkdiv.
    const uint32_t sys = clock_get_hz(clk_sys);
    const uint16_t wrap = 1000;
    float clkdiv = (float)sys / ((float)freq_hz * (wrap + 1));

    if (clkdiv < 1.0f) clkdiv = 1.0f;
    if (clkdiv > 255.0f) clkdiv = 255.0f;

    pwm_set_clkdiv(slice, clkdiv);
    pwm_set_wrap(slice, wrap);
    pwm_set_gpio_level(BUZZER_PIN, wrap / 2);
    pwm_set_enabled(slice, true);
    sleep_ms(ms);
    pwm_set_enabled(slice, false);
}

// ---------------- SX127x (RFM95W) minimal driver ----------------
// SPI helpers
static inline void lora_cs(bool level) { gpio_put(LORA_NSS_PIN, level); }

static uint8_t lora_read_reg(uint8_t reg) {
    // reg with MSB=0 for read
    uint8_t tx[2] = { (uint8_t)(reg & 0x7F), 0x00 };
    uint8_t rx[2] = {0};
    lora_cs(false);
    spi_write_read_blocking(LORA_SPI, tx, rx, 2);
    lora_cs(true);
    return rx[1];
}

static void lora_write_reg(uint8_t reg, uint8_t val) {
    // reg with MSB=1 for write
    uint8_t tx[2] = { (uint8_t)(reg | 0x80), val };
    lora_cs(false);
    spi_write_blocking(LORA_SPI, tx, 2);
    lora_cs(true);
}

// SX127x registers (subset)
#define REG_FIFO                0x00
#define REG_OP_MODE             0x01
#define REG_FRF_MSB             0x06
#define REG_FRF_MID             0x07
#define REG_FRF_LSB             0x08
#define REG_PA_CONFIG           0x09
#define REG_LNA                 0x0C
#define REG_FIFO_ADDR_PTR       0x0D
#define REG_FIFO_TX_BASE_ADDR   0x0E
#define REG_FIFO_RX_BASE_ADDR   0x0F
#define REG_IRQ_FLAGS           0x12
#define REG_RX_NB_BYTES         0x13
#define REG_PKT_SNR_VALUE       0x19
#define REG_PKT_RSSI_VALUE      0x1A
#define REG_MODEM_CONFIG_1      0x1D
#define REG_MODEM_CONFIG_2      0x1E
#define REG_PREAMBLE_MSB        0x20
#define REG_PREAMBLE_LSB        0x21
#define REG_PAYLOAD_LENGTH      0x22
#define REG_MODEM_CONFIG_3      0x26
#define REG_DIO_MAPPING_1       0x40
#define REG_VERSION             0x42

// Modes
#define MODE_LONG_RANGE_MODE    0x80
#define MODE_SLEEP              0x00
#define MODE_STDBY              0x01
#define MODE_TX                 0x03

static volatile bool lora_tx_done = false;

static void __isr lora_dio0_irq(uint gpio, uint32_t events) {
    (void)gpio; (void)events;
    lora_tx_done = true;
}

static void lora_set_frequency_hz(uint32_t freq_hz) {
    // FRF = freq_hz / (32e6 / 2^19) = freq_hz * 2^19 / 32e6
    uint64_t frf = ((uint64_t)freq_hz << 19) / 32000000ULL;
    lora_write_reg(REG_FRF_MSB, (uint8_t)(frf >> 16));
    lora_write_reg(REG_FRF_MID, (uint8_t)(frf >> 8));
    lora_write_reg(REG_FRF_LSB, (uint8_t)(frf >> 0));
}

static bool lora_init(void) {
    // Reset pulse
    gpio_put(LORA_RST_PIN, 0);
    sleep_ms(10);
    gpio_put(LORA_RST_PIN, 1);
    sleep_ms(10);

    uint8_t ver = lora_read_reg(REG_VERSION);
    // SX1276 is typically 0x12
    if (ver == 0x00 || ver == 0xFF) return false;

    // Sleep + LoRa
    lora_write_reg(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_SLEEP);
    sleep_ms(5);

    // Standby
    lora_write_reg(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_STDBY);

    // Base addresses
    lora_write_reg(REG_FIFO_TX_BASE_ADDR, 0x00);
    lora_write_reg(REG_FIFO_RX_BASE_ADDR, 0x00);

    // Frequency: choose 868 MHz (change to 915e6 if needed)
    lora_set_frequency_hz(868000000UL);

    // PA config: PA_BOOST, power ~17dBm (0x8F is common-ish; tune for your region/rules)
    lora_write_reg(REG_PA_CONFIG, 0x8F);

    // LNA boost
    lora_write_reg(REG_LNA, lora_read_reg(REG_LNA) | 0x03);

    // Modem config:
    // BW=125kHz (0x70), CR=4/5 (0x02), explicit header (0x00) => 0x72
    lora_write_reg(REG_MODEM_CONFIG_1, 0x72);
    // SF=7 (0x70), CRC on (0x04) => 0x74
    lora_write_reg(REG_MODEM_CONFIG_2, 0x74);
    // LowDataRateOptimize off, AGC auto on => 0x04
    lora_write_reg(REG_MODEM_CONFIG_3, 0x04);

    // Preamble 8
    lora_write_reg(REG_PREAMBLE_MSB, 0x00);
    lora_write_reg(REG_PREAMBLE_LSB, 0x08);

    // DIO0 mapping to TxDone (00)
    lora_write_reg(REG_DIO_MAPPING_1, 0x00);

    // Clear IRQ flags
    lora_write_reg(REG_IRQ_FLAGS, 0xFF);

    return true;
}

static void lora_write_fifo(const uint8_t *data, size_t len) {
    // Write FIFO in burst: addr|0x80 then payload
    uint8_t header = (uint8_t)(REG_FIFO | 0x80);
    lora_cs(false);
    spi_write_blocking(LORA_SPI, &header, 1);
    spi_write_blocking(LORA_SPI, data, len);
    lora_cs(true);
}

static bool lora_send_packet(const uint8_t *data, size_t len, uint32_t timeout_ms) {
    if (len > 255) return false;
    lora_tx_done = false;

    lora_write_reg(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_STDBY);
    lora_write_reg(REG_FIFO_ADDR_PTR, 0x00);
    lora_write_reg(REG_PAYLOAD_LENGTH, (uint8_t)len);

    lora_write_fifo(data, len);

    // Clear IRQ flags, then TX
    lora_write_reg(REG_IRQ_FLAGS, 0xFF);
    lora_write_reg(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_TX);

    absolute_time_t t0 = get_absolute_time();
    while (!lora_tx_done) {
        if (absolute_time_diff_us(t0, get_absolute_time()) > (int64_t)timeout_ms * 1000) {
            return false;
        }
        tight_loop_contents();
    }

    // Return to standby
    lora_write_reg(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_STDBY);
    return true;
}

// ---------------- CM4 UART ----------------
static void cm4_uart_init(void) {
    uart_init(CM4_UART, CM4_UART_BAUD);
    gpio_set_function(CM4_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(CM4_RX_PIN, GPIO_FUNC_UART);
    uart_set_format(CM4_UART, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(CM4_UART, true);

    gpio_init(CM4_TRIGGER_PIN);
    gpio_set_dir(CM4_TRIGGER_PIN, GPIO_IN);
    gpio_pull_down(CM4_TRIGGER_PIN);

    gpio_init(CM4_RST_PIN);
    gpio_set_dir(CM4_RST_PIN, GPIO_OUT);
    gpio_put(CM4_RST_PIN, 1);
}

static void cm4_send_line(const char *s) {
    uart_puts(CM4_UART, s);
    uart_puts(CM4_UART, "\r\n");
}

// ---------------- Main ----------------
int main(void) {
    stdio_init_all();
    sleep_ms(1200);

    // LEDs
    gpio_init(LED_BLUE_PIN); gpio_set_dir(LED_BLUE_PIN, GPIO_OUT);
    gpio_init(LED_RED_PIN);  gpio_set_dir(LED_RED_PIN,  GPIO_OUT);
    led_blue(false); led_red(false);

    // I2C
    i2c_init(I2C_PORT, I2C_BAUD);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);

    // ADC battery
    adc_init();
    adc_gpio_init(BAT_ADC_GPIO);
    adc_select_input(BAT_ADC_INPUT);

    // Buzzer
    buzzer_init();

    // SPI for LoRa
    spi_init(LORA_SPI, 8 * 1000 * 1000); // 8 MHz
    gpio_set_function(LORA_SCK_PIN,  GPIO_FUNC_SPI);
    gpio_set_function(LORA_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(LORA_MISO_PIN, GPIO_FUNC_SPI);

    gpio_init(LORA_NSS_PIN);
    gpio_set_dir(LORA_NSS_PIN, GPIO_OUT);
    lora_cs(true);

    gpio_init(LORA_RST_PIN);
    gpio_set_dir(LORA_RST_PIN, GPIO_OUT);
    gpio_put(LORA_RST_PIN, 1);

    gpio_init(LORA_DIO0_PIN);
    gpio_set_dir(LORA_DIO0_PIN, GPIO_IN);
    gpio_pull_down(LORA_DIO0_PIN);

    // DIO0 interrupt for TxDone
    gpio_set_irq_enabled_with_callback(LORA_DIO0_PIN, GPIO_IRQ_EDGE_RISE, true, &lora_dio0_irq);

    // CM4 comm
    cm4_uart_init();

    // Init sensors
    bool ok_mpu = mpu6050_init();
    bool ok_bmp = bmp280_init();
    bool ok_lora = lora_init();

    printf("Init: MPU=%d BMP=%d LORA=%d\n", ok_mpu, ok_bmp, ok_lora);
    cm4_send_line("RP2040 boot");

    // Startup beeps
    buzzer_beep(2000, 80);
    sleep_ms(40);
    buzzer_beep(2500, 80);

    const float SEA_LEVEL_PA = 101325.0f; // adjust after calibration

    uint32_t seq = 0;
    while (true) {
        led_blue(true);

        // Read battery
        float vbat = read_battery_voltage();

        // Read MPU
        mpu_raw_t mpu = {0};
        bool got_mpu = mpu6050_read_raw(&mpu);
        float mpu_temp = got_mpu ? mpu6050_temp_c(mpu.temp) : 0.0f;

        // Read BMP
        int32_t adcT=0, adcP=0;
        int32_t t_c_x100=0, p_pa=0;
        bool got_bmp = bmp280_read_raw(&adcT, &adcP);
        float alt_m = 0.0f;
        if (got_bmp) {
            bmp280_compensate(adcT, adcP, &t_c_x100, &p_pa);
            alt_m = altitude_m((float)p_pa, SEA_LEVEL_PA);
        }

        // TRIGGER from CM4 (example behavior)
        bool trig = gpio_get(CM4_TRIGGER_PIN);
        if (trig) {
            // Tell CM4 you saw trigger
            cm4_send_line("TRIGGER=1");
            led_red(true);
            buzzer_beep(3000, 50);
            led_red(false);
        }

        // Build a compact telemetry packet (binary)
        // Format:
        // [0..3]  seq
        // [4..7]  vbat_mV
        // [8..9]  bmp_temp_c_x100 (int16)
        // [10..13] pressure_pa (int32)
        // [14..17] alt_cm (int32)
        // [18..29] mpu ax ay az gx gy gz (int16 each) (12 bytes)
        // total 30 bytes
        uint8_t pkt[30];
        memset(pkt, 0, sizeof(pkt));

        uint32_t vbat_mV = (uint32_t)(vbat * 1000.0f);
        int32_t alt_cm = (int32_t)(alt_m * 100.0f);

        memcpy(&pkt[0],  &seq,     4);
        memcpy(&pkt[4],  &vbat_mV, 4);

        int16_t bmpT16 = (int16_t)t_c_x100;
        memcpy(&pkt[8],  &bmpT16,  2);
        memcpy(&pkt[10], &p_pa,    4);
        memcpy(&pkt[14], &alt_cm,  4);

        if (got_mpu) {
            memcpy(&pkt[18], &mpu.ax, 2);
            memcpy(&pkt[20], &mpu.ay, 2);
            memcpy(&pkt[22], &mpu.az, 2);
            memcpy(&pkt[24], &mpu.gx, 2);
            memcpy(&pkt[26], &mpu.gy, 2);
            memcpy(&pkt[28], &mpu.gz, 2);
        }

        // Send via LoRa
        bool sent = false;
        if (ok_lora) {
            sent = lora_send_packet(pkt, sizeof(pkt), 1000);
        }

        // Print debug
        printf("#%lu VBAT=%.3fV MPU=%d TMP=%.2fC BMP=%d BMP_T=%.2fC P=%ldPa ALT=%.1fm LORA=%d\n",
               (unsigned long)seq,
               vbat,
               got_mpu,
               mpu_temp,
               got_bmp,
               (float)t_c_x100 / 100.0f,
               (long)p_pa,
               alt_m,
               sent);

        // Also send a short ASCII status to CM4
        char line[128];
        snprintf(line, sizeof(line),
                 "SEQ=%lu VBAT=%.3f BMP_T=%.2f P=%ld ALT=%.1f",
                 (unsigned long)seq, vbat, (float)t_c_x100/100.0f, (long)p_pa, alt_m);
        cm4_send_line(line);

        led_blue(false);

        seq++;
        sleep_ms(200); // 5 Hz telemetry
    }
}
