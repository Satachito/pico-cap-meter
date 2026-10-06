// コンデンサ容量計 (Raspberry Pi Pico)
//
// 配線 (BreadBoard.txt):
//   GP22 ──[10kΩ]──┐
//                  ├── GP26 (ADC0) ── DUT(+) ──(−)── GND
//   GP21 ──[1MΩ ]──┘
//
// 低容量レンジ (1MΩ / PIO):
//   GP21 から充電し、GP26 のシュミット入力が HIGH になるまでを PIO で数える。
//   しきい値電圧は個体差があるので 'c' コマンドで ADC を使って校正する。
// 高容量レンジ (10kΩ / ADC):
//   GP22 から充電しながら ADC で 10〜80% の区間を連続サンプリングし、
//   y = -ln(1 - v/4096) = t/τ に重み付き最小二乗で直線を当てはめて τ を求める。
//   多数のサンプルを使うので RP2040 の ADC の DNL 段差 (E11) の影響が平均化される。
// 自動レンジ:
//   低容量レンジの方が精度が良いので、測れる (約 280nF 以下) ときは低容量レンジを使う。
//   前回のレンジから測り始める。低容量レンジがタイムアウトしたら高容量レンジに移り、
//   高容量レンジで 200nF 未満 (または小さすぎて測れない) なら低容量レンジに戻る。
//   毎回低容量レンジから試すと、タイムアウトまでの充電 (1µF で約 0.6V) による誘電吸収で
//   続く高容量レンジの値がずれる (1µF のセラミックで +0.4%) ので、それを避けている。
//
// USB シリアルのコマンド:
//   a: 自動レンジ  l: 低容量レンジ固定  h: 高容量レンジ固定
//   z: ゼロ点校正 (DUT を外して実行)
//   c: しきい値校正 (1nF〜1µF 程度のコンデンサを挿して実行)
//   x: レンジ間校正 (30〜250nF のコンデンサを挿して実行。高容量レンジを低容量レンジに合わせる)
//   i: 校正値表示
//   R <1MΩ側 [Ω]> <10kΩ側 [Ω]>: 抵抗の実測値を設定 (例 "R 998000 9870"。レンジ間校正はやり直しになる)
//   K <nF>: 基準コンデンサ校正 (値の分かっているコンデンサを挿して実行。抵抗値を逆算する。
//           30nF 以上なら 10kΩ側も決まり、レンジ間校正も済む。先に z と c が必要)
// 'c' と 'z' と 'x' の結果と抵抗値はフラッシュに保存され、起動時に読み込まれる。
//
// Mac アプリ (mac/) 向けに、人間向けの表示とは別に '#' で始まる機械可読な行も出す:
//   #state,<idle|measure_lo|measure_hi|discharge|cal_zero|cal_threshold|cal_cross|cal_reference>  (変わったときだけ)
//   #v,<電圧 [V]>                            0.25 秒以上かかる充電・放電の途中経過
//   #meas,<lo|hi>,<容量 [F] | nan>           nan は測定範囲外
//   #mode,<auto|lo|hi>
//   #cal,<しきい値/VDD>,<浮遊容量 [pF]>,<高容量レンジ補正>,<済んだ校正: 1=ゼロ点 2=しきい値 4=レンジ間 8=基準>,
//        <1MΩ側 [Ω]>,<10kΩ側 [Ω]>
//   #done,<z|c|x|R|K>,<ok|fail|abort>,<メッセージ>

#include <algorithm>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/flash.h"
#include "hardware/pio.h"
#include "pico/flash.h"

#include "cap_timer.pio.h"

namespace {

constexpr uint PIN_CHARGE_LO = 21;  // 1MΩ
constexpr uint PIN_CHARGE_HI = 22;  // 10kΩ
constexpr uint PIN_SENSE = 26;      // ADC0
constexpr uint ADC_INPUT = 0;

// 抵抗値の初期値 (公称値)。テスターで測った値を 'R' コマンド (Mac アプリの「抵抗値」) で入れると、
// フラッシュに保存されて精度が上がる
constexpr double R_LO_DEFAULT = 1e6;
constexpr double R_HI_DEFAULT = 10e3;
// 受け付ける範囲 (公称値から大きく外れる値は入力ミスとみなす)
constexpr double R_LO_MIN = 500e3, R_LO_MAX = 2e6;
constexpr double R_HI_MIN = 5e3, R_HI_MAX = 20e3;

constexpr double LO_TIMEOUT_S = 0.2;
constexpr double AUTO_BACK_TO_LO_F = 200e-9;  // 自動レンジで高容量から低容量に戻る容量
constexpr double HI_TIMEOUT_S = 20.0;
constexpr int LO_AVERAGE = 8;
constexpr int ZERO_AVERAGE = 32;
constexpr double STRAY_MAX_F = 200e-12;  // ゼロ点校正でこれを超えたら DUT が挿さっているとみなす
// レンジ間校正: 両方のレンジで測れる容量の範囲と回数
constexpr double CROSS_MIN_F = 30e-9;    // これ未満は高容量レンジの点が少なく不正確
constexpr double CROSS_MAX_F = 250e-9;   // これを超えると低容量レンジがタイムアウトする
constexpr int CROSS_LO_COUNT = 4;
// 基準コンデンサ校正: 受け付ける値と、挿したものとのずれの許容
constexpr double REFERENCE_MIN_F = 1e-9;
constexpr double REFERENCE_MAX_F = 250e-9;
constexpr double REFERENCE_TOLERANCE = 0.2;
constexpr int CROSS_HI_WARMUP = 10;      // 誘電吸収が落ち着くまで空測定する
constexpr int CROSS_HI_COUNT = 10;
constexpr uint32_t MEASURE_INTERVAL_MS = 300;

constexpr double ADC_FULL = 4096.0;
constexpr uint16_t HI_FIT_MIN = 410;        // 10%: これより下はオフセットの影響が大きい
constexpr uint16_t HI_FIT_MAX = 3277;       // 80%: これより上は ln の傾きが急でノイズが増える
constexpr int HI_FIT_MIN_SAMPLES = 10;
// 放電完了の判定: DISCHARGE_LOW_COUNT を下回ってから、平均値が下がらなくなるまで待つ
constexpr int DISCHARGE_AVERAGE = 32;
constexpr double DISCHARGE_LOW_COUNT = 100;  // 約 80mV。0V の読み値 (27〜45) より十分上
constexpr uint64_t DISCHARGE_MIN_SETTLE_US = 300;
constexpr uint64_t DISCHARGE_TIMEOUT_US = 60'000'000;

enum class Range { Auto, Lo, Hi };

PIO pio = pio0;
uint sm;
uint sm_offset;

double vth_ratio = 0.5;  // シュミット立ち上がりしきい値 / VDD ('c' で校正)
// 浮遊容量ぶんの充電時間 [s] ('z' で校正)。容量ではなく時間で持つので、
// 後から 'c' でしきい値を変えても差し引く量がずれない。
double stray_t = 0.0;
double hi_gain = 1.0;    // 高容量レンジへの補正係数 ('x' で校正)
double r_lo = R_LO_DEFAULT;  // GP21 の抵抗 [Ω]
double r_hi = R_HI_DEFAULT;  // GP22 の抵抗 [Ω]

// 済んだ校正の印。値が初期値と同じでも校正済みと分かるように、値とは別に持つ
constexpr uint32_t CAL_DONE_ZERO = 1u << 0;
constexpr uint32_t CAL_DONE_THRESHOLD = 1u << 1;
constexpr uint32_t CAL_DONE_CROSS = 1u << 2;
constexpr uint32_t CAL_DONE_REFERENCE = 1u << 3;  // 抵抗値を基準コンデンサから決めた ('R' で入れたら外れる)
uint32_t cal_done = 0;

// 校正値はフラッシュ最後のセクタに保存する (UF2 を書き込み直しても消えない)
constexpr uint32_t CAL_FLASH_OFFSET = PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE;
// 保存形式を変えたら番号を上げる (古い形式は読まずに初期値から校正し直す)
constexpr uint32_t CAL_MAGIC = 0x344C4143;  // "CAL4"

struct Calibration {
    uint32_t magic;
    uint32_t done;  // CAL_DONE_*
    double vth_ratio;
    double stray_t;
    double hi_gain;
    double r_lo;
    double r_hi;
    uint32_t checksum;
    uint32_t reserved2;
};
static_assert(sizeof(Calibration) <= FLASH_PAGE_SIZE);

// FNV-1a (checksum より前のバイトが対象)
uint32_t calibration_checksum(const Calibration &cal) {
    const auto *bytes = reinterpret_cast<const uint8_t *>(&cal);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < offsetof(Calibration, checksum); i++) {
        h = (h ^ bytes[i]) * 16777619u;
    }
    return h;
}

bool plausible_vth(double r) { return r > 0.1 && r < 0.9; }
// 抵抗の誤差 (1% 程度) なら十分収まる範囲。外れるのは接触不良などで校正に失敗したとき
bool plausible_hi_gain(double g) { return g > 0.95 && g < 1.05; }
bool plausible_r_lo(double r) { return r >= R_LO_MIN && r <= R_LO_MAX; }
bool plausible_r_hi(double r) { return r >= R_HI_MIN && r <= R_HI_MAX; }

void load_calibration() {
    const auto &cal = *reinterpret_cast<const Calibration *>(XIP_BASE + CAL_FLASH_OFFSET);
    if (cal.magic == CAL_MAGIC && cal.checksum == calibration_checksum(cal) && plausible_vth(cal.vth_ratio)) {
        vth_ratio = cal.vth_ratio;
        stray_t = cal.stray_t;
        if (plausible_r_lo(cal.r_lo)) r_lo = cal.r_lo;
        if (plausible_r_hi(cal.r_hi)) r_hi = cal.r_hi;
        // 補正係数だけがおかしいときは、それだけ初期値に戻す
        if (plausible_hi_gain(cal.hi_gain)) hi_gain = cal.hi_gain;
        cal_done = cal.done;
        if (hi_gain != cal.hi_gain) cal_done &= ~CAL_DONE_CROSS;
    }
}

void program_calibration_page(void *page) {
    flash_range_erase(CAL_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(CAL_FLASH_OFFSET, static_cast<const uint8_t *>(page), FLASH_PAGE_SIZE);
}

void save_calibration() {
    Calibration cal = {};
    cal.magic = CAL_MAGIC;
    cal.vth_ratio = vth_ratio;
    cal.stray_t = stray_t;
    cal.hi_gain = hi_gain;
    cal.r_lo = r_lo;
    cal.r_hi = r_hi;
    cal.done = cal_done;
    cal.checksum = calibration_checksum(cal);

    alignas(4) uint8_t page[FLASH_PAGE_SIZE];
    std::memset(page, 0xFF, sizeof(page));
    std::memcpy(page, &cal, sizeof(cal));
    // フラッシュ書き込み中は XIP が止まるので、割り込みを止めて RAM 上の処理だけで行う
    const int result = flash_safe_execute(program_calibration_page, page, UINT32_MAX);
    if (result == PICO_OK) {
        printf("校正値をフラッシュに保存しました\n");
    } else {
        printf("フラッシュへの保存に失敗しました (%d)\n", result);
    }
}

// 長い待ちの途中でも USB シリアルのコマンドを拾う。拾ったら測定を中断して main で処理する。
constexpr uint64_t COMMAND_POLL_US = 10'000;
constexpr char COMMANDS[] = "alhzcxiRK?";

int pending_command = PICO_ERROR_TIMEOUT;
uint64_t last_command_poll = 0;

int read_command() {
    while (true) {
        const int c = getchar_timeout_us(0);
        // 改行など、コマンド以外の文字は読み捨てる
        if (c == PICO_ERROR_TIMEOUT || (c != 0 && std::strchr(COMMANDS, c))) return c;
    }
}

// 待ちループの中から呼ぶ。コマンドが届いていれば true (呼び出し側は中断する)
bool command_pending() {
    if (pending_command != PICO_ERROR_TIMEOUT) return true;
    const uint64_t now = time_us_64();
    if (now - last_command_poll < COMMAND_POLL_US) return false;
    last_command_poll = now;
    pending_command = read_command();
    return pending_command != PICO_ERROR_TIMEOUT;
}

int take_command() {
    const int c = pending_command != PICO_ERROR_TIMEOUT ? pending_command : read_command();
    pending_command = PICO_ERROR_TIMEOUT;
    return c;
}

const char *reported_state = "";

void report_state(const char *state) {
    if (std::strcmp(state, reported_state) == 0) return;
    reported_state = state;
    printf("#state,%s\n", state);
}

constexpr uint64_t VOLTAGE_REPORT_US = 250'000;
uint64_t last_voltage_report = 0;

// start から 0.25 秒以上たっている長い充電・放電のときだけ、0.25 秒ごとに電圧を出す
void report_voltage(double counts, uint64_t start) {
    const uint64_t now = time_us_64();
    if (now - start < VOLTAGE_REPORT_US || now - last_voltage_report < VOLTAGE_REPORT_US) return;
    last_voltage_report = now;
    printf("#v,%.3f\n", counts / ADC_FULL * 3.3);
}

void drive(uint pin, bool level) {
    gpio_set_function(pin, GPIO_FUNC_SIO);
    gpio_put(pin, level);
    gpio_set_dir(pin, GPIO_OUT);
}

// Hi-Z にする。入力バッファも切る: RP2350 A2 (Pico 2) のエラッタ E9 で、入力バッファが有効な
// Hi-Z のピンは 2V 台に張り付こうとして電流を流し出し、抵抗越しにノードを充電してしまう
// (Pico 2 で低容量レンジが約 1% 小さく出ていた)
void release(uint pin) {
    gpio_set_function(pin, GPIO_FUNC_SIO);
    gpio_set_dir(pin, GPIO_IN);
    gpio_set_input_enabled(pin, false);
}

// ADC 測定時はデジタル入力バッファを切ってリークを減らす
void sense_digital(bool enabled) {
    gpio_set_input_enabled(PIN_SENSE, enabled);
}

double adc_average(int n) {
    uint32_t sum = 0;
    for (int i = 0; i < n; i++) sum += adc_read();
    return double(sum) / n;
}

// 両方の抵抗から GND に落とし、ADC の平均値が下がらなくなったら放電完了とみなして
// そのときの ADC 値 (0V の読み値) を返す。
//
// 大容量では 1 回の判定時間に 1 カウントも下がらないので、まず DISCHARGE_LOW_COUNT を
// 下回るまで待ち、そこから「それまでにかかった時間の 1/4」下がらなければ完了とする。
// 指数減衰なので、その時点の残りは 1 カウント程度になっている。
//
// 0V の読み値は絶対値では判定しない。RP2040 の ADC のオフセット (実機で約 27) に加えて、
// 読むたびにサンプリング容量が入力から電荷を出し入れするため、10kΩ 越しの浮遊容量しか
// ないノードでは 0V でも持ち上がって見える (実機で約 45)。DUT が挿さっていれば持ち上がらない。
//
// コマンドで中断されたときは、次に呼ばれたときに続きから再開する。

// 放電の進み具合。中断しても経過時間を失わないように関数の外に持つ
bool discharged = false;
uint64_t discharge_start = 0;  // 0: 放電を始めていない

// 充電を始めるときに呼ぶ
void mark_charging() {
    discharged = false;
    discharge_start = 0;
}

std::optional<double> discharge() {
    drive(PIN_CHARGE_HI, false);
    drive(PIN_CHARGE_LO, false);
    double lowest = adc_average(DISCHARGE_AVERAGE);
    // 放電しきった後に充電していなければ待たない (DUT を差し替えた場合に備えて電圧は確かめる)
    if (discharged && lowest < DISCHARGE_LOW_COUNT) return lowest;

    discharged = false;
    if (discharge_start == 0) discharge_start = time_us_64();
    const uint64_t start = discharge_start;
    uint64_t last_drop = time_us_64();
    while (true) {
        const uint64_t now = time_us_64();
        const double v = adc_average(DISCHARGE_AVERAGE);
        report_voltage(v, start);
        if (v < lowest - 1.0 || v > DISCHARGE_LOW_COUNT) {
            lowest = std::min(lowest, v);
            last_drop = now;
        } else if (now - last_drop > std::max(DISCHARGE_MIN_SETTLE_US, (last_drop - start) / 4)) {
            break;
        }
        if (now - start > DISCHARGE_TIMEOUT_US || command_pending()) return std::nullopt;
    }
    discharged = true;
    return adc_average(DISCHARGE_AVERAGE);
}

// 1MΩ で充電してしきい値を超えるまでの時間 [s]
// 測定途中のステートマシンを止め、充電ピンを LOW にしてプログラム先頭 (pull 待ち) に戻す
void abort_charge_timer() {
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_restart(pio, sm);
    pio_sm_exec(pio, sm, pio_encode_set(pio_pins, 0));
    pio_sm_exec(pio, sm, pio_encode_jmp(sm_offset));
    pio_sm_set_enabled(pio, sm, true);
}

std::optional<double> charge_time_lo() {
    sense_digital(true);
    if (!discharge()) return std::nullopt;
    release(PIN_CHARGE_HI);

    const uint32_t f_sys = clock_get_hz(clk_sys);
    const auto timeout = static_cast<uint32_t>(LO_TIMEOUT_S * f_sys / 2);

    mark_charging();
    pio_gpio_init(pio, PIN_CHARGE_LO);
    pio_sm_put_blocking(pio, sm, timeout);
    while (pio_sm_is_rx_fifo_empty(pio, sm)) {
        if (command_pending()) {
            abort_charge_timer();
            drive(PIN_CHARGE_LO, false);
            return std::nullopt;
        }
    }
    const uint32_t x = pio_sm_get(pio, sm);
    drive(PIN_CHARGE_LO, false);

    // タイムアウト、または開始時点ですでにしきい値を超えていた (放電しきれていない)
    if (x == 0xFFFFFFFFu || x == timeout) return std::nullopt;
    return 2.0 * (timeout - x) / f_sys;
}

std::optional<double> average_charge_time_lo(int n) {
    double sum = 0;
    for (int i = 0; i < n; i++) {
        const auto t = charge_time_lo();
        if (!t) return std::nullopt;
        sum += *t;
    }
    return sum / n;
}

double charge_time_to_capacitance_lo(double t) {
    return t / (r_lo * -std::log(1.0 - vth_ratio));
}

std::optional<double> measure_lo() {
    const auto t = average_charge_time_lo(LO_AVERAGE);
    if (!t) return std::nullopt;
    return charge_time_to_capacitance_lo(*t - stray_t);
}

// RP2040-E11: 512, 1536, 2560, 3584 のコードは幅が異常に広いので当てはめから除く
bool is_dnl_spike(uint16_t v) {
    const uint16_t d = v & 0x3FF;
    return d >= 511 && d <= 513;
}

// y = -ln(1 - v/4096) の表 (サンプルごとに log を計算すると遅いので事前に作る)
float charge_log_table[4096];

void init_charge_log_table() {
    for (int v = 0; v < 4096; v++) {
        charge_log_table[v] = static_cast<float>(-std::log(1.0 - v / ADC_FULL));
    }
}

std::optional<double> measure_hi_raw() {
    sense_digital(false);
    const auto zero_reading = discharge();
    if (!zero_reading) return std::nullopt;
    const int zero = static_cast<int>(std::lround(*zero_reading));
    release(PIN_CHARGE_LO);

    // y = a·t + b の重み付き最小二乗。v の誤差は y では 1/(4096 - v) 倍に
    // 拡大されるので、重みは (1 - v/4096)^2 にする。
    double sw = 0, swt = 0, swy = 0, swtt = 0, swty = 0;
    int n = 0;
    const uint64_t t0 = time_us_64();
    mark_charging();
    gpio_put(PIN_CHARGE_HI, true);
    while (true) {
        const double t = double(time_us_64() - t0);
        const uint16_t raw = adc_read();
        report_voltage(raw, t0);
        const int v = raw - zero;
        if (v >= HI_FIT_MAX) break;
        if (t > HI_TIMEOUT_S * 1e6 || command_pending()) {
            gpio_put(PIN_CHARGE_HI, false);
            return std::nullopt;
        }
        if (v < HI_FIT_MIN || is_dnl_spike(raw)) continue;

        const double r = 1.0 - v / ADC_FULL;
        const double w = r * r;
        const double y = charge_log_table[v];
        sw += w;
        swt += w * t;
        swy += w * y;
        swtt += w * t * t;
        swty += w * t * y;
        n++;
    }
    gpio_put(PIN_CHARGE_HI, false);

    // 容量が小さすぎて点が足りないときは測定不能 (低容量レンジで測る)
    if (n < HI_FIT_MIN_SAMPLES) return std::nullopt;
    const double denom = sw * swtt - swt * swt;
    if (denom <= 0) return std::nullopt;
    const double a = (sw * swty - swt * swy) / denom;  // 1/τ [1/µs]
    if (a <= 0) return std::nullopt;
    return 1e-6 / a / r_hi;
}

void report_calibration() {
    printf("#cal,%.4f,%.2f,%.4f,%lu,%.0f,%.0f\n", vth_ratio, charge_time_to_capacitance_lo(stray_t) * 1e12, hi_gain,
           static_cast<unsigned long>(cal_done), r_lo, r_hi);
}

// 校正の結果を人間向けと '#done' の両方で出す
void report_done(char command, const char *result, const char *format, ...) {
    char message[160];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    printf("%s\n#done,%c,%s,%s\n", message, command, result, message);
    if (std::strcmp(result, "ok") == 0) report_calibration();
}

// 1MΩ でゆっくり充電し、シュミット入力が反転した瞬間の ADC 値を読む
void calibrate_threshold() {
    constexpr int N = 5;
    sense_digital(true);
    double sum = 0;
    for (int i = 0; i < N; i++) {
        const auto zero = discharge();
        if (!zero) {
            if (command_pending()) {
                report_done('c', "abort", "しきい値校正を中断しました");
            } else {
                report_done('c', "fail", "放電できません");
            }
            return;
        }
        release(PIN_CHARGE_HI);
        const uint64_t t0 = time_us_64();
        mark_charging();
        drive(PIN_CHARGE_LO, true);
        uint16_t before = 0;
        while (!gpio_get(PIN_SENSE)) {
            before = adc_read();
            report_voltage(before, t0);
            if (time_us_64() - t0 > 2'000'000) {
                drive(PIN_CHARGE_LO, false);
                report_done('c', "fail", "容量が大きすぎます (1µF 以下を挿してください)");
                return;
            }
            if (command_pending()) {
                drive(PIN_CHARGE_LO, false);
                report_done('c', "abort", "しきい値校正を中断しました");
                return;
            }
        }
        const uint16_t after = adc_read();
        const uint64_t elapsed = time_us_64() - t0;
        drive(PIN_CHARGE_LO, false);
        if (elapsed < 1000) {
            report_done('c', "fail", "容量が小さすぎます (1nF 以上を挿してください)");
            return;
        }
        sum += ((before + after) / 2.0 - *zero) / ADC_FULL;
    }
    vth_ratio = sum / N;
    cal_done |= CAL_DONE_THRESHOLD;
    save_calibration();
    report_done('c', "ok", "しきい値: %.4f × VDD (%.3f V @3.3V)", vth_ratio, vth_ratio * 3.3);
}

std::optional<double> measure_hi() {
    const auto c = measure_hi_raw();
    if (!c) return std::nullopt;
    return *c * hi_gain;
}

// コマンドが来たら false (中断)
bool wait_ms_or_command(uint32_t ms) {
    const uint64_t until = time_us_64() + ms * 1000ull;
    while (time_us_64() < until) {
        if (command_pending()) return false;
    }
    return true;
}

// 校正用に低容量レンジで CROSS_LO_COUNT 回測って平均する。測れなければ理由を報告して nullopt
std::optional<double> average_lo_for_calibration(char command, const char *name, const char *hint) {
    double sum = 0;
    for (int i = 0; i < CROSS_LO_COUNT; i++) {
        const auto c = measure_lo();
        if (!c) {
            if (command_pending()) {
                report_done(command, "abort", "%sを中断しました", name);
            } else {
                report_done(command, "fail", "低容量レンジで測れません (%s)", hint);
            }
            return std::nullopt;
        }
        sum += *c;
    }
    return sum / CROSS_LO_COUNT;
}

// 校正用に高容量レンジ (補正前) で測って平均する。普段の測定と同じ間隔で充放電を繰り返し、
// 誘電吸収が落ち着いてから平均する。測れなければ理由を報告して nullopt
std::optional<double> average_hi_raw_for_calibration(char command, const char *name) {
    double sum = 0;
    for (int i = 0; i < CROSS_HI_WARMUP + CROSS_HI_COUNT; i++) {
        const auto c = measure_hi_raw();
        if (!c) {
            if (command_pending()) {
                report_done(command, "abort", "%sを中断しました", name);
            } else {
                report_done(command, "fail", "高容量レンジで測れません");
            }
            return std::nullopt;
        }
        if (i >= CROSS_HI_WARMUP) sum += *c;
        discharge();
        if (!wait_ms_or_command(MEASURE_INTERVAL_MS)) {
            report_done(command, "abort", "%sを中断しました", name);
            return std::nullopt;
        }
    }
    return sum / CROSS_HI_COUNT;
}

// 同じコンデンサを両方のレンジで測り、高容量レンジの値が低容量レンジに一致するよう hi_gain を決める
void calibrate_cross() {
    printf("レンジ間校正中 (約 %u 秒)...\n", unsigned((CROSS_HI_WARMUP + CROSS_HI_COUNT) * MEASURE_INTERVAL_MS / 1000 + 2));

    const auto c_lo = average_lo_for_calibration('x', "レンジ間校正", "30〜250nF を挿してください");
    if (!c_lo) return;
    if (*c_lo < CROSS_MIN_F || *c_lo > CROSS_MAX_F) {
        report_done('x', "fail", "容量が範囲外です: %.3f nF (30〜250nF を挿してください)", *c_lo * 1e9);
        return;
    }
    const auto c_hi = average_hi_raw_for_calibration('x', "レンジ間校正");
    if (!c_hi) return;

    const double gain = *c_lo / *c_hi;
    if (!plausible_hi_gain(gain)) {
        report_done('x', "fail", "補正係数が大きすぎます: %.4f (低容量 %.3f nF / 高容量 %.3f nF)", gain, *c_lo * 1e9,
                    *c_hi * 1e9);
        return;
    }
    hi_gain = gain;
    cal_done |= CAL_DONE_CROSS;
    save_calibration();
    report_done('x', "ok", "低容量 %.3f nF / 高容量 %.3f nF -> 補正係数 %.4f", *c_lo * 1e9, *c_hi * 1e9, hi_gain);
}

void calibrate_zero() {
    const auto t = average_charge_time_lo(ZERO_AVERAGE);
    if (!t) {
        if (command_pending()) {
            report_done('z', "abort", "ゼロ点校正を中断しました");
        } else {
            report_done('z', "fail", "ゼロ点校正に失敗しました (DUT を外してください)");
        }
        return;
    }
    const double stray = charge_time_to_capacitance_lo(*t);
    if (stray > STRAY_MAX_F) {
        report_done('z', "fail", "浮遊容量が大きすぎます: %.2f pF (DUT を外してください)", stray * 1e12);
        return;
    }
    stray_t = *t;
    cal_done |= CAL_DONE_ZERO;
    save_calibration();
    report_done('z', "ok", "浮遊容量: %.2f pF", stray * 1e12);
}

// コマンド文字に続く引数を 1 行読む
void read_argument_line(char *line, size_t size) {
    size_t n = 0;
    while (n < size - 1) {
        const int c = getchar_timeout_us(200'000);
        if (c == PICO_ERROR_TIMEOUT || c == '\r' || c == '\n') break;
        line[n++] = static_cast<char>(c);
    }
    line[n] = '\0';
}

// 'R' に続く 1 行 (" 998000 9870") を読んで抵抗値を設定する
void set_resistors() {
    char line[64];
    read_argument_line(line, sizeof(line));
    char *end = nullptr;
    const double lo = std::strtod(line, &end);
    const double hi = std::strtod(end, &end);
    if (!plausible_r_lo(lo) || !plausible_r_hi(hi)) {
        report_done('R', "fail", "抵抗値が範囲外です (1MΩ側 %.0f〜%.0f kΩ / 10kΩ側 %.0f〜%.0f kΩ)", R_LO_MIN / 1e3,
                    R_LO_MAX / 1e3, R_HI_MIN / 1e3, R_HI_MAX / 1e3);
        return;
    }
    r_lo = lo;
    r_hi = hi;
    // 補正係数は古い抵抗値で決めたものなので、レンジ間校正はやり直しにする
    hi_gain = 1.0;
    cal_done &= ~(CAL_DONE_CROSS | CAL_DONE_REFERENCE);
    save_calibration();
    report_done('R', "ok", "抵抗値: %.1f kΩ / %.3f kΩ (レンジ間校正をやり直してください)", r_lo / 1e3, r_hi / 1e3);
}

// 'K' に続く値 [nF] の基準コンデンサを測り、測定値がその値になるよう抵抗値を逆算する。
// 低容量レンジ (容量 ∝ 1 / r_lo) から r_lo を、30nF 以上なら高容量レンジから r_hi も決める
void calibrate_reference() {
    char line[32];
    read_argument_line(line, sizeof(line));
    const double ref = std::strtod(line, nullptr) * 1e-9;
    if (!(cal_done & CAL_DONE_ZERO) || !(cal_done & CAL_DONE_THRESHOLD)) {
        report_done('K', "fail", "先にゼロ点校正としきい値校正をしてください");
        return;
    }
    if (ref < REFERENCE_MIN_F || ref > REFERENCE_MAX_F) {
        report_done('K', "fail", "基準コンデンサは %.0f〜%.0f nF にしてください", REFERENCE_MIN_F * 1e9,
                    REFERENCE_MAX_F * 1e9);
        return;
    }
    printf("基準コンデンサ校正中...\n");

    const auto c_lo = average_lo_for_calibration('K', "基準コンデンサ校正", "基準コンデンサを挿してください");
    if (!c_lo) return;
    if (std::fabs(*c_lo / ref - 1) > REFERENCE_TOLERANCE) {
        report_done('K', "fail", "挿してあるコンデンサ (%.3f nF) と入力した値 (%.3f nF) が合いません", *c_lo * 1e9,
                    ref * 1e9);
        return;
    }
    const double new_r_lo = r_lo * *c_lo / ref;

    double new_r_hi = r_hi, new_gain = hi_gain;
    const bool with_hi = ref >= CROSS_MIN_F;
    if (with_hi) {
        const auto c_hi = average_hi_raw_for_calibration('K', "基準コンデンサ校正");
        if (!c_hi) return;
        new_r_hi = r_hi * *c_hi / ref;
        new_gain = 1.0;
    } else {
        // 高容量レンジは測れないので、今の「低容量レンジとの揃い方」を保つ
        new_gain = hi_gain * ref / *c_lo;
    }
    if (!plausible_r_lo(new_r_lo) || !plausible_r_hi(new_r_hi) || !plausible_hi_gain(new_gain)) {
        report_done('K', "fail", "計算した抵抗値が範囲外です (1MΩ側 %.1f kΩ / 10kΩ側 %.3f kΩ)", new_r_lo / 1e3,
                    new_r_hi / 1e3);
        return;
    }
    r_lo = new_r_lo;
    r_hi = new_r_hi;
    hi_gain = new_gain;
    cal_done |= CAL_DONE_REFERENCE | (with_hi ? CAL_DONE_CROSS : 0);
    save_calibration();
    report_done('K', "ok", "基準 %.3f nF: 抵抗値 %.1f kΩ / %.3f kΩ%s", ref * 1e9, r_lo / 1e3, r_hi / 1e3,
                with_hi ? "" : " (10kΩ側は 30nF 以上の基準で決まります)");
}

void print_capacitance(std::optional<double> c, const char *range, const char *range_key) {
    if (c) {
        printf("#meas,%s,%.6e\n", range_key, *c);
    } else {
        printf("#meas,%s,nan\n", range_key);
    }
    if (!c) {
        printf("[%s] 測定範囲外\n", range);
    } else if (std::fabs(*c) < 1e-9) {
        printf("[%s] %.2f pF\n", range, *c * 1e12);
    } else if (std::fabs(*c) < 1e-6) {
        printf("[%s] %.3f nF\n", range, *c * 1e9);
    } else {
        printf("[%s] %.3f uF\n", range, *c * 1e6);
    }
}

void print_status() {
    printf("しきい値: %.4f × VDD / 浮遊容量: %.2f pF / 高容量レンジ補正: %.4f / 抵抗: %.1f kΩ, %.3f kΩ\n", vth_ratio,
           charge_time_to_capacitance_lo(stray_t) * 1e12, hi_gain, r_lo / 1e3, r_hi / 1e3);
    report_calibration();
}

const char *range_key(Range range) {
    switch (range) {
        case Range::Lo: return "lo";
        case Range::Hi: return "hi";
        default: return "auto";
    }
}

void print_help() {
    printf("a:自動 l:低容量 h:高容量 z:ゼロ点校正 c:しきい値校正 x:レンジ間校正 i:校正値表示\n");
}

}  // namespace

int main() {
    stdio_init_all();

#ifdef PICO_SMPS_MODE_PIN
    // Pico の電源 IC を PWM モードにしてリップルを減らし、ADC のノイズを抑える
    gpio_init(PICO_SMPS_MODE_PIN);
    gpio_put(PICO_SMPS_MODE_PIN, true);
    gpio_set_dir(PICO_SMPS_MODE_PIN, GPIO_OUT);
#endif

    adc_init();
    adc_gpio_init(PIN_SENSE);
    adc_select_input(ADC_INPUT);
    init_charge_log_table();
    load_calibration();

    for (uint pin : {PIN_CHARGE_LO, PIN_CHARGE_HI}) {
        gpio_init(pin);
        gpio_disable_pulls(pin);  // リセット直後はプルダウンが有効になっている
    }

    sm_offset = pio_add_program(pio, &cap_timer_program);
    sm = pio_claim_unused_sm(pio, true);
    cap_timer_program_init(pio, sm, sm_offset, PIN_CHARGE_LO, PIN_SENSE);

    discharge();

    Range range = Range::Auto;
    bool auto_on_hi = false;  // 自動レンジで今使っているレンジ
    print_help();
    while (true) {
        const int command = take_command();
        switch (command) {
            case 'a': range = Range::Auto; printf("自動レンジ\n"); break;
            case 'l': range = Range::Lo; printf("低容量レンジ\n"); break;
            case 'h': range = Range::Hi; printf("高容量レンジ\n"); break;
            case 'z': report_state("cal_zero"); calibrate_zero(); break;
            case 'c': report_state("cal_threshold"); calibrate_threshold(); break;
            case 'x': report_state("cal_cross"); calibrate_cross(); break;
            case 'i': print_status(); break;
            case 'R': set_resistors(); break;
            case 'K': report_state("cal_reference"); calibrate_reference(); break;
            case '?': print_help(); break;
            default: break;
        }
        if (command == 'a' || command == 'l' || command == 'h' || command == 'i') {
            printf("#mode,%s\n", range_key(range));
        }

        constexpr const char *LO_NAME = "1M/PIO";
        constexpr const char *HI_NAME = "10k/ADC";
        std::optional<double> c;
        const char *range_name;
        if (range == Range::Hi || (range == Range::Auto && auto_on_hi)) {
            report_state("measure_hi");
            c = measure_hi();
            range_name = HI_NAME;
            if (range == Range::Auto && !command_pending()) {
                if (!c) {
                    // 小さい容量に差し替えられた (点が足りずに測れない)
                    report_state("measure_lo");
                    c = measure_lo();
                    range_name = LO_NAME;
                    auto_on_hi = !c;
                } else if (*c < AUTO_BACK_TO_LO_F) {
                    auto_on_hi = false;
                }
            }
        } else {
            report_state("measure_lo");
            c = measure_lo();
            range_name = LO_NAME;
            if (range == Range::Auto && !c && !command_pending()) {
                report_state("measure_hi");
                c = measure_hi();
                range_name = HI_NAME;
                auto_on_hi = true;
            }
        }
        // コマンドで中断された測定の結果は捨てて、すぐにコマンドを処理する
        if (command_pending()) continue;
        print_capacitance(c, range_name, range_name == HI_NAME ? "hi" : "lo");

        report_state("discharge");
        discharge();
        report_state("idle");
        wait_ms_or_command(MEASURE_INTERVAL_MS);
    }
}
