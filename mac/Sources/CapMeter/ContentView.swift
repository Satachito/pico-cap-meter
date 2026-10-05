import Charts
import SwiftUI

struct ContentView: View {
    @Environment(MeterModel.self) private var model

    var body: some View {
        VStack(spacing: 0) {
            ConnectionBar()
            Divider()
            ScrollView {
                VStack(spacing: 16) {
                    GuidanceCard()
                    HStack(alignment: .top, spacing: 16) {
                        VStack(spacing: 16) {
                            ReadingCard()
                            ActivityCard()
                        }
                        .frame(maxWidth: .infinity)
                        VStack(spacing: 16) {
                            CalibrationCard()
                            HistoryCard()
                        }
                        .frame(maxWidth: .infinity)
                    }
                    LogCard()
                }
                .padding(16)
            }
        }
    }
}

// MARK: - 接続とモード

private struct ConnectionBar: View {
    @Environment(MeterModel.self) private var model

    var body: some View {
        HStack(spacing: 12) {
            Circle()
                .fill(model.connected ? Color.green : Color.secondary)
                .frame(width: 10, height: 10)
            Text(model.connected ? model.portLabel : "Pico を探しています…")
                .foregroundStyle(model.connected ? .primary : .secondary)
            Spacer()
            Text("レンジ")
                .foregroundStyle(.secondary)
            Picker("レンジ", selection: Binding(get: { model.mode }, set: { model.setMode($0) })) {
                ForEach(RangeMode.allCases) { Text($0.label).tag($0) }
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .frame(width: 330)
            .disabled(!model.connected || model.calibrating != nil)
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
    }
}

// MARK: - 次にやること

private struct GuidanceCard: View {
    @Environment(MeterModel.self) private var model

    private struct Guidance {
        var icon: String
        var tint: Color
        var title: String
        var detail: String
        var action: (label: String, enabled: Bool, perform: () -> Void)?
    }

    var body: some View {
        let g = guidance
        Card {
            VStack(alignment: .leading, spacing: 12) {
                if let result = model.lastResult {
                    ResultBanner(result: result)
                }
                HStack(alignment: .top, spacing: 14) {
                    Image(systemName: g.icon)
                        .font(.system(size: 30))
                        .foregroundStyle(g.tint)
                        .frame(width: 40)
                    VStack(alignment: .leading, spacing: 6) {
                        Text("次にやること")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                        Text(g.title)
                            .font(.title2.bold())
                        Text(g.detail)
                            .foregroundStyle(.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    Spacer()
                    if let action = g.action {
                        Button(action.label, action: action.perform)
                            .buttonStyle(.borderedProminent)
                            .controlSize(.large)
                            .disabled(!action.enabled)
                    }
                }
            }
        }
    }

    private var guidance: Guidance {
        guard model.connected else {
            return Guidance(icon: "cable.connector", tint: .secondary, title: "Pico を USB でつないでください",
                            detail: "つながると自動で認識します。BOOTSEL ボタンは押さずに挿してください。")
        }
        if let step = model.calibrating {
            return Guidance(icon: "gearshape.2", tint: .blue, title: "\(step.title)中です",
                            detail: "終わるまでコンデンサに触らないでください (\(step.duration))。")
        }
        guard let calibration = model.calibration else {
            return Guidance(icon: "hourglass", tint: .secondary, title: "校正値を読み込んでいます", detail: "")
        }
        if let step = model.nextCalibration {
            let ready = model.readiness(step)
            let instruction: String
            switch step {
            case .zero:
                instruction = "コンデンサを抜いて、GP26 の行と GND の行に何も挿さない状態にしてください。"
            case .threshold:
                instruction = "1nF〜1µF のコンデンサ (103 や 104 など) を挿してください。"
            case .cross:
                instruction = "30〜250nF のフィルムコンデンサ (104 など) を挿してください。誘電吸収が少ないフィルムが向いています。"
            }
            let detail = ready.ok
                ? "準備ができています (\(ready.reason))。ボタンを押してください。"
                : "\(instruction)\n今の状態: \(ready.reason)"
            return Guidance(icon: ready.ok ? "hand.point.right.fill" : "arrow.down.to.line.circle",
                            tint: ready.ok ? .green : .orange,
                            title: "\(step.number) \(step.title)をしてください",
                            detail: detail,
                            action: ("\(step.title)を実行", ready.ok, { model.run(step) }))
        }
        _ = calibration
        switch model.presence {
        case .unknown:
            return Guidance(icon: "hourglass", tint: .secondary, title: "最初の測定を待っています", detail: "")
        case .empty:
            return Guidance(icon: "arrow.down.to.line.circle", tint: .blue, title: "測りたいコンデンサを挿してください",
                            detail: "GP26 の行と GND の行のあいだに挿します。電解コンデンサは + を GP26 側にし、挿す前に足をショートして放電してください。")
        case .capacitor:
            let slow = model.latest?.range == "hi"
                ? "大容量は 1 回の測定 (主に放電) に時間がかかります。" : ""
            if model.isStable {
                return Guidance(icon: "checkmark.circle.fill", tint: .green, title: "測定できています",
                                detail: "値が安定しています。別のコンデンサを測るときは、抜いて挿し替えてください。\(slow)")
            }
            return Guidance(icon: "waveform.path.ecg", tint: .blue, title: "測定値が落ち着くのを待っています",
                            detail: "3 回続けて値がそろうと「安定」になります。\(slow)")
        case .outOfRange:
            if model.mode != .auto {
                return Guidance(icon: "exclamationmark.triangle.fill", tint: .orange, title: "このレンジでは測れません",
                                detail: "\(model.mode.label) に固定されています。自動に切り替えると測れる可能性があります。",
                                action: ("自動に切り替える", true, { model.setMode(.auto) }))
            }
            return Guidance(icon: "exclamationmark.triangle.fill", tint: .orange, title: "測定範囲外です",
                            detail: "約 1000µF を超えているか、足の接触が悪い可能性があります。挿し直してみてください。")
        }
    }
}

private struct ResultBanner: View {
    let result: CalibrationResult

    var body: some View {
        let (icon, color, title): (String, Color, String) = switch result.outcome {
        case "ok": ("checkmark.seal.fill", .green, "\(result.step.title)が完了しました")
        case "abort": ("stop.circle", .secondary, "\(result.step.title)を中断しました")
        default: ("exclamationmark.triangle.fill", .orange, "\(result.step.title)に失敗しました")
        }
        HStack(spacing: 8) {
            Image(systemName: icon).foregroundStyle(color)
            Text(title).bold()
            Text(result.message).foregroundStyle(.secondary)
            Spacer()
        }
        .padding(8)
        .background(color.opacity(0.12), in: RoundedRectangle(cornerRadius: 8))
    }
}

// MARK: - 測定値

private struct ReadingCard: View {
    @Environment(MeterModel.self) private var model

    var body: some View {
        Card(title: "測定値") {
            VStack(alignment: .leading, spacing: 8) {
                switch model.presence {
                case .unknown:
                    big("—", unit: "")
                    Text(model.connected ? "最初の測定を待っています" : "未接続").foregroundStyle(.secondary)
                case .empty:
                    big("—", unit: "")
                    if let c = model.latest?.farads {
                        Text("何も挿さっていません (\(Format.farads(c)))").foregroundStyle(.secondary)
                    }
                case .outOfRange:
                    big("範囲外", unit: "")
                    if let latest = model.latest {
                        Text(latest.rangeLabel).foregroundStyle(.secondary)
                    }
                case .capacitor(let c):
                    let (number, unit) = Format.split(c)
                    big(number, unit: unit)
                    HStack(spacing: 8) {
                        if let latest = model.latest {
                            Label(latest.rangeLabel, systemImage: latest.range == "hi" ? "gauge.high" : "gauge.low")
                                .foregroundStyle(.secondary)
                        }
                        Spacer()
                        if model.isStable {
                            Label("安定", systemImage: "checkmark.circle.fill").foregroundStyle(.green)
                        } else {
                            Label("読み取り中", systemImage: "ellipsis.circle").foregroundStyle(.secondary)
                        }
                    }
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private func big(_ number: String, unit: String) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 6) {
            Text(number)
                .font(.system(size: 52, weight: .semibold, design: .rounded))
                .monospacedDigit()
            Text(unit).font(.title)
        }
    }
}

// MARK: - 今やっていること

private struct ActivityCard: View {
    @Environment(MeterModel.self) private var model

    var body: some View {
        Card(title: "今やっていること") {
            TimelineView(.periodic(from: .now, by: 0.5)) { context in
                let (icon, title, detail) = describe(model.activity)
                VStack(alignment: .leading, spacing: 8) {
                    HStack(spacing: 10) {
                        Image(systemName: icon)
                            .font(.title2)
                            .foregroundStyle(.blue)
                            .frame(width: 28)
                            .symbolEffect(.pulse, isActive: model.activity != nil && model.activity != .idle)
                        Text(title).font(.headline)
                        Spacer()
                        let elapsed = context.date.timeIntervalSince(model.activitySince)
                        if model.activity != nil, model.activity != .idle, elapsed >= 2 {
                            Text("\(Int(elapsed)) 秒").monospacedDigit().foregroundStyle(.secondary)
                        }
                    }
                    Text(detail)
                        .font(.callout)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                    if let v = model.liveVoltage {
                        HStack {
                            ProgressView(value: min(max(v, 0), 3.3), total: 3.3)
                            Text(String(format: "%.2f V", v)).monospacedDigit().frame(width: 60, alignment: .trailing)
                        }
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
    }

    private func describe(_ activity: Activity?) -> (String, String, String) {
        guard model.connected else { return ("bolt.horizontal.circle", "未接続", "Pico をつなぐと、ここに動作の状況が出ます。") }
        switch activity {
        case nil:
            return ("questionmark.circle", "状態を確認中", "次の動作が始まると表示されます。")
        case .measureLo:
            return ("timer", "低容量レンジで測定中",
                    "1MΩ で充電し、GP26 が HIGH に変わるまでの時間を PIO で数えています (8 回平均)。")
        case .measureHi:
            return ("chart.xyaxis.line", "高容量レンジで測定中",
                    "10kΩ で充電しながら ADC で電圧を読み、充電カーブを当てはめています。")
        case .discharge:
            return ("arrow.down.circle", "放電中",
                    "次の測定の前にコンデンサを 0V に戻しています。大容量では数十秒かかることがあります。")
        case .idle:
            return ("pause.circle", "次の測定まで待機中", "0.3 秒ごとに測定を繰り返します。")
        case .calZero:
            return ("scope", "ゼロ点校正中", CalibrationStep.zero.purpose)
        case .calThreshold:
            return ("slider.horizontal.3", "しきい値校正中", CalibrationStep.threshold.purpose)
        case .calCross:
            return ("arrow.left.arrow.right", "レンジ間校正中", CalibrationStep.cross.purpose)
        }
    }
}

// MARK: - 校正

private struct CalibrationCard: View {
    @Environment(MeterModel.self) private var model

    var body: some View {
        Card(title: "校正") {
            VStack(alignment: .leading, spacing: 12) {
                ForEach(CalibrationStep.allCases) { step in
                    row(step)
                    if step != CalibrationStep.allCases.last { Divider() }
                }
                Text("校正値は Pico のフラッシュに保存され、電源を切っても消えません。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
    }

    private func row(_ step: CalibrationStep) -> some View {
        let done = model.calibration?.isDone(step) ?? false
        let ready = model.readiness(step)
        let running = model.calibrating == step
        return HStack(alignment: .top, spacing: 10) {
            Image(systemName: done ? "checkmark.circle.fill" : "circle")
                .foregroundStyle(done ? .green : .secondary)
                .font(.title3)
            VStack(alignment: .leading, spacing: 3) {
                HStack {
                    Text("\(step.number) \(step.title)").bold()
                    if let value = value(step), done {
                        Text(value).monospacedDigit().foregroundStyle(.secondary)
                    }
                }
                Text("必要なもの: \(step.requirement)")
                    .font(.caption)
                Text(running ? "実行中…" : ready.reason)
                    .font(.caption)
                    .foregroundStyle(ready.ok || running ? .green : .secondary)
            }
            Spacer()
            if running {
                ProgressView().controlSize(.small)
            } else {
                Button(done ? "やり直す" : "実行") { model.run(step) }
                    .disabled(!ready.ok)
            }
        }
        .help(step.purpose)
    }

    private func value(_ step: CalibrationStep) -> String? {
        guard let c = model.calibration else { return nil }
        switch step {
        case .zero: return String(format: "%.2f pF", c.strayPF)
        case .threshold: return String(format: "%.4f × VDD (%.3f V)", c.vth, c.vth * 3.3)
        case .cross: return String(format: "× %.4f", c.hiGain)
        }
    }
}

// MARK: - 履歴

private struct HistoryCard: View {
    @Environment(MeterModel.self) private var model

    var body: some View {
        Card(title: "最近の測定値") {
            let points = model.readings.compactMap { r in r.farads.map { (r.date, $0) } }
            if let last = points.last {
                let scale = unitScale(last.1)
                Chart(points, id: \.0) { point in
                    LineMark(x: .value("時刻", point.0), y: .value(scale.unit, point.1 / scale.factor))
                    PointMark(x: .value("時刻", point.0), y: .value(scale.unit, point.1 / scale.factor))
                        .symbolSize(12)
                }
                .chartYScale(domain: .automatic(includesZero: false))
                .chartYAxisLabel(scale.unit)
                .frame(height: 150)
            } else {
                Text("まだ測定値がありません").foregroundStyle(.secondary).frame(height: 150)
            }
        }
    }

    private func unitScale(_ c: Double) -> (factor: Double, unit: String) {
        let a = abs(c)
        if a < 1e-9 { return (1e-12, "pF") }
        if a < 1e-6 { return (1e-9, "nF") }
        return (1e-6, "µF")
    }
}

// MARK: - ログ

private struct LogCard: View {
    @Environment(MeterModel.self) private var model
    @State private var expanded = false

    var body: some View {
        Card {
            DisclosureGroup("シリアルの生ログ", isExpanded: $expanded) {
                ScrollViewReader { proxy in
                    ScrollView {
                        LazyVStack(alignment: .leading, spacing: 1) {
                            ForEach(Array(model.log.enumerated()), id: \.offset) { index, line in
                                Text(line)
                                    .font(.system(.caption, design: .monospaced))
                                    .textSelection(.enabled)
                                    .id(index)
                            }
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    .frame(height: 180)
                    .onChange(of: model.log.count) {
                        proxy.scrollTo(model.log.count - 1, anchor: .bottom)
                    }
                }
            }
        }
    }
}

// MARK: - 共通

private struct Card<Content: View>: View {
    var title: String?
    @ViewBuilder var content: Content

    init(title: String? = nil, @ViewBuilder content: () -> Content) {
        self.title = title
        self.content = content()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            if let title {
                Text(title).font(.caption.bold()).foregroundStyle(.secondary)
            }
            content
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.background.secondary, in: RoundedRectangle(cornerRadius: 12))
    }
}
