// WineMetalGL Settings: where WineMetalGL is installed (Installations.swift),
// its updates (Updater.swift), the game fixes it applies (GamePatches.swift),
// and UpscaleGL's preset, x87 acceleration and logging, for programs
// launched without a way to set their environment. They go to the settings
// file the core reads when it loads (src/core/util/settings.h); the
// environment still wins, and a change applies to games started afterwards.

import AppKit
import SwiftUI

// MARK: - Where the app looks

/// The home directory and defaults the app works with. WINEMETALGL_SETTINGS_HOME
/// gives it another home (and its own defaults), to try updates and game fixes
/// on made-up prefixes without touching the real ones.
enum AppEnvironment {
    static let testHome: URL? = ProcessInfo.processInfo.environment["WINEMETALGL_SETTINGS_HOME"]
        .flatMap { $0.isEmpty ? nil : URL(fileURLWithPath: $0, isDirectory: true) }
    static let home = testHome ?? FileManager.default.homeDirectoryForCurrentUser
    static let defaults = testHome == nil ? UserDefaults.standard
        : UserDefaults(suiteName: "com.metalsharp.winemetalgl.settings.test") ?? .standard
    /// For the scripts the app runs.
    static var processEnvironment: [String: String] {
        var environment = ProcessInfo.processInfo.environment
        if let testHome { environment["HOME"] = testHome.path }
        return environment
    }
}

// MARK: - The settings file

enum Upscale: String, CaseIterable, Identifiable {
    case off, quality, balanced, performance

    var id: String { rawValue }

    var title: String {
        switch self {
        case .off: "Off"
        case .quality: "Quality"
        case .balanced: "Balanced"
        case .performance: "Performance"
        }
    }

    var detail: String {
        switch self {
        case .off: "Games render at your screen's resolution."
        case .quality: "Fullscreen games render at 67% of your screen and MetalFX scales them up to it."
        case .balanced: "Fullscreen games render at 58% of your screen and MetalFX scales them up to it."
        case .performance: "Fullscreen games render at half your screen and MetalFX scales them up to it."
        }
    }
}

/// ~/Library/Application Support/WineMetalGL/settings.conf: NAME=value lines.
/// Only the settings this app shows are rewritten; other lines stay.
@MainActor
final class SettingsStore: ObservableObject {
    @Published var upscale: Upscale = .off { didSet { if upscale != oldValue { save() } } }
    @Published var logging = false {
        didSet {
            guard logging != oldValue else { return }
            if !loading { loggingChosen = true }
            save()
        }
    }
    /// Logging is in the file (chosen here or there); until then WineMetalGL
    /// keeps its default, warnings only, and the file says nothing of it.
    @Published private(set) var loggingChosen = false
    @Published var x87 = false {
        didSet {
            guard x87 != oldValue else { return }
            if !loading { x87Chosen = true }
            save()
        }
    }
    /// x87 acceleration is in the file (chosen here or there).
    private var x87Chosen = false

    let fileURL: URL
    let logsURL: URL
    private var lines: [String] = []
    private var loading = false

    init() {
        let home = AppEnvironment.home
        fileURL = home.appendingPathComponent("Library/Application Support/WineMetalGL/settings.conf")
        logsURL = home.appendingPathComponent("Library/Logs/WineMetalGL", isDirectory: true)
        load()
    }

    private static let managed: Set<String> = ["WINEMETALGL_UPSCALE", "WINEMETALGL_LOG", "WINEMETALGL_LOG_FILE", "WINEMETALGL_X87"]

    private static func entry(_ line: String) -> (name: String, value: String)? {
        let text = line.trimmingCharacters(in: .whitespaces)
        guard !text.hasPrefix("#"), let equals = text.firstIndex(of: "=") else { return nil }
        return (text[..<equals].trimmingCharacters(in: .whitespaces),
                text[text.index(after: equals)...].trimmingCharacters(in: .whitespaces))
    }

    func load() {
        loading = true
        defer { loading = false }
        let text = (try? String(contentsOf: fileURL, encoding: .utf8)) ?? ""
        lines = text.split(separator: "\n", omittingEmptySubsequences: false).map(String.init)
        if lines.last == "" { lines.removeLast() }
        var values: [String: String] = [:]
        for line in lines { if let e = Self.entry(line) { values[e.name] = e.value } }
        upscale = Upscale(rawValue: (values["WINEMETALGL_UPSCALE"] ?? "off").lowercased()) ?? .off
        let level = (values["WINEMETALGL_LOG"] ?? "").lowercased()
        loggingChosen = !level.isEmpty
        logging = !level.isEmpty && !["off", "none", "-1", "0", "1", "err", "error", "warn", "warning"].contains(level)
        x87Chosen = values["WINEMETALGL_X87"] != nil
        x87 = values["WINEMETALGL_X87"] == "1"
    }

    private func save() {
        guard !loading else { return }
        var kept = lines.filter { line in
            guard let e = Self.entry(line) else { return true }
            return !Self.managed.contains(e.name)
        }
        if kept.isEmpty { kept = ["# WineMetalGL settings, written by WineMetalGL Settings.", "# The environment wins over these."] }
        if upscale != .off { kept.append("WINEMETALGL_UPSCALE=\(upscale.rawValue)") }
        if logging {
            kept.append("WINEMETALGL_LOG=2")
        } else if loggingChosen {
            kept.append("WINEMETALGL_LOG=off")
            kept.append("WINEMETALGL_LOG_FILE=0")
        }
        if x87Chosen { kept.append("WINEMETALGL_X87=\(x87 ? 1 : 0)") }
        lines = kept
        do {
            try FileManager.default.createDirectory(at: fileURL.deletingLastPathComponent(), withIntermediateDirectories: true)
            try (kept.joined(separator: "\n") + "\n").write(to: fileURL, atomically: true, encoding: .utf8)
        } catch {
            NSAlert(error: error).runModal()
        }
    }

    func openLogs() {
        try? FileManager.default.createDirectory(at: logsURL, withIntermediateDirectories: true)
        NSWorkspace.shared.open(logsURL)
    }
}

// MARK: - Liquid Glass, with a material before macOS 26

extension View {
    @ViewBuilder func glassCard() -> some View {
        if #available(macOS 26.0, *) {
            self.padding(16).glassEffect(.regular, in: .rect(cornerRadius: 22))
        } else {
            self.padding(16).background(.regularMaterial, in: RoundedRectangle(cornerRadius: 22))
        }
    }

    @ViewBuilder func glassButton(prominent: Bool) -> some View {
        if #available(macOS 26.0, *) {
            if prominent { self.buttonStyle(.glassProminent) } else { self.buttonStyle(.glass) }
        } else {
            if prominent { self.buttonStyle(.borderedProminent) } else { self.buttonStyle(.bordered) }
        }
    }
}

// MARK: - A card that opens

/// A card whose title row opens and closes the rest of it; every card starts
/// closed. The row keeps the card's switch (`accessory`) and, closed, a
/// short summary.
struct CollapsibleCard<Accessory: View, Content: View>: View {
    let title: String
    let systemImage: String
    let summary: String
    @ViewBuilder let accessory: Accessory
    @ViewBuilder let content: Content
    // A state object, not @State: the Command Line Tools' SDK has no plugin for its macro.
    @StateObject private var state = Openness()

    final class Openness: ObservableObject { @Published var open = false }
    private var open: Bool { state.open }

    init(_ title: String, systemImage: String, summary: String = "",
         @ViewBuilder accessory: () -> Accessory, @ViewBuilder content: () -> Content) {
        self.title = title
        self.systemImage = systemImage
        self.summary = summary
        self.accessory = accessory()
        self.content = content()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 8) {
                Button {
                    withAnimation(.snappy(duration: 0.2)) { state.open.toggle() }
                } label: {
                    HStack(spacing: 8) {
                        Image(systemName: "chevron.right")
                            .font(.caption.weight(.semibold))
                            .foregroundStyle(.secondary)
                            .rotationEffect(.degrees(open ? 90 : 0))
                            .frame(width: 10)
                        Label(title, systemImage: systemImage).font(.headline)
                        if !open && !summary.isEmpty {
                            Text(summary)
                                .font(.callout)
                                .foregroundStyle(.secondary)
                                .lineLimit(1)
                                .truncationMode(.tail)
                        }
                        Spacer(minLength: 0)
                    }
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel(title)
                .accessibilityValue(open ? "Expanded" : "Collapsed")
                accessory
            }
            if open { content }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .glassCard()
    }
}

// MARK: - The window

struct SettingsView: View {
    @StateObject private var store = SettingsStore()
    @StateObject private var installations = Installations()
    @StateObject private var fixes = GamePatches()
    @StateObject private var updater = Updater()

    private var version: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? ""
    }

    var body: some View {
        VStack(spacing: 14) {
            VStack(spacing: 6) {
                if let logo = Bundle.main.image(forResource: "MetalSharpLogo") {
                    Image(nsImage: logo)
                        .resizable()
                        .interpolation(.high)
                        .frame(width: 76, height: 76)
                        .shadow(color: .black.opacity(0.25), radius: 8, y: 3)
                }
                Text("WineMetalGL").font(.title2.weight(.semibold))
                Text(version.isEmpty ? "OpenGL on Metal" : "OpenGL on Metal · \(version)")
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }
            .padding(.top, 4)

            if let status = updater.status {
                HStack(spacing: 8) {
                    ProgressView().controlSize(.small)
                    Text(status).font(.callout).foregroundStyle(.secondary)
                }
            }

            InstallationsCard(installations: installations)

            GameFixesCard(fixes: fixes, installations: installations)

            CollapsibleCard("UpscaleGL", systemImage: "arrow.up.left.and.arrow.down.right", summary: store.upscale.title) {
                EmptyView()
            } content: {
                HStack(spacing: 6) {
                    ForEach(Upscale.allCases) { preset in
                        let selected = store.upscale == preset
                        Button {
                            store.upscale = preset
                        } label: {
                            if selected {
                                Label(preset.title, systemImage: "checkmark").fontWeight(.semibold)
                            } else {
                                Text(preset.title)
                            }
                        }
                        .glassButton(prominent: selected)
                        .tint(selected ? Color.accentColor : nil)
                        .controlSize(.large)
                        .accessibilityAddTraits(selected ? .isSelected : [])
                    }
                }
                Text(store.upscale.detail)
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }

            CollapsibleCard("x87 Acceleration", systemImage: "bolt") {
                Toggle("x87 Acceleration", isOn: $store.x87)
                    .toggleStyle(.switch)
                    .labelsHidden()
            } content: {
                Text(store.x87 ? "32-bit games get Rosetta's fast x87 math (x87sidecar), unless their Wine already gives it to them."
                     : "32-bit games run their x87 math the way Rosetta does it alone, which is much slower.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }

            CollapsibleCard("Logging", systemImage: "text.alignleft") {
                Toggle("Logging", isOn: $store.logging)
                    .toggleStyle(.switch)
                    .labelsHidden()
            } content: {
                Text(store.logging ? "WineMetalGL logs everything it does, one file per game run."
                     : store.loggingChosen ? "WineMetalGL writes no log files."
                     : "WineMetalGL logs only warnings, one file per game run.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                Button {
                    store.openLogs()
                } label: {
                    Label("Open Log Folder", systemImage: "folder")
                }
                .glassButton(prominent: false)
            }

            Text("Changes apply to games started after them.")
                .font(.footnote)
                .foregroundStyle(.tertiary)
        }
        .padding(20)
        .frame(width: 470)
        .task { await launched() }
        .alert(item: $updater.report) { report in
            Alert(title: Text(report.title), message: Text(report.text))
        }
    }

    /// After an update, the new release goes into every Wine WineMetalGL is
    /// in; then a newer release is looked for, and the game fixes applied.
    private func launched() async {
        if let version = updater.takePendingModuleUpdate() {
            await installations.updateEverywhere(to: version)
        }
        if await updater.offerUpdate() { return }
        await fixes.apply(to: installations.targets)
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

@main
struct WineMetalGLSettingsApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate

    var body: some Scene {
        Window("WineMetalGL Settings", id: "settings") {
            if #available(macOS 15.0, *) {
                SettingsView().containerBackground(.thinMaterial, for: .window)
            } else {
                SettingsView()
            }
        }
        .windowStyle(.hiddenTitleBar)
        .windowResizability(.contentSize)
    }
}
