// Install Komodo: a one-screen installer in Komodo's own look (Premiere greys, Premiere blue for
// what's active). It shows which Premiere Pro / After Effects versions are on this Mac, warns if one
// is running, copies the plug-in (LookMatch.plugin, its file name since before Komodo) into Adobe's
// shared MediaCore folder and, for Premiere Pro 2025 or later, installs the Komodo panel (.ccx)
// with Adobe's UnifiedPluginInstallerAgent.
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

    // Komodo needs CC 2015 or later: Premiere Pro 9.0+, After Effects 13.5+.
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
        case .missingPayload: return "The Komodo plug-in is missing from this installer. Download it again."
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
    var logLines = ["Komodo installed \(Date())", "Location: \(dest)", "Hosts:"]
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
    _ = run(["--remove", "LookMatch"])  // the panel's name before 1.0
    _ = run(["--remove", "Komodo"])
    let (status, out) = run(["--install", ccx.path])
    if status == 0 { return .installed }
    let detail = out.split(separator: "\n").last(where: { !$0.hasPrefix("Creative Cloud") && !$0.isEmpty })
    return .failed(detail.map(String.init) ?? "Adobe's plug-in installer stopped with code \(status).")
}

// MARK: - Model

final class InstallerModel: ObservableObject {
    enum Step: Int, CaseIterable { case ready, installing, done }

    @Published var step: Step = .ready
    @Published var hosts: [HostApp] = []
    @Published var installError: String?
    @Published var installCancelled = false
    @Published var panelResult: PanelResult?
    @Published var runningAdobe: [String] = []

    init() {
        hosts = scanForHosts()
        refreshRunning()
    }

    var adobeHosts: [HostApp] { hosts.filter { $0.kind != .other } }

    /// Premiere Pro or After Effects open right now: the plug-in can't be replaced while they run.
    func refreshRunning() {
        if ProcessInfo.processInfo.environment["LOOKMATCH_INSTALLER_SNAPSHOTS"] != nil { return }
        runningAdobe = NSWorkspace.shared.runningApplications.compactMap { app in
            let id = app.bundleIdentifier ?? ""
            guard id.hasPrefix("com.adobe.PremierePro") || id.hasPrefix("com.adobe.AfterEffects") else { return nil }
            return app.localizedName
        }
    }

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
        refreshRunning()
        guard runningAdobe.isEmpty else { return }
        step = .installing
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
                self.panelResult = panel
                if cancelled {
                    self.installCancelled = true
                    self.step = .ready
                } else if let failure = failure {
                    self.installError = failure
                    self.step = .ready
                } else {
                    self.step = .done
                }
            }
        }
    }
}

// MARK: - Komodo look (same palette as the panel)

enum K {
    static let bg = Color(red: Double(0x23) / 255, green: Double(0x23) / 255, blue: Double(0x23) / 255)
    static let card = Color(red: Double(0x2a) / 255, green: Double(0x2a) / 255, blue: Double(0x2a) / 255)
    static let border = Color(red: Double(0x3e) / 255, green: Double(0x3e) / 255, blue: Double(0x3e) / 255)
    static let tile = Color(red: Double(0x4a) / 255, green: Double(0x4a) / 255, blue: Double(0x4a) / 255)
    static let text = Color(red: Double(0xe6) / 255, green: Double(0xe6) / 255, blue: Double(0xe6) / 255)
    static let dim = Color(red: Double(0xa8) / 255, green: Double(0xa8) / 255, blue: Double(0xa8) / 255)
    static let faint = Color(red: Double(0x7a) / 255, green: Double(0x7a) / 255, blue: Double(0x7a) / 255)
    static let blue = Color(red: Double(0x2d) / 255, green: Double(0x6f) / 255, blue: Double(0xdb) / 255)
    static let blueHover = Color(red: Double(0x3b) / 255, green: Double(0x7f) / 255, blue: Double(0xe8) / 255)
    static let blueText = Color(red: Double(0x5b) / 255, green: Double(0x9b) / 255, blue: Double(0xf0) / 255)
    static let warn = Color(red: Double(0xfb) / 255, green: Double(0xbf) / 255, blue: Double(0x24) / 255)
    static let err = Color(red: Double(0xf8) / 255, green: Double(0x71) / 255, blue: Double(0x71) / 255)
    static let width: CGFloat = 560, height: CGFloat = 460
}

/// The Komodo mark: two overlapping rings in a rounded tile, as in the panel header.
struct Logo: View {
    var size: CGFloat = 44
    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: size * 0.3).fill(K.tile)
            Circle().stroke(Color.white, lineWidth: size * 0.06)
                .frame(width: size * 0.36, height: size * 0.36).offset(x: -size * 0.09)
            Circle().stroke(Color(white: 0.82), lineWidth: size * 0.06)
                .frame(width: size * 0.36, height: size * 0.36).offset(x: size * 0.09)
        }
        .frame(width: size, height: size)
    }
}

struct PrimaryButton: ButtonStyle {
    var enabled = true
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 14, weight: .heavy))
            .foregroundColor(enabled ? .white : K.faint)
            .frame(maxWidth: .infinity)
            .padding(.vertical, 11)
            .background(RoundedRectangle(cornerRadius: 12)
                .fill(enabled ? (configuration.isPressed ? K.blueHover : K.blue) : K.card))
            .overlay(RoundedRectangle(cornerRadius: 12).stroke(enabled ? Color.clear : K.border, lineWidth: 1))
    }
}

struct QuietButton: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 11.5, weight: .semibold))
            .foregroundColor(configuration.isPressed ? .white : K.dim)
            .padding(.horizontal, 12).padding(.vertical, 6)
            .background(RoundedRectangle(cornerRadius: 8).fill(K.card))
            .overlay(RoundedRectangle(cornerRadius: 8).stroke(K.border, lineWidth: 1))
    }
}

struct Card<Content: View>: View {
    let content: Content
    init(@ViewBuilder _ content: () -> Content) { self.content = content() }
    var body: some View {
        content
            .padding(12)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: 12).fill(K.card))
            .overlay(RoundedRectangle(cornerRadius: 12).stroke(K.border, lineWidth: 1))
    }
}

struct SectionLabel: View {
    let text: String
    var body: some View {
        Text(text).font(.system(size: 10, weight: .bold)).tracking(2.5).foregroundColor(K.dim)
    }
}

/// A note line with a small icon: info (grey), warning (amber), error (red), ok (blue).
struct Note: View {
    enum Kind { case info, warn, error, ok }
    let kind: Kind
    let text: String
    var body: some View {
        HStack(alignment: .top, spacing: 8) {
            Image(systemName: icon).foregroundColor(color).font(.system(size: 12, weight: .semibold)).padding(.top, 1)
            Text(text).font(.system(size: 12)).foregroundColor(kind == .info ? K.dim : K.text)
                .fixedSize(horizontal: false, vertical: true)
        }
    }
    var icon: String {
        switch kind {
        case .info: return "info.circle"
        case .warn: return "exclamationmark.triangle.fill"
        case .error: return "xmark.octagon.fill"
        case .ok: return "checkmark.circle.fill"
        }
    }
    var color: Color {
        switch kind {
        case .info: return K.faint
        case .warn: return K.warn
        case .error: return K.err
        case .ok: return K.blue
        }
    }
}

// MARK: - Screens

struct Header: View {
    var body: some View {
        HStack(spacing: 12) {
            Logo(size: 40)
            VStack(alignment: .leading, spacing: 1) {
                Text("Komodo").font(.system(size: 20, weight: .heavy)).foregroundColor(.white)
                Text("v1.0.0 · PRO PLUGS").font(.system(size: 10.5, weight: .semibold)).foregroundColor(K.faint)
            }
            Spacer()
        }
    }
}

/// A white tick on a solid blue circle (the filled SF Symbol renders its tick dark in this window).
struct BlueTick: View {
    var size: CGFloat
    var body: some View {
        ZStack {
            Circle().fill(K.blue)
            Image(systemName: "checkmark").font(.system(size: size * 0.5, weight: .bold)).foregroundColor(.white)
        }
        .frame(width: size, height: size)
    }
}

struct HostRow: View {
    let host: HostApp
    var body: some View {
        HStack(spacing: 10) {
            Group {
                if host.kind != .other && host.supported {
                    BlueTick(size: 15)
                } else {
                    Image(systemName: icon).foregroundColor(color).font(.system(size: 13, weight: .semibold))
                }
            }
            .frame(width: 16)
            VStack(alignment: .leading, spacing: 1) {
                Text(host.name + (host.version.isEmpty ? "" : "  \(host.version)"))
                    .font(.system(size: 12.5, weight: .bold)).foregroundColor(.white)
                Text(note).font(.system(size: 11)).foregroundColor(K.dim)
            }
            Spacer()
        }
    }
    var icon: String {
        if host.kind == .other { return "exclamationmark.circle" }
        return host.supported ? "checkmark.circle.fill" : "minus.circle"
    }
    var color: Color {
        if host.kind == .other { return K.warn }
        return host.supported ? K.blue : K.faint
    }
    var note: String {
        if host.kind == .other { return "Not Premiere Pro or After Effects: Komodo won't appear here." }
        if !host.supported { return "Older than CC 2015: Komodo won't load here." }
        let what = host.supportsPanel ? "Effect and panel" : "Effect"
        return host.located ? "Located manually. \(what) will be installed." : "\(what) will be installed."
    }
}

struct ReadyView: View {
    @ObservedObject var model: InstallerModel

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Match the look of any reference still, then finish it with film texture.")
                .font(.system(size: 13)).foregroundColor(K.dim).fixedSize(horizontal: false, vertical: true)
            HStack {
                SectionLabel(text: "ADOBE APPS ON THIS MAC")
                Spacer()
                Button("Locate…") { model.locate() }.buttonStyle(QuietButton())
            }
            Card {
                if model.hosts.isEmpty {
                    Note(kind: .info, text: "No Premiere Pro or After Effects found. You can still install: Komodo appears once you install one, or use Locate… if it's somewhere else.")
                } else {
                    ScrollView {
                        VStack(alignment: .leading, spacing: 10) { ForEach(model.hosts) { HostRow(host: $0) } }
                    }
                    .frame(maxHeight: 128)
                }
            }
            if !model.runningAdobe.isEmpty {
                Note(kind: .warn, text: "Quit \(model.runningAdobe.joined(separator: " and ")) first, then click Install. Save your work before you quit.")
            }
            if model.installCancelled { Note(kind: .info, text: InstallError.cancelled.errorDescription!) }
            if let error = model.installError { Note(kind: .error, text: error) }
            Spacer(minLength: 0)
            Button(model.installError == nil && !model.installCancelled ? "Install Komodo" : "Try Again") { model.install() }
                .buttonStyle(PrimaryButton(enabled: model.runningAdobe.isEmpty))
                .disabled(!model.runningAdobe.isEmpty)
            Text("macOS will ask for your password to copy Komodo into Adobe's shared plug-in folder.")
                .font(.system(size: 10.5)).foregroundColor(K.faint).frame(maxWidth: .infinity)
        }
    }
}

struct InstallingView: View {
    var body: some View {
        VStack(spacing: 14) {
            Spacer()
            ProgressView().progressViewStyle(CircularProgressViewStyle(tint: K.blue)).scaleEffect(1.1)
            Text("Installing Komodo…").font(.system(size: 15, weight: .bold)).foregroundColor(.white)
            Text("Enter your password if macOS asks for it.").font(.system(size: 12)).foregroundColor(K.dim)
            Spacer()
        }
        .frame(maxWidth: .infinity)
    }
}

struct DoneView: View {
    @ObservedObject var model: InstallerModel

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 10) {
                BlueTick(size: 26)
                Text("Komodo is installed").font(.system(size: 18, weight: .heavy)).foregroundColor(.white)
            }
            SectionLabel(text: "WHERE TO FIND IT")
            Card {
                VStack(alignment: .leading, spacing: 10) {
                    step("1", "Open Premiere Pro.")
                    if model.panelResult == .installed {
                        step("2", "Window > UXP Plugins > Komodo opens the panel. Drop a still, click Match.")
                    }
                }
            }
            Text("In After Effects: Effect > PRO PLUGS > Komodo.").font(.system(size: 11.5)).foregroundColor(K.dim)
            if let panel = model.panelResult, panel != .noPremiere, panel != .installed {
                Note(kind: .warn, text: panelNote(panel))
            }
            Spacer(minLength: 0)
            Button("Done") { NSApp.terminate(nil) }.buttonStyle(PrimaryButton())
        }
    }

    func step(_ n: String, _ text: String) -> some View {
        HStack(alignment: .top, spacing: 10) {
            Text(n).font(.system(size: 11, weight: .heavy)).foregroundColor(.white)
                .frame(width: 20, height: 20).background(Circle().fill(K.blue))
            Text(text).font(.system(size: 12.5)).foregroundColor(K.text).fixedSize(horizontal: false, vertical: true)
                .padding(.top, 2)
        }
    }
}

func panelNote(_ result: PanelResult) -> String {
    switch result {
    case .installed: return "The Komodo panel is installed too: Window > UXP Plugins > Komodo."
    case .noPremiere: return ""
    case .noInstaller: return "The panel wasn't installed because Adobe's plug-in installer is missing. Open Creative Cloud once, then run this installer again. The effect works without it."
    case .missingPayload: return "This installer doesn't include the panel. The effect works without it."
    case .failed(let msg): return "The panel couldn't be installed (\(msg)). The effect works without it; run this installer again to retry."
    }
}

struct InstallerView: View {
    @ObservedObject var model: InstallerModel

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Header()
            Rectangle().fill(K.border).frame(height: 1)
            Group {
                switch model.step {
                case .ready: ReadyView(model: model)
                case .installing: InstallingView()
                case .done: DoneView(model: model)
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        }
        .padding(.horizontal, 24)
        .padding(.top, 34)  // clear of the window's close button (the title bar is hidden)
        .padding(.bottom, 20)
        .frame(width: K.width, height: K.height)
        .background(K.bg)
        .preferredColorScheme(.dark)
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in
            model.refreshRunning()
        }
    }
}

// MARK: - App

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    func applicationDidFinishLaunching(_ notification: Notification) {
        if let file = ProcessInfo.processInfo.environment["LOOKMATCH_INSTALLER_ICON"] {
            renderIcon(to: URL(fileURLWithPath: file))
            NSApp.terminate(nil)
            return
        }
        if let dir = ProcessInfo.processInfo.environment["LOOKMATCH_INSTALLER_SNAPSHOTS"] {
            renderSnapshots(to: URL(fileURLWithPath: dir))
            NSApp.terminate(nil)
            return
        }
        // Fixed size, dark, no title text: the window is Komodo's own surface edge to edge.
        DispatchQueue.main.async {
            for window in NSApp.windows {
                window.styleMask.remove(.resizable)
                window.titlebarAppearsTransparent = true
                window.titleVisibility = .hidden
                window.isMovableByWindowBackground = true
                window.backgroundColor = NSColor(red: 35.0 / 255, green: 35.0 / 255, blue: 35.0 / 255, alpha: 1)
                window.appearance = NSAppearance(named: .darkAqua)
                window.setContentSize(NSSize(width: K.width, height: K.height))
                window.center()
            }
        }
    }

    /// The app icon: the Komodo mark at 1024 px, inset like other macOS icons (build_app.sh makes the .icns).
    func renderIcon(to file: URL) {
        let view = NSHostingView(rootView: Logo(size: 824).frame(width: 1024, height: 1024))
        view.frame = NSRect(x: 0, y: 0, width: 1024, height: 1024)
        let window = NSWindow(contentRect: view.frame, styleMask: [.borderless], backing: .buffered, defer: false)
        window.backgroundColor = .clear
        window.contentView = view
        view.layoutSubtreeIfNeeded()
        RunLoop.current.run(until: Date().addingTimeInterval(0.3))
        guard let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) else { return }
        view.cacheDisplay(in: view.bounds, to: rep)
        try? rep.representation(using: .png, properties: [:])?.write(to: file)
    }

    /// Renders each screen offscreen to PNG, for docs and visual checks (no window or permissions needed).
    func renderSnapshots(to dir: URL) {
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let model = InstallerModel()
        var shots: [(String, () -> Void)] = [("1-ready", { model.step = .ready })]
        if let locate = ProcessInfo.processInfo.environment["LOOKMATCH_INSTALLER_LOCATE"] {
            let paths = locate.split(separator: ":").map(String.init)
            shots.append(("2-ready-located", { paths.forEach { model.add(located: URL(fileURLWithPath: $0)) } }))
        }
        shots += [
            ("3-premiere-open", { model.runningAdobe = ["Adobe Premiere Pro 2026"] }),
            ("4-installing", { model.runningAdobe = []; model.step = .installing }),
            ("5-cancelled", { model.installCancelled = true; model.step = .ready }),
            ("6-done", { model.installCancelled = false; model.panelResult = .installed; model.step = .done }),
        ]
        for (name, apply) in shots {
            apply()
            let view = NSHostingView(rootView: InstallerView(model: model))
            view.frame = NSRect(x: 0, y: 0, width: K.width, height: K.height)
            let window = NSWindow(contentRect: view.frame, styleMask: [.titled], backing: .buffered, defer: false)
            window.appearance = NSAppearance(named: .darkAqua)
            window.contentView = view
            view.layoutSubtreeIfNeeded()
            RunLoop.current.run(until: Date().addingTimeInterval(0.3))
            guard let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) else { continue }
            view.cacheDisplay(in: view.bounds, to: rep)
            try? rep.representation(using: .png, properties: [:])?.write(to: dir.appendingPathComponent("\(name).png"))
        }
    }
}

@main
struct KomodoInstallerApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate
    @StateObject private var model = InstallerModel()

    var body: some Scene {
        WindowGroup("Install Komodo") {
            InstallerView(model: model)
        }
        .windowStyle(HiddenTitleBarWindowStyle())
        .commands { CommandGroup(replacing: .newItem) {} }
    }
}
