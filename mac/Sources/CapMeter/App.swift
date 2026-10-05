import SwiftUI

@main
struct CapMeterApp: App {
    @State private var model = MeterModel()

    var body: some Scene {
        WindowGroup("コンデンサ容量計") {
            ContentView()
                .environment(model)
                .frame(minWidth: 860, minHeight: 640)
        }
        .windowResizability(.contentMinSize)
    }
}
