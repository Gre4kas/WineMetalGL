// Where WineMetalGL is installed, switching it on and off there, and
// installing it: CrossOver bottles, MetalSharp's prefixes (its Steam and GOG
// ones), ~/.wine, and the
// prefixes installed into from here. On and off is the prefix's DLL
// override, opengl32=native (WineMetalGL) or builtin (Wine's own OpenGL),
// set through that Wine's `reg`. Installing runs the release's own scripts,
// which the app carries (Contents/Resources/Installer: install-crossover.sh,
// install-wine-module.sh, and the release's module directory when the app was
// built with it; without it, the scripts download the latest release).

import AppKit
import SwiftUI

struct WineTarget: Identifiable, Hashable, Sendable {
    enum Kind { case crossover, prefix }
    var kind: Kind
    var name: String
    var prefix: URL      // the bottle's directory, or the WINEPREFIX
    var wine: URL?       // a prefix's Wine (the directory with bin/wine); CrossOver's own for a bottle
    var installed: Bool  // winemetalgl.dll is in its system32
    var enabled: Bool    // opengl32=native
    var id: String { prefix.path }
}

@MainActor
final class Installations: ObservableObject {
    @Published private(set) var targets: [WineTarget] = []
    @Published private(set) var busy: String?
    @Published var report: Report?

    struct Report: Identifiable {
        let id = UUID()
        let title: String
        let text: String
    }

    private let home = AppEnvironment.home
    private let savedKey = "installedPrefixes"  // [[prefix, wine]] installed into from here

    /// CrossOver's Contents/SharedSupport/CrossOver.
    var crossover: URL? {
        ["/Applications/CrossOver.app", home.appendingPathComponent("Applications/CrossOver.app").path]
            .map { URL(fileURLWithPath: $0).appendingPathComponent("Contents/SharedSupport/CrossOver") }
            .first { FileManager.default.isExecutableFile(atPath: $0.appendingPathComponent("bin/wine").path) }
    }

    /// The release carried with the scripts (else they download it).
    var carriesRelease: Bool {
        guard let installer else { return false }
        return FileManager.default.fileExists(atPath: installer.appendingPathComponent("module").path)
    }

    /// The install scripts the app carries.
    var installer: URL? {
        guard let dir = Bundle.main.resourceURL?.appendingPathComponent("Installer"),
              FileManager.default.fileExists(atPath: dir.appendingPathComponent("install-wine-module.sh").path)
        else { return nil }
        return dir
    }

    var bottles: [WineTarget] { targets.filter { $0.kind == .crossover } }
    var anyInstalled: Bool { targets.contains { $0.installed } }
    /// An install button shows only until that kind of install exists: a
    /// bottle with WineMetalGL hides Install into CrossOver (as does having no
    /// bottles), a prefix with it Install Normally.
    var offersCrossOver: Bool { !bottles.isEmpty && !bottles.contains { $0.installed } }
    var offersNormal: Bool { !targets.contains { $0.kind == .prefix && $0.installed } }

    init() { refresh() }

    // MARK: Finding them

    func refresh() {
        let fm = FileManager.default
        var found: [WineTarget] = []
        let bottlesDir = home.appendingPathComponent("Library/Application Support/CrossOver/Bottles")
        for bottle in (try? fm.contentsOfDirectory(at: bottlesDir, includingPropertiesForKeys: nil)) ?? []
        where fm.fileExists(atPath: bottle.appendingPathComponent("cxbottle.conf").path) {
            found.append(target(.crossover, name: bottle.lastPathComponent, prefix: bottle, wine: crossover))
        }
        let metalsharp = home.appendingPathComponent(".metalsharp")
        let metalsharpWine = metalsharp.appendingPathComponent("runtime/wine")
        for dir in (try? fm.contentsOfDirectory(at: metalsharp, includingPropertiesForKeys: nil)) ?? []
        where dir.lastPathComponent.hasPrefix("prefix-") && fm.fileExists(atPath: dir.appendingPathComponent("drive_c").path) {
            let name = "MetalSharp " + dir.lastPathComponent.dropFirst("prefix-".count)
            found.append(target(.prefix, name: name, prefix: dir, wine: metalsharpWine))
        }
        // MetalSharp's GOG games run in bottles/gog-prefix/prefix.
        let bottlesPrefixes = metalsharp.appendingPathComponent("bottles")
        for dir in (try? fm.contentsOfDirectory(at: bottlesPrefixes, includingPropertiesForKeys: nil)) ?? []
        where fm.fileExists(atPath: dir.appendingPathComponent("prefix/drive_c").path) {
            found.append(target(.prefix, name: "MetalSharp " + dir.lastPathComponent,
                                prefix: dir.appendingPathComponent("prefix"), wine: metalsharpWine))
        }
        let defaultPrefix = home.appendingPathComponent(".wine")
        if fm.fileExists(atPath: defaultPrefix.appendingPathComponent("drive_c").path) {
            found.append(target(.prefix, name: "Default prefix", prefix: defaultPrefix, wine: Self.wineApps().first))
        }
        for pair in AppEnvironment.defaults.array(forKey: savedKey) as? [[String]] ?? [] where pair.count == 2 {
            let prefix = URL(fileURLWithPath: pair[0])
            guard !found.contains(where: { $0.prefix.path == prefix.path }),
                  fm.fileExists(atPath: prefix.appendingPathComponent("drive_c").path) else { continue }
            found.append(target(.prefix, name: prefix.lastPathComponent, prefix: prefix, wine: URL(fileURLWithPath: pair[1])))
        }
        targets = found
    }

    private func target(_ kind: WineTarget.Kind, name: String, prefix: URL, wine: URL?) -> WineTarget {
        let system32 = prefix.appendingPathComponent("drive_c/windows/system32/winemetalgl.dll")
        return WineTarget(kind: kind, name: name, prefix: prefix, wine: wine,
                          installed: FileManager.default.fileExists(atPath: system32.path),
                          enabled: Self.overrideIsNative(prefix: prefix))
    }

    /// WineHQ's apps' Wines (Contents/Resources/wine).
    static func wineApps() -> [URL] {
        ["Wine Stable", "Wine Devel", "Wine Staging"].map {
            URL(fileURLWithPath: "/Applications/\($0).app/Contents/Resources/wine")
        }.filter { FileManager.default.isExecutableFile(atPath: $0.appendingPathComponent("bin/wine").path) }
    }

    /// The prefix's opengl32 override, from user.reg (as wineserver last saved it).
    static func overrideIsNative(prefix: URL) -> Bool {
        guard let text = try? String(contentsOf: prefix.appendingPathComponent("user.reg"), encoding: .utf8) else { return false }
        var inOverrides = false
        for line in text.split(separator: "\n") {
            if line.hasPrefix("[") {
                inOverrides = line.lowercased().hasPrefix("[software\\\\wine\\\\dlloverrides]")
            } else if inOverrides, line.lowercased().hasPrefix("\"opengl32\"=") {
                return line.dropFirst("\"opengl32\"=".count).lowercased().hasPrefix("\"native")
            }
        }
        return false
    }

    // MARK: On and off

    func setEnabled(_ target: WineTarget, _ on: Bool) {
        guard let wine = target.wine else {
            report = Report(title: "No Wine for \(target.name)",
                            text: "WineMetalGL Settings does not know which Wine runs \(target.prefix.path).")
            return
        }
        let value = on ? "native" : "builtin"
        var arguments = ["reg", "add", "HKCU\\Software\\Wine\\DllOverrides", "/v", "opengl32", "/d", value, "/f"]
        var environment = ["WINEDEBUG": "-all"]
        if target.kind == .crossover {
            arguments = ["--bottle", target.prefix.path, "--wait-children"] + arguments
        } else {
            environment["WINEPREFIX"] = target.prefix.path
        }
        let executable = wine.appendingPathComponent("bin/wine")
        run(on ? "Turning WineMetalGL on in \(target.name)…" : "Turning WineMetalGL off in \(target.name)…",
            executable, arguments, environment) { [weak self] ok, output in
            guard let self else { return }
            if ok {
                // user.reg follows when wineserver next saves it.
                if let i = self.targets.firstIndex(where: { $0.id == target.id }) { self.targets[i].enabled = on }
            } else {
                self.report = Report(title: "Could not switch WineMetalGL in \(target.name)", text: output)
            }
        }
    }

    // MARK: Installing

    func installCrossOver(_ bottle: WineTarget) {
        guard let installer else { return }
        run("Installing WineMetalGL into \(bottle.name)…", URL(fileURLWithPath: "/bin/sh"),
            [installer.appendingPathComponent("install-crossover.sh").path, bottle.prefix.path], [:]) { [weak self] ok, output in
            self?.finishInstall(ok, output, into: bottle.name)
        }
    }

    /// After an update (Updater.swift): this app's release, installed again
    /// into every bottle and prefix that has WineMetalGL, the way it was
    /// installed there.
    func updateEverywhere(to version: String) async {
        refresh()
        guard let installer else { return }
        var failed: [String] = []
        var updated: [String] = []
        for target in targets where target.installed {
            let arguments: [String]
            if target.kind == .crossover {
                arguments = [installer.appendingPathComponent("install-crossover.sh").path, target.prefix.path]
            } else if let wine = target.wine {
                arguments = [installer.appendingPathComponent("install-wine-module.sh").path, wine.path, target.prefix.path]
            } else {
                failed.append("\(target.name): WineMetalGL Settings does not know which Wine runs it.")
                continue
            }
            let (ok, output) = await perform("Updating WineMetalGL in \(target.name)…", URL(fileURLWithPath: "/bin/sh"), arguments, [:])
            if ok { updated.append(target.name) } else { failed.append("\(target.name): \(output)") }
        }
        refresh()
        if !failed.isEmpty {
            report = Report(title: "WineMetalGL \(version) could not be installed everywhere",
                            text: failed.joined(separator: "\n\n"))
        } else if !updated.isEmpty {
            report = Report(title: "WineMetalGL Settings \(version) is installed",
                            text: "WineMetalGL \(version) is now in " + ListFormatter.localizedString(byJoining: updated) + ".")
        } else {
            report = Report(title: "WineMetalGL Settings \(version) is installed", text: "WineMetalGL is not installed in any Wine yet.")
        }
    }

    func installNormally() {
        guard let installer else { return }
        let winePanel = NSOpenPanel()
        winePanel.message = "Choose the Wine to install WineMetalGL into: a Wine app (such as Wine Stable) or the folder with bin/wine."
        winePanel.prompt = "Choose Wine"
        winePanel.canChooseDirectories = true
        winePanel.canChooseFiles = true
        winePanel.allowedContentTypes = [.applicationBundle, .folder]
        winePanel.directoryURL = URL(fileURLWithPath: "/Applications")
        guard winePanel.runModal() == .OK, var wine = winePanel.url else { return }
        if wine.pathExtension == "app" { wine = wine.appendingPathComponent("Contents/Resources/wine") }
        guard FileManager.default.isExecutableFile(atPath: wine.appendingPathComponent("bin/wine").path) else {
            report = Report(title: "Not a Wine", text: "\(wine.path) has no bin/wine.")
            return
        }
        let prefixPanel = NSOpenPanel()
        prefixPanel.message = "Choose the Wine prefix whose games should use WineMetalGL (a new folder becomes a new prefix)."
        prefixPanel.prompt = "Choose Prefix"
        prefixPanel.canChooseDirectories = true
        prefixPanel.canChooseFiles = false
        prefixPanel.canCreateDirectories = true
        prefixPanel.showsHiddenFiles = true
        prefixPanel.directoryURL = home
        guard prefixPanel.runModal() == .OK, let prefix = prefixPanel.url else { return }
        run("Installing WineMetalGL into \(prefix.lastPathComponent)…", URL(fileURLWithPath: "/bin/sh"),
            [installer.appendingPathComponent("install-wine-module.sh").path, wine.path, prefix.path], [:]) { [weak self] ok, output in
            guard let self else { return }
            if ok {
                var saved = AppEnvironment.defaults.array(forKey: self.savedKey) as? [[String]] ?? []
                saved.removeAll { $0.first == prefix.path }
                saved.append([prefix.path, wine.path])
                AppEnvironment.defaults.set(saved, forKey: self.savedKey)
            }
            self.finishInstall(ok, output, into: prefix.lastPathComponent)
        }
    }

    private func finishInstall(_ ok: Bool, _ output: String, into name: String) {
        refresh()
        report = ok ? Report(title: "WineMetalGL is installed in \(name)", text: "Games started there now use it.")
                    : Report(title: "Installing into \(name) failed", text: output)
    }

    // MARK: Running

    private func run(_ message: String, _ executable: URL, _ arguments: [String], _ environment: [String: String],
                     then done: @escaping @MainActor (Bool, String) -> Void) {
        Task {
            let (ok, output) = await self.perform(message, executable, arguments, environment)
            done(ok, output)
        }
    }

    private func perform(_ message: String, _ executable: URL, _ arguments: [String],
                         _ environment: [String: String]) async -> (Bool, String) {
        busy = message
        defer { busy = nil }
        let (status, output) = await Self.execute(executable, arguments, environment)
        // The end of the output says what went wrong.
        return (status == 0, String(output.suffix(1500)).trimmingCharacters(in: .whitespacesAndNewlines))
    }

    /// Runs a program off the main thread: its exit status and output.
    nonisolated static func execute(_ executable: URL, _ arguments: [String],
                                    _ environment: [String: String] = [:]) async -> (Int32, String) {
        await Task.detached { executeNow(executable, arguments, environment) }.value
    }

    nonisolated static func executeNow(_ executable: URL, _ arguments: [String],
                                       _ environment: [String: String] = [:]) -> (Int32, String) {
        let process = Process()
        process.executableURL = executable
        process.arguments = arguments
        process.environment = AppEnvironment.processEnvironment.merging(environment) { $1 }
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        do {
            try process.run()
            let data = pipe.fileHandleForReading.readDataToEndOfFile()
            process.waitUntilExit()
            return (process.terminationStatus, String(decoding: data, as: UTF8.self))
        } catch {
            return (-1, error.localizedDescription)
        }
    }
}

// MARK: - The card

struct InstallationsCard: View {
    @ObservedObject var installations: Installations

    private var summary: String {
        if let busy = installations.busy { return busy }
        let count = installations.targets.filter(\.installed).count
        return count == 0 ? "Not installed" : count == 1 ? "Installed in 1 Wine" : "Installed in \(count) Wines"
    }

    var body: some View {
        CollapsibleCard("WineMetalGL", systemImage: "shippingbox", summary: summary) {
            Button {
                installations.refresh()
            } label: {
                Image(systemName: "arrow.clockwise")
            }
            .buttonStyle(.borderless)
            .help("Look again")
        } content: {
            if !installations.anyInstalled {
                Text("WineMetalGL is not installed in any Wine yet.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }
            if !installations.targets.isEmpty {
                ScrollView {
                    VStack(spacing: 8) {
                        ForEach(installations.targets) { target in row(target) }
                    }
                }
                .frame(height: min(CGFloat(installations.targets.count) * 40, 168))
            }
            if installations.offersCrossOver || installations.offersNormal {
                HStack(spacing: 8) {
                    if installations.offersCrossOver {
                        Menu {
                            ForEach(installations.bottles) { bottle in
                                Button(bottle.name) { installations.installCrossOver(bottle) }
                            }
                        } label: {
                            Label("Install into CrossOver", systemImage: "wineglass")
                        }
                        .glassButton(prominent: !installations.anyInstalled)
                        .fixedSize()
                        .disabled(installations.installer == nil || installations.busy != nil)
                        .help("Choose a bottle.")
                    }
                    if installations.offersNormal {
                        Button {
                            installations.installNormally()
                        } label: {
                            Label("Install Normally", systemImage: "square.and.arrow.down")
                        }
                        .glassButton(prominent: !installations.anyInstalled)
                        .disabled(installations.installer == nil || installations.busy != nil)
                        .help("Choose a Wine and a prefix.")
                    }
                }
            }
            if installations.offersCrossOver || installations.offersNormal {
                Text(installations.installer == nil ? "This build of WineMetalGL Settings cannot install WineMetalGL."
                     : installations.carriesRelease ? "Installs the WineMetalGL that comes with this app."
                     : "Downloads the latest WineMetalGL release from GitHub to install.")
                    .font(.footnote)
                    .foregroundStyle(.tertiary)
            }
            if let busy = installations.busy {
                HStack(spacing: 8) {
                    ProgressView().controlSize(.small)
                    Text(busy).font(.callout).foregroundStyle(.secondary)
                }
            }
        }
        .alert(item: $installations.report) { report in
            Alert(title: Text(report.title), message: Text(report.text))
        }
    }

    private func row(_ target: WineTarget) -> some View {
        HStack(spacing: 10) {
            Image(systemName: target.kind == .crossover ? "wineglass" : "folder")
                .foregroundStyle(.secondary)
                .frame(width: 18)
            VStack(alignment: .leading, spacing: 1) {
                Text(target.name).lineLimit(1)
                Text(target.prefix.path.replacingOccurrences(of: NSHomeDirectory(), with: "~"))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
            Spacer()
            if target.installed {
                Toggle(target.name, isOn: Binding(get: { target.enabled },
                                                  set: { installations.setEnabled(target, $0) }))
                    .toggleStyle(.switch)
                    .labelsHidden()
                    .disabled(installations.busy != nil)
                    .help(target.enabled ? "WineMetalGL is on here." : "Wine's own OpenGL is used here.")
            } else {
                Text("Not installed").font(.caption).foregroundStyle(.secondary)
            }
        }
    }
}
