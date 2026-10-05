import Foundation
import Observation

/// ファームウェアが '#state' で知らせる、今やっていること
enum Activity: String {
    case idle
    case measureLo = "measure_lo"
    case measureHi = "measure_hi"
    case discharge
    case calZero = "cal_zero"
    case calThreshold = "cal_threshold"
    case calCross = "cal_cross"

    var isCalibration: Bool { self == .calZero || self == .calThreshold || self == .calCross }
}

enum RangeMode: String, CaseIterable, Identifiable {
    case auto, lo, hi
    var id: String { rawValue }
    var command: String {
        switch self {
        case .auto: return "a"
        case .lo: return "l"
        case .hi: return "h"
        }
    }
    var label: String {
        switch self {
        case .auto: return "自動"
        case .lo: return "低容量 (1MΩ)"
        case .hi: return "高容量 (10kΩ)"
        }
    }
}

struct Reading: Identifiable {
    let id = UUID()
    let date: Date
    let farads: Double?   // nil: 測定範囲外
    let range: String     // "lo" / "hi"

    var rangeLabel: String { range == "hi" ? "高容量レンジ (10kΩ / ADC)" : "低容量レンジ (1MΩ / PIO)" }
}

/// 3 種類の校正。ファームウェアの 'z' / 'c' / 'x' に対応する
enum CalibrationStep: String, CaseIterable, Identifiable {
    case zero = "z"
    case threshold = "c"
    case cross = "x"
    var id: String { rawValue }

    var number: String {
        switch self {
        case .zero: return "①"
        case .threshold: return "②"
        case .cross: return "③"
        }
    }
    var title: String {
        switch self {
        case .zero: return "ゼロ点校正"
        case .threshold: return "しきい値校正"
        case .cross: return "レンジ間校正"
        }
    }
    var purpose: String {
        switch self {
        case .zero: return "ブレッドボードや配線の浮遊容量を測って、測定値から差し引きます。"
        case .threshold: return "GP26 のデジタル入力が HIGH に変わる電圧を測ります。低容量レンジの精度に直結します。"
        case .cross: return "同じコンデンサを両方のレンジで測り、高容量レンジを低容量レンジに合わせます。"
        }
    }
    var requirement: String {
        switch self {
        case .zero: return "何も挿さない"
        case .threshold: return "1nF〜1µF のコンデンサ (103 / 104 など)"
        case .cross: return "30〜250nF のフィルムコンデンサ (104 など)"
        }
    }
    var duration: String {
        switch self {
        case .zero: return "1 秒ほど"
        case .threshold: return "1nF なら一瞬、1µF なら数秒"
        case .cross: return "約 8 秒"
        }
    }
    var activity: Activity {
        switch self {
        case .zero: return .calZero
        case .threshold: return .calThreshold
        case .cross: return .calCross
        }
    }
}

struct CalibrationValues {
    var vth = 0.5         // しきい値 / VDD
    var strayPF = 0.0     // 浮遊容量 [pF]
    var hiGain = 1.0      // 高容量レンジの補正係数
    /// ファームウェアが知らせる済んだ校正の印 (1=ゼロ点 2=しきい値 4=レンジ間)。古いファームウェアでは nil
    var doneFlags: Int?

    func isDone(_ step: CalibrationStep) -> Bool {
        if let doneFlags {
            switch step {
            case .zero: return doneFlags & 1 != 0
            case .threshold: return doneFlags & 2 != 0
            case .cross: return doneFlags & 4 != 0
            }
        }
        // 印がなければ、ファームウェアの初期値のままかどうかで判断する
        switch step {
        case .zero: return strayPF != 0
        case .threshold: return abs(vth - 0.5) > 1e-6
        case .cross: return abs(hiGain - 1) > 1e-6
        }
    }
}

struct CalibrationResult {
    let step: CalibrationStep
    let outcome: String   // ok / fail / abort
    let message: String
    let date = Date()
}

/// 今挿さっているもの (最新の測定値から判断する)
enum Presence: Equatable {
    case unknown
    case empty
    case capacitor(Double)
    case outOfRange
}

@MainActor
@Observable
final class MeterModel {
    private(set) var connected = false
    private(set) var portLabel = ""
    private(set) var activity: Activity?
    private(set) var activitySince = Date()
    private(set) var voltage: Double?
    private(set) var voltageDate = Date.distantPast
    private(set) var mode: RangeMode = .auto
    private(set) var calibration: CalibrationValues?
    private(set) var readings: [Reading] = []
    private(set) var lastResult: CalibrationResult?
    private(set) var log: [String] = []
    /// ボタンを押してから、ファームウェアが校正を始めたと知らせてくるまでの間
    private(set) var requestedStep: CalibrationStep?

    private let port = SerialPort()
    private var scanTimer: Timer?
    /// ウィンドウが裏に回っても App Nap で受信がまとめて遅らされないようにする
    /// (遅れると測定値の時刻がずれ、"今やっていること" も古くなる)
    private let activityToken = ProcessInfo.processInfo.beginActivity(
        options: .userInitiatedAllowingIdleSystemSleep, reason: "コンデンサ容量計の測定値を受信している")

    static let maxReadings = 60
    static let maxLog = 300

    init() {
        port.onLines = { [weak self] lines in lines.forEach { self?.handle($0) } }
        port.onClose = { [weak self] in self?.disconnected() }
        scanTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.connectIfNeeded() }
        }
        connectIfNeeded()
    }

    // MARK: - 接続

    private func connectIfNeeded() {
        guard !connected else { return }
        guard let pico = SerialPort.availablePorts().first(where: { $0.isPico }) else { return }
        do {
            try port.open(path: pico.path)
            connected = true
            portLabel = pico.label
            appendLog(Date(), "● 接続: \(pico.label)")
            port.send("i")   // 校正値とモードを聞く
        } catch {
            appendLog(Date(), "● 接続できません: \(error.localizedDescription)")
        }
    }

    private func disconnected() {
        connected = false
        activity = nil
        voltage = nil
        requestedStep = nil
        appendLog(Date(), "● 切断されました")
    }

    // MARK: - 操作

    func setMode(_ newMode: RangeMode) {
        port.send(newMode.command)
    }

    func run(_ step: CalibrationStep) {
        requestedStep = step
        lastResult = nil
        port.send(step.rawValue)
    }

    // MARK: - 受信

    private func handle(_ received: SerialPort.Line) {
        let line = received.text
        appendLog(received.date, line)
        guard line.hasPrefix("#") else { return }
        let maxSplits = line.hasPrefix("#cal,") ? 4 : 3
        let fields = line.dropFirst().split(separator: ",", maxSplits: maxSplits, omittingEmptySubsequences: false)
            .map(String.init)
        guard let kind = fields.first else { return }
        switch kind {
        case "state":
            guard fields.count >= 2, let a = Activity(rawValue: fields[1]) else { return }
            if a != activity { activitySince = received.date }
            activity = a
            if a.isCalibration { requestedStep = nil }
            if a == .idle || a == .measureLo { voltage = nil }
        case "v":
            guard fields.count >= 2, let v = Double(fields[1]) else { return }
            voltage = v
            voltageDate = received.date
        case "meas":
            guard fields.count >= 3 else { return }
            let value = Double(fields[2])
            let farads = (value?.isNaN ?? true) ? nil : value
            readings.append(Reading(date: received.date, farads: farads, range: fields[1]))
            if readings.count > Self.maxReadings { readings.removeFirst(readings.count - Self.maxReadings) }
        case "mode":
            guard fields.count >= 2, let m = RangeMode(rawValue: fields[1]) else { return }
            mode = m
        case "cal":
            guard fields.count >= 4, let vth = Double(fields[1]), let stray = Double(fields[2]),
                  let gain = Double(fields[3]) else { return }
            let flags = fields.count >= 5 ? Int(fields[4]) : nil
            calibration = CalibrationValues(vth: vth, strayPF: stray, hiGain: gain, doneFlags: flags)
        case "done":
            guard fields.count >= 4, let step = CalibrationStep(rawValue: fields[1]) else { return }
            lastResult = CalibrationResult(step: step, outcome: fields[2], message: fields[3])
            requestedStep = nil
        default:
            break
        }
    }

    private func appendLog(_ date: Date, _ line: String) {
        let time = date.formatted(date: .omitted, time: .standard)
        log.append("\(time)  \(line)")
        if log.count > Self.maxLog { log.removeFirst(log.count - Self.maxLog) }
    }

    // MARK: - 状態の判断

    var latest: Reading? { readings.last }

    /// 校正中 (ボタンを押した直後も含む)
    var calibrating: CalibrationStep? {
        if let requestedStep { return requestedStep }
        switch activity {
        case .calZero: return .zero
        case .calThreshold: return .threshold
        case .calCross: return .cross
        default: return nil
        }
    }

    /// 0.25 秒ごとに届く途中経過の電圧。止まっていたら表示しない
    var liveVoltage: Double? {
        guard let voltage, Date().timeIntervalSince(voltageDate) < 1.5 else { return nil }
        return voltage
    }

    /// 何も挿さっていないとみなす上限。ゼロ点校正前は浮遊容量 (約 27pF) がそのまま見える
    var emptyThreshold: Double {
        (calibration?.isDone(.zero) ?? false) ? 3e-12 : 100e-12
    }

    var presence: Presence {
        guard let latest else { return .unknown }
        guard let c = latest.farads else { return .outOfRange }
        return c < emptyThreshold ? .empty : .capacitor(c)
    }

    /// 直近の同じレンジの測定値が揃っているか
    var isStable: Bool {
        let recent = readings.suffix(3)
        guard recent.count == 3, let range = recent.last?.range,
              recent.allSatisfy({ $0.range == range && $0.farads != nil }) else { return false }
        let values = recent.compactMap(\.farads)
        guard let lo = values.min(), let hi = values.max() else { return false }
        return hi - lo <= max(abs(hi) * 0.003, 0.5e-12)
    }

    /// 校正を今実行できるか。できないときは理由を返す
    func readiness(_ step: CalibrationStep) -> (ok: Bool, reason: String) {
        guard connected else { return (false, "Pico がつながっていません") }
        if calibrating != nil { return (false, "ほかの校正を実行中です") }
        switch (step, presence) {
        case (_, .unknown):
            return (false, "最初の測定を待っています")
        case (.zero, .empty):
            return (true, "何も挿さっていません")
        case (.zero, _):
            return (false, "コンデンサを抜いてください")
        case (.threshold, .capacitor(let c)) where c >= 1e-9 && c <= 1e-6:
            return (true, "\(Format.farads(c)) が挿さっています")
        case (.cross, .capacitor(let c)) where c >= 30e-9 && c <= 250e-9:
            return (true, "\(Format.farads(c)) が挿さっています")
        case (.threshold, .capacitor(let c)), (.cross, .capacitor(let c)):
            return (false, "今の \(Format.farads(c)) では範囲外です")
        case (_, .empty):
            return (false, "コンデンサが挿さっていません")
        case (_, .outOfRange):
            return (false, "測定範囲外です")
        }
    }

    /// 次に校正すべきもの (全部済んでいれば nil)
    var nextCalibration: CalibrationStep? {
        guard let calibration else { return nil }
        return CalibrationStep.allCases.first { !calibration.isDone($0) }
    }
}

enum Format {
    /// ファームウェアの表示に合わせた桁数
    static func farads(_ c: Double) -> String {
        let a = abs(c)
        if a < 1e-9 { return String(format: "%.2f pF", c * 1e12) }
        if a < 1e-6 { return String(format: "%.3f nF", c * 1e9) }
        if a < 1e-3 { return String(format: "%.3f µF", c * 1e6) }
        return String(format: "%.3f mF", c * 1e3)
    }

    static func split(_ c: Double) -> (number: String, unit: String) {
        let parts = farads(c).split(separator: " ")
        return (String(parts[0]), String(parts[1]))
    }
}
