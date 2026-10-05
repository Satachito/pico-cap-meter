import Foundation
import Darwin
import IOKit

/// 最小限の POSIX シリアルポート (Pico の USB CDC は /dev/cu.usbmodem* に出てくる)。
///
/// 受信は専用のキューで行い、行をまとめてメインスレッドに渡す。画面の描画が遅れても
/// 受信が止まらないようにするため (止まると Pico 側の USB 送信が詰まり、printf で測定が止まる)。
final class SerialPort {
    struct Line {
        let date: Date     // 受信した時刻
        let text: String
    }

    struct PortInfo: Hashable {
        let path: String
        let label: String
        let isPico: Bool
    }

    /// メインスレッドで呼ばれる
    var onLines: (([Line]) -> Void)?
    var onClose: (() -> Void)?

    private let queue = DispatchQueue(label: "local.capmeter.serial")
    private var fd: Int32 = -1          // queue 上でだけ触る (send はメインから同期で呼ぶ)
    private var source: DispatchSourceRead?
    private var buffer = Data()

    /// USB CDC のシリアルポートを機器名付きで返す (Pico を他のボードと取り違えないように)
    static func availablePorts() -> [PortInfo] {
        var result: [PortInfo] = []
        guard let match = IOServiceMatching("IOSerialBSDClient") else { return [] }
        var iter: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter) == KERN_SUCCESS else { return [] }
        defer { IOObjectRelease(iter) }
        while true {
            let svc = IOIteratorNext(iter)
            if svc == 0 { break }
            defer { IOObjectRelease(svc) }
            guard let path = IORegistryEntryCreateCFProperty(svc, "IOCalloutDevice" as CFString, kCFAllocatorDefault, 0)?
                    .takeRetainedValue() as? String, path.hasPrefix("/dev/cu.usbmodem") else { continue }
            func prop(_ key: String) -> Any? {
                IORegistryEntrySearchCFProperty(svc, kIOServicePlane, key as CFString, kCFAllocatorDefault,
                                                IOOptionBits(kIORegistryIterateRecursively | kIORegistryIterateParents))
            }
            let vendor = prop("idVendor") as? Int ?? 0
            let name = prop("USB Product Name") as? String ?? "不明な機器"
            let short = path.replacingOccurrences(of: "/dev/", with: "")
            let isPico = vendor == 0x2E8A            // Raspberry Pi
            result.append(PortInfo(path: path, label: "\(name) — \(short)", isPico: isPico))
        }
        return result.sorted { $0.path < $1.path }
    }

    func open(path: String) throws {
        let newFd = Darwin.open(path, O_RDWR | O_NOCTTY | O_NONBLOCK)
        guard newFd >= 0 else { throw NSError(domain: NSPOSIXErrorDomain, code: Int(errno),
            userInfo: [NSLocalizedDescriptionKey: String(cString: strerror(errno))]) }
        var t = termios()
        tcgetattr(newFd, &t)
        cfmakeraw(&t)
        cfsetspeed(&t, 115200)
        t.c_cflag |= tcflag_t(CLOCAL | CREAD)
        tcsetattr(newFd, TCSANOW, &t)

        queue.sync {
            fd = newFd
            buffer.removeAll()
            let s = DispatchSource.makeReadSource(fileDescriptor: newFd, queue: queue)
            s.setEventHandler { [weak self] in self?.readAvailable() }
            s.setCancelHandler { Darwin.close(newFd) }
            source = s
            s.resume()
        }
    }

    func close() {
        queue.async { self.closeOnQueue() }
    }

    func send(_ text: String) {
        queue.async {
            guard self.fd >= 0 else { return }
            let bytes = Array((text + "\r").utf8)
            _ = bytes.withUnsafeBufferPointer { Darwin.write(self.fd, $0.baseAddress, $0.count) }
        }
    }

    private func closeOnQueue() {
        source?.cancel()
        source = nil
        fd = -1
        buffer.removeAll()
    }

    private func readAvailable() {
        var chunk = [UInt8](repeating: 0, count: 4096)
        let n = Darwin.read(fd, &chunk, chunk.count)
        if n <= 0 {
            if n == 0 || (errno != EAGAIN && errno != EINTR) {
                closeOnQueue()
                DispatchQueue.main.async { self.onClose?() }
            }
            return
        }
        let now = Date()
        buffer.append(chunk, count: n)
        var lines: [Line] = []
        while let i = buffer.firstIndex(where: { $0 == 10 || $0 == 13 }) {
            let text = String(decoding: buffer[buffer.startIndex..<i], as: UTF8.self)
            buffer.removeSubrange(buffer.startIndex...i)
            if !text.isEmpty { lines.append(Line(date: now, text: text)) }
        }
        guard !lines.isEmpty else { return }
        DispatchQueue.main.async { self.onLines?(lines) }
    }
}
