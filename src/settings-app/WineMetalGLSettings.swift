// WineMetalGL Settings: UpscaleGL's preset and logging, for programs
// launched without a way to set their environment. Both go to the settings
// file the core reads when it loads (src/core/util/settings.h); the
// environment still wins, and a change applies to games started afterwards.

import AppKit
import SwiftUI

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

    let fileURL: URL
    let logsURL: URL
    private var lines: [String] = []
    private var loading = false

    init() {
        let home = FileManager.default.homeDirectoryForCurrentUser
        fileURL = home.appendingPathComponent("Library/Application Support/WineMetalGL/settings.conf")
        logsURL = home.appendingPathComponent("Library/Logs/WineMetalGL", isDirectory: true)
        load()
    }

    private static let managed: Set<String> = ["WINEMETALGL_UPSCALE", "WINEMETALGL_LOG", "WINEMETALGL_LOG_FILE"]

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

// MARK: - The window

struct SettingsView: View {
    @StateObject private var store = SettingsStore()

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

            VStack(alignment: .leading, spacing: 10) {
                Label("UpscaleGL", systemImage: "arrow.up.left.and.arrow.down.right")
                    .font(.headline)
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
            .frame(maxWidth: .infinity, alignment: .leading)
            .glassCard()

            VStack(alignment: .leading, spacing: 10) {
                HStack {
                    Label("Logging", systemImage: "text.alignleft").font(.headline)
                    Spacer()
                    Toggle("Logging", isOn: $store.logging)
                        .toggleStyle(.switch)
                        .labelsHidden()
                }
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
            .frame(maxWidth: .infinity, alignment: .leading)
            .glassCard()

            Text("Changes apply to games started after them.")
                .font(.footnote)
                .foregroundStyle(.tertiary)
        }
        .padding(20)
        .frame(width: 470)
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
