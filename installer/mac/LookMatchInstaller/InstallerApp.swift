// Install LookMatch: a small native installer that shows which Premiere Pro / After Effects
// versions are on this Mac inline (no alerts), lets the user locate one manually or continue
// without, copies LookMatch.plugin into Adobe's shared MediaCore folder and, for Premiere Pro
// 2025 or later, installs the LookMatch panel (.ccx) with Adobe's UnifiedPluginInstallerAgent.
//
// Built by installer/mac/build_app.sh with swiftc (no Xcode project).
// Environment, for testing only:
//   LOOKMATCH_INSTALLER_DRY_RUN=1      skip the admin copy and pretend it worked
//   LOOKMATCH_INSTALLER_SNAPSHOTS=dir  render every step to PNGs in dir and quit
//   LOOKMATCH_INSTALLER_LOCATE=a:b     with SNAPSHOTS: apps to add as if chosen with Locate…

import AppKit
import SwiftUI
import UniformTypeIdentifiers

let mediaCoreDir = "/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore"
let installLog = "/Library/Logs/LookMatch-install.log"
let upiaPath = "/Library/Application Support/Adobe/Adobe Desktop Common/RemoteComponents/UPI/UnifiedPluginInstallerAgent/UnifiedPluginInstallerAgent.app/Contents/MacOS/UnifiedPluginInstallerAgent"

// MARK: - Host detection

struct HostApp: Identifiable, Equatable {
    enum Kind { case premiere, afterEffects, other }

    let id = UUID()
    let name: String
    let path: String
    let version: String
    let kind: Kind
    var located = false

    // LookMatch needs CC 2015 or later: Premiere Pro 9.0+, After Effects 13.5+.
    var supported: Bool {
        let parts = version.split(separator: ".").map { Int($0) ?? 0 } + [0, 0]
        switch kind {
        case .premiere: return parts[0] >= 9
        case .afterEffects: return parts[0] > 13 || (parts[0] == 13 && parts[1] >= 5)
        case .other: return false
        }
    }

    // The panel is UXP, which Premiere Pro supports from 2025 (25.0).
    var supportsPanel: Bool {
        kind == .premiere && (Int(version.split(separator: ".").first ?? "") ?? 0) >= 25
    }

    static func == (a: HostApp, b: HostApp) -> Bool { a.path == b.path }

    static func at(_ url: URL) -> HostApp {
        let bundle = Bundle(url: url)
        let info = bundle?.infoDictionary ?? [:]
        let id = bundle?.bundleIdentifier ?? ""
        let name = (info["CFBundleDisplayName"] as? String) ?? url.deletingPathExtension().lastPathComponent
        let version = (info["CFBundleShortVersionString"] as? String) ?? ""
        let fileName = url.deletingPathExtension().lastPathComponent
        let kind: Kind
        if id.hasPrefix("com.adobe.PremierePro") || fileName.hasPrefix("Adobe Premiere Pro") {
            kind = .premiere
        } else if id.hasPrefix("com.adobe.AfterEffects") || fileName.hasPrefix("Adobe After Effects") {
            kind = .afterEffects
        } else {
            kind = .other
        }
        return HostApp(name: name, path: url.path, version: version, kind: kind)
    }
}

let hostPrefixes = ["Adobe Premiere Pro", "Adobe After Effects"]

func isHostAppName(_ name: String) -> Bool {
    // Skip helpers such as "Adobe After Effects Render Engine.app"
    guard name.hasSuffix(".app"), hostPrefixes.contains(where: { name.hasPrefix($0) }) else { return false }
    return !name.contains("Render Engine") && !name.contains("Crash")
}

/// Adobe installs each release as /Applications/Adobe Premiere Pro 2026/Adobe Premiere Pro 2026.app
/// (CC 2015-2019: "Adobe Premiere Pro CC 2019"). Look in /Applications and ~/Applications.
func scanForHosts() -> [HostApp] {
    let fm = FileManager.default
    let roots = ["/Applications", NSHomeDirectory() + "/Applications"]
    var found: [HostApp] = []
    for root in roots {
        guard let entries = try? fm.contentsOfDirectory(atPath: root) else { continue }
        for entry in entries.sorted() where hostPrefixes.contains(where: { entry.hasPrefix($0) }) {
            let url = URL(fileURLWithPath: root).appendingPathComponent(entry)
            if isHostAppName(entry) {
                found.append(.at(url))
            } else if let inner = try? fm.contentsOfDirectory(atPath: url.path) {
                for app in inner.sorted() where isHostAppName(app) {
                    found.append(.at(url.appendingPathComponent(app)))
                }
            }
        }
    }
    return found
}

// MARK: - Install

enum InstallError: LocalizedError {
    case missingPayload
    case cancelled
    case failed(String)

    var errorDescription: String? {
        switch self {
        case .missingPayload: return "LookMatch.plugin is missing from this installer. Download it again."
        case .cancelled: return "The installation was cancelled. Nothing was changed."
        case .failed(let msg): return msg
        }
    }
}

func shellQuote(_ s: String) -> String { "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'" }

func appleScriptQuote(_ s: String) -> String {
    "\"" + s.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"") + "\""
}

/// Copies the bundled plug-in into the shared MediaCore folder with administrator rights
/// (the standard macOS password prompt), clears quarantine and appends to the install log.
func installPlugin(hosts: [HostApp]) throws {
    guard let src = Bundle.main.url(forResource: "LookMatch", withExtension: "plugin") else {
        throw InstallError.missingPayload
    }
    let dest = mediaCoreDir + "/LookMatch.plugin"
    var logLines = ["LookMatch installed \(Date())", "Location: \(dest)", "Hosts:"]
    logLines += hosts.isEmpty ? ["  none found"] : hosts.map { "  \($0.name) \($0.version)\($0.supported ? "" : " (not supported)")" }
    let printLog = "printf '%s\\n' " + logLines.map(shellQuote).joined(separator: " ")

    let command = [
        "/bin/mkdir -p \(shellQuote(mediaCoreDir))",
        "/bin/rm -rf \(shellQuote(dest))",
        "/usr/bin/ditto \(shellQuote(src.path)) \(shellQuote(dest))",
        "(/usr/bin/xattr -dr com.apple.quarantine \(shellQuote(dest)) || true)",
        "(\(printLog) >> \(shellQuote(installLog)) || true)",
    ].joined(separator: " && ")

    if ProcessInfo.processInfo.environment["LOOKMATCH_INSTALLER_DRY_RUN"] == "1" {
        Thread.sleep(forTimeInterval: 1.0)
    } else {
        var error: NSDictionary?
        let script = NSAppleScript(source: "do shell script \(appleScriptQuote(command)) with administrator privileges")
        script?.executeAndReturnError(&error)
        if let error = error {
            if (error[NSAppleScript.errorNumber] as? Int) == -128 { throw InstallError.cancelled }
            throw InstallError.failed((error[NSAppleScript.errorMessage] as? String) ?? "The plug-in could not be copied.")
        }
    }

    // An older per-user copy would load twice next to the shared one; remove it (no admin needed).
    let userCopy = NSHomeDirectory() + "/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/LookMatch.plugin"
    try? FileManager.default.removeItem(atPath: userCopy)
}

enum PanelResult: Equatable {
    case installed, noPremiere, noInstaller, missingPayload, failed(String)
}

/// Installs the bundled panel as the current user (no admin needed); the effect works without it.
func installPanel(hosts: [HostApp]) -> PanelResult {
    guard hosts.contains(where: { $0.supportsPanel }) else { return .noPremiere }
    guard let ccx = Bundle.main.urls(forResourcesWithExtension: "ccx", subdirectory: nil)?.first else {
        return .missingPayload
    }
    if ProcessInfo.processInfo.environment["LOOKMATCH_INSTALLER_DRY_RUN"] == "1" { return .installed }
    guard FileManager.default.isExecutableFile(atPath: upiaPath) else { return .noInstaller }

    func run(_ args: [String]) -> (Int32, String) {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: upiaPath)
        p.arguments = args
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = pipe
        do { try p.run() } catch { return (-1, error.localizedDescription) }
        let out = String(data: pipe.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
        p.waitUntilExit()
        return (p.terminationStatus, out)
    }
    // Remove an older copy first: the agent won't replace a panel of the same version.
    _ = run(["--remove", "LookMatch"])
    let (status, out) = run(["--install", ccx.path])
    // The agent exits with 0 even when it fails ("Failed to install, status = -644!"), so read its output.
    if status == 0 && out.contains("Installation Successful") { return .installed }
    let detail = out.split(separator: "\n").last(where: { !$0.hasPrefix("Creative Cloud") && !$0.isEmpty })
    return .failed(detail.map(String.init) ?? "Adobe's plug-in installer stopped with code \(status).")
}

// MARK: - Model

final class InstallerModel: ObservableObject {
    enum Step: Int, CaseIterable {
        case welcome, apps, install, done
        var title: String { ["Introduction", "Adobe Apps", "Install", "Done"][rawValue] }
    }

    @Published var step: Step = .welcome
    @Published var hosts: [HostApp] = []
    @Published var installing = false
    @Published var installError: String?
    @Published var installCancelled = false
    @Published var panelResult: PanelResult?

    init() { hosts = scanForHosts() }

    var adobeHosts: [HostApp] { hosts.filter { $0.kind != .other } }

    func locate() {
        let panel = NSOpenPanel()
        panel.title = "Locate Premiere Pro or After Effects"
        panel.prompt = "Choose"
        panel.allowedContentTypes = [.application]
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        panel.directoryURL = URL(fileURLWithPath: "/Applications")
        guard panel.runModal() == .OK, let url = panel.url else { return }
        add(located: url)
    }

    func add(located url: URL) {
        var app = HostApp.at(url)
        app.located = true
        if !hosts.contains(app) { hosts.append(app) }
    }

    func install() {
        installing = true
        installError = nil
        installCancelled = false
        let hosts = self.hosts
        // The password prompt is modal; run off the main thread so the progress UI keeps drawing.
        DispatchQueue.global(qos: .userInitiated).async {
            var failure: String?
            var cancelled = false
            var panel: PanelResult?
            do {
                try installPlugin(hosts: hosts)
                panel = installPanel(hosts: hosts)
            } catch InstallError.cancelled { cancelled = true } catch {
                failure = error.localizedDescription
            }
            DispatchQueue.main.async {
                self.installing = false
                self.panelResult = panel
                if cancelled {
                    self.installCancelled = true
                } else if let failure = failure {
                    self.installError = failure
                } else {
                    self.step = .done
                }
            }
        }
    }
}

// MARK: - Views

struct StepList: View {
    let current: InstallerModel.Step

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            ForEach(InstallerModel.Step.allCases, id: \.rawValue) { step in
                HStack(spacing: 8) {
                    Circle()
                        .fill(step == current ? Color.accentColor : Color.secondary.opacity(0.35))
                        .frame(width: 8, height: 8)
                    Text(step.title)
                        .fontWeight(step == current ? .semibold : .regular)
                        .foregroundColor(step == current ? .primary : .secondary)
                }
            }
            Spacer()
        }
        .padding(.top, 28)
        .padding(.leading, 22)
        .frame(width: 170, alignment: .leading)
    }
}

struct HostRow: View {
    let host: HostApp

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            Image(systemName: icon)
                .foregroundColor(color)
                .font(.system(size: 15))
                .frame(width: 18)
            VStack(alignment: .leading, spacing: 2) {
                Text(host.name + (host.version.isEmpty ? "" : "  \(host.version)"))
                    .fontWeight(.medium)
                Text(note)
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
        }
    }

    var icon: String {
        if host.kind == .other { return "exclamationmark.circle" }
        return host.supported ? "checkmark.circle.fill" : "minus.circle"
    }

    var color: Color {
        if host.kind == .other { return .orange }
        return host.supported ? .green : .secondary
    }

    var note: String {
        if host.kind == .other { return "This isn't Premiere Pro or After Effects, so LookMatch won't appear in it." }
        if !host.supported { return "Older than CC 2015. LookMatch won't load here." }
        let what = host.supportsPanel ? "The LookMatch effect and panel" : "LookMatch"
        return host.located ? "Located manually. \(what) will appear in it." : "\(what) will appear in it."
    }
}

struct WelcomeView: View {
    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("LookMatch by PRO PLUGS")
                .font(.system(size: 22, weight: .semibold))
            Text("Match the look of any reference still, then finish the shot with film print emulation, halation, bloom and grain.")
                .fixedSize(horizontal: false, vertical: true)
            Text("Works with Premiere Pro and After Effects CC 2015 to 2026. One plug-in covers every version, so there's nothing to choose. Premiere Pro 2025 and later also get the LookMatch panel.")
                .foregroundColor(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            Text("Quit Premiere Pro and After Effects before installing.")
                .foregroundColor(.secondary)
        }
    }
}

struct AppsView: View {
    @ObservedObject var model: InstallerModel

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Adobe apps on this Mac")
                .font(.system(size: 18, weight: .semibold))
            if model.adobeHosts.isEmpty {
                HStack(alignment: .top, spacing: 10) {
                    Image(systemName: "info.circle")
                        .foregroundColor(.secondary)
                        .font(.system(size: 15))
                    VStack(alignment: .leading, spacing: 4) {
                        Text("No Premiere Pro or After Effects found on this Mac.")
                            .fontWeight(.medium)
                        Text("If one is installed somewhere else, locate it. Or continue without it: LookMatch installs now and appears once you install Premiere Pro or After Effects.")
                            .font(.callout)
                            .foregroundColor(.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
                .padding(12)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(RoundedRectangle(cornerRadius: 8).fill(Color.secondary.opacity(0.08)))
            }
            if model.hosts.count > 4 {
                ScrollView { hostList }.frame(height: 170)
            } else if !model.hosts.isEmpty {
                hostList
            }
            Button("Locate…") { model.locate() }
            Text("LookMatch installs into Adobe's shared plug-in folder, which every version reads.")
                .font(.caption)
                .foregroundColor(.secondary)
        }
    }

    var hostList: some View {
        VStack(alignment: .leading, spacing: 12) {
            ForEach(model.hosts) { HostRow(host: $0) }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

struct InstallView: View {
    @ObservedObject var model: InstallerModel

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Install LookMatch")
                .font(.system(size: 18, weight: .semibold))
            Text("LookMatch will be copied to:")
            Text(mediaCoreDir)
                .font(.system(.callout, design: .monospaced))
                .foregroundColor(.secondary)
            Text("macOS will ask for your administrator password.")
                .foregroundColor(.secondary)
            if model.installing {
                HStack(spacing: 8) {
                    ProgressView().scaleEffect(0.6).frame(width: 16, height: 16)
                    Text("Installing…")
                }
            }
            if model.installCancelled {
                HStack(alignment: .top, spacing: 8) {
                    Image(systemName: "info.circle").foregroundColor(.secondary)
                    Text(InstallError.cancelled.errorDescription!).foregroundColor(.secondary)
                }
            }
            if let error = model.installError {
                HStack(alignment: .top, spacing: 8) {
                    Image(systemName: "xmark.octagon.fill").foregroundColor(.red)
                    Text(error).fixedSize(horizontal: false, vertical: true)
                }
            }
        }
    }
}

struct DoneView: View {
    @ObservedObject var model: InstallerModel

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(spacing: 10) {
                Image(systemName: "checkmark.circle.fill")
                    .foregroundColor(.green)
                    .font(.system(size: 24))
                Text("LookMatch is installed")
                    .font(.system(size: 18, weight: .semibold))
            }
            Text("Restart Premiere Pro, then find it under Effects > Video Effects > PRO PLUGS > LookMatch.")
                .fixedSize(horizontal: false, vertical: true)
            Text("In After Effects: Effect > PRO PLUGS > LookMatch.")
                .foregroundColor(.secondary)
            if let panel = model.panelResult, panel != .noPremiere {
                HStack(alignment: .top, spacing: 8) {
                    Image(systemName: panel == .installed ? "checkmark.circle.fill" : "exclamationmark.triangle.fill")
                        .foregroundColor(panel == .installed ? .green : .orange)
                    Text(panelNote(panel)).fixedSize(horizontal: false, vertical: true)
                }
            }
        }
    }
}

func panelNote(_ result: PanelResult) -> String {
    switch result {
    case .installed: return "The LookMatch panel is installed too: Window > UXP Plugins > LookMatch."
    case .noPremiere: return ""
    case .noInstaller: return "The panel wasn't installed because Adobe's plug-in installer is missing. Open Creative Cloud once, then run this installer again. The effect works without it."
    case .missingPayload: return "This installer doesn't include the panel. The effect works without it."
    case .failed(let msg): return "The panel couldn't be installed (\(msg)). The effect works without it. To add the panel, double-click LookMatchPanel.ccx on this installer's disk (Creative Cloud installs it), or run this installer again."
    }
}

struct InstallerView: View {
    @ObservedObject var model: InstallerModel

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 0) {
                StepList(current: model.step)
                Divider()
                Group {
                    switch model.step {
                    case .welcome: WelcomeView()
                    case .apps: AppsView(model: model)
                    case .install: InstallView(model: model)
                    case .done: DoneView(model: model)
                    }
                }
                .padding(28)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
            }
            Divider()
            HStack {
                Spacer()
                if model.step != .welcome && model.step != .done {
                    Button("Go Back") { model.step = InstallerModel.Step(rawValue: model.step.rawValue - 1)! }
                        .disabled(model.installing)
                }
                primaryButton
            }
            .padding(16)
        }
        .frame(width: 640, height: 420)
    }

    @ViewBuilder var primaryButton: some View {
        switch model.step {
        case .welcome:
            Button("Continue") { model.step = .apps }.keyboardShortcut(.defaultAction)
        case .apps:
            Button(model.adobeHosts.isEmpty ? "Continue Without" : "Continue") { model.step = .install }
                .keyboardShortcut(.defaultAction)
        case .install:
            Button(model.installError == nil && !model.installCancelled ? "Install" : "Try Again") { model.install() }
                .keyboardShortcut(.defaultAction)
                .disabled(model.installing)
        case .done:
            Button("Done") { NSApp.terminate(nil) }.keyboardShortcut(.defaultAction)
        }
    }
}

// MARK: - App

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    func applicationDidFinishLaunching(_ notification: Notification) {
        if let dir = ProcessInfo.processInfo.environment["LOOKMATCH_INSTALLER_SNAPSHOTS"] {
            renderSnapshots(to: URL(fileURLWithPath: dir))
            NSApp.terminate(nil)
            return
        }
        // macOS 11 SwiftUI doesn't size the window to its content: fix it at 640x420, like Installer.app.
        DispatchQueue.main.async {
            for window in NSApp.windows {
                window.styleMask.remove(.resizable)
                window.setContentSize(NSSize(width: 640, height: 420))
                window.center()
            }
        }
    }

    /// Renders each step offscreen to PNG, for docs and visual checks (no window or permissions needed).
    func renderSnapshots(to dir: URL) {
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let model = InstallerModel()
        var shots: [(String, () -> Void)] = [
            ("1-welcome", { model.step = .welcome }),
            ("2-apps", { model.step = .apps }),
        ]
        if let locate = ProcessInfo.processInfo.environment["LOOKMATCH_INSTALLER_LOCATE"] {
            let paths = locate.split(separator: ":").map(String.init)
            shots.append(("3-apps-located", { paths.forEach { model.add(located: URL(fileURLWithPath: $0)) } }))
        }
        shots += [
            ("4-install", { model.step = .install }),
            ("5-installing", { model.installing = true }),
            ("6-install-cancelled", { model.installing = false; model.installCancelled = true }),
            ("7-done", { model.installCancelled = false; model.panelResult = .installed; model.step = .done }),
            ("8-done-panel-failed", { model.panelResult = .failed("Failed to install, status = -644!") }),
        ]
        for (name, apply) in shots {
            apply()
            let view = NSHostingView(rootView: InstallerView(model: model))
            view.frame = NSRect(x: 0, y: 0, width: 640, height: 420)
            let window = NSWindow(contentRect: view.frame, styleMask: [.titled], backing: .buffered, defer: false)
            window.contentView = view
            view.layoutSubtreeIfNeeded()
            RunLoop.current.run(until: Date().addingTimeInterval(0.2))
            guard let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) else { continue }
            view.cacheDisplay(in: view.bounds, to: rep)
            try? rep.representation(using: .png, properties: [:])?.write(to: dir.appendingPathComponent("\(name).png"))
        }
    }
}

@main
struct LookMatchInstallerApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate
    @StateObject private var model = InstallerModel()

    var body: some Scene {
        WindowGroup("Install LookMatch") {
            InstallerView(model: model)
        }
        .commands { CommandGroup(replacing: .newItem) {} }
    }
}
