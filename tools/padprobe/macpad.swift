// macpad.swift <seconds>: log every change of the physical controller's buttons and triggers as macOS
// GameController sees them (background events on), one line per change.
import Foundation
import GameController
setvbuf(stdout, nil, _IOLBF, 0)
let secs = Double(CommandLine.arguments.dropFirst().first ?? "120") ?? 120
GCController.shouldMonitorBackgroundEvents = true
func fmt(_ t: Date) -> String { let f = DateFormatter(); f.dateFormat = "HH:mm:ss.SSS"; return f.string(from: t) }
var last = ""
func attach(_ c: GCController) {
    print("\(fmt(Date())) controller: \(c.vendorName ?? "?") \(c.productCategory)")
    guard let g = c.extendedGamepad else { print("  no extendedGamepad"); return }
    g.valueChangedHandler = { g, _ in
        var on: [String] = []
        let b: [(String, GCControllerButtonInput)] = [("A", g.buttonA), ("B", g.buttonB), ("X", g.buttonX), ("Y", g.buttonY),
            ("LB", g.leftShoulder), ("RB", g.rightShoulder), ("Menu", g.buttonMenu)]
        for (n, x) in b where x.isPressed { on.append(n) }
        if let o = g.buttonOptions, o.isPressed { on.append("View") }
        if g.dpad.up.isPressed { on.append("Up") }; if g.dpad.down.isPressed { on.append("Down") }
        if g.dpad.left.isPressed { on.append("Left") }; if g.dpad.right.isPressed { on.append("Right") }
        let s = String(format: "buttons=[%@] LT=%.2f RT=%.2f", on.joined(separator: " "), g.leftTrigger.value, g.rightTrigger.value)
        if s != last { last = s; print("\(fmt(Date())) \(s)") }
    }
}
NotificationCenter.default.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { n in
    if let c = n.object as? GCController { attach(c) } }
GCController.startWirelessControllerDiscovery {}
DispatchQueue.main.asyncAfter(deadline: .now() + 1) { if GCController.controllers().isEmpty { print("no controller seen") } }
DispatchQueue.main.asyncAfter(deadline: .now() + secs) { print("done"); exit(0) }
RunLoop.main.run()
