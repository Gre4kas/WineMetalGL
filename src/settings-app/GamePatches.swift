// Game fixes: Game Specific Patched Files, which the app carries
// (Contents/Resources/GamePatches, scripts/build-settings-app.sh), applied
// where their games are installed in the bottles and prefixes it knows
// (Installations.swift), each the way its README says:
//
//   Quake 4: legacy-display-modes.reg into the prefix or bottle
//     (apply-display-modes.sh, which stops its wineserver: it waits while the
//     prefix's programs run), and autoexec.cfg into the game's q4base.
//   SpaceEngine: winemac.so, Wolfenstein: The New Order: xaudio2_7.dll, into
//     MetalSharp's Wine 11.17, the Wine they were built from; elsewhere they
//     are not applied.
//
// A fix that is in place already is left alone, so applying is done each time
// the app opens; a file it replaces is first copied to
// ~/Library/Application Support/WineMetalGL/Backups, under its own path (the
// first time only, so the backup is the file from before any fix). "Apply
// Game Fixes" turns all of it off.

import CryptoKit
import Foundation
import SwiftUI

struct GameFix: Identifiable, Sendable {
    enum State: Sendable { case applied, inPlace, waiting, notApplicable, failed }
    let game: String
    let place: String
    let state: State
    let detail: String
    var id: String { game + "\u{0}" + place }
}

@MainActor
final class GamePatches: ObservableObject {
    @Published var enabled: Bool {
        didSet { AppEnvironment.defaults.set(enabled, forKey: Self.enabledKey) }
    }
    @Published private(set) var results: [GameFix] = []
    @Published private(set) var busy = false
    private static let enabledKey = "applyGameFixes"

    init() {
        enabled = AppEnvironment.defaults.object(forKey: Self.enabledKey) as? Bool ?? true
    }

    var carried: URL? {
        guard let dir = Bundle.main.resourceURL?.appendingPathComponent("GamePatches"),
              FileManager.default.fileExists(atPath: dir.path) else { return nil }
        return dir
    }

    func apply(to targets: [WineTarget]) async {
        guard enabled, !busy, let carried else { return }
        busy = true
        defer { busy = false }
        let patcher = GamePatcher(fixes: carried, home: AppEnvironment.home)
        results = await Task.detached { patcher.apply(to: targets) }.value
    }
}

/// The work, off the main thread.
struct GamePatcher: Sendable {
    let fixes: URL
    let home: URL
    var backups: URL { home.appendingPathComponent("Library/Application Support/WineMetalGL/Backups", isDirectory: true) }

    enum Game: CaseIterable {
        case quake4, spaceEngine, wolfenstein

        var title: String {
            switch self {
            case .quake4: "Quake 4"
            case .spaceEngine: "SpaceEngine"
            case .wolfenstein: "Wolfenstein: The New Order"
            }
        }

        var folder: String {
            switch self {
            case .quake4: "Quake 4"
            case .spaceEngine: "SpaceEngine"
            case .wolfenstein: "Wolfenstein The New Order"
            }
        }

        /// The program that says the game is there (in its folder, or up to two below).
        var executable: String {
            switch self {
            case .quake4: "quake4.exe"
            case .spaceEngine: "spaceengine.exe"
            case .wolfenstein: "wolfneworder_x64.exe"
            }
        }

        /// For a fix to a Wine: the file it replaces, under lib/wine.
        var wineFile: String? {
            switch self {
            case .quake4: nil
            case .spaceEngine: "x86_64-unix/winemac.so"
            case .wolfenstein: "x86_64-windows/xaudio2_7.dll"
            }
        }
    }

    func apply(to targets: [WineTarget]) -> [GameFix] {
        var results: [GameFix] = []
        var seen = Set<String>()
        for target in targets {
            let found = GameFinder(home: home).games(in: target)
            for game in Game.allCases {
                for folder in found[game] ?? [] {
                    // A fix to a Wine is made (and shown) once, for the first prefix it was found in.
                    let key = game.wineFile != nil ? "\(game.folder)\u{0}\(target.wine?.standardizedFileURL.path ?? target.id)"
                                                   : "\(game.folder)\u{0}\(folder.path)"
                    guard seen.insert(key).inserted else { continue }
                    switch game {
                    case .quake4: results.append(quake4(target, folder))
                    case .spaceEngine, .wolfenstein: results.append(wineFix(game, target))
                    }
                }
            }
        }
        return results
    }

    // MARK: Quake 4

    private func quake4(_ target: WineTarget, _ folder: URL) -> GameFix {
        var done: [String] = []
        var inPlace: [String] = []
        var problems: [String] = []
        var waiting = false
        let source = fixes.appendingPathComponent("Quake 4")

        // autoexec.cfg: r_useSMP 1. One that the player has is kept and the
        // line added to it, unless it sets r_useSMP itself.
        if let base = Self.child(folder, named: "q4base") {
            let autoexec = base.appendingPathComponent("autoexec.cfg")
            let ours = (try? String(contentsOf: source.appendingPathComponent("autoexec.cfg"), encoding: .utf8)) ?? ""
            do {
                if let theirs = try? String(contentsOf: autoexec, encoding: .utf8) {
                    if theirs.lowercased().contains("r_usesmp") {
                        inPlace.append("autoexec.cfg")
                    } else {
                        try backUp(autoexec)
                        try (theirs + (theirs.hasSuffix("\n") || theirs.isEmpty ? "" : "\n") + ours)
                            .write(to: autoexec, atomically: true, encoding: .utf8)
                        done.append("autoexec.cfg")
                    }
                } else {
                    try ours.write(to: autoexec, atomically: true, encoding: .utf8)
                    done.append("autoexec.cfg")
                }
            } catch {
                problems.append("autoexec.cfg: \(error.localizedDescription)")
            }
        }

        // The display modes, prefix-wide.
        if Self.emulatesModeset(target.prefix) {
            inPlace.append("display modes")
        } else if Self.wineRunning(target.prefix) {
            waiting = true
        } else {
            var arguments = [source.appendingPathComponent("apply-display-modes.sh").path, target.prefix.path]
            if target.kind == .prefix, let wine = target.wine { arguments.append(wine.appendingPathComponent("bin/wine").path) }
            do {
                try backUp(target.prefix.appendingPathComponent("user.reg"))
                let (status, output) = Installations.executeNow(URL(fileURLWithPath: "/bin/sh"), arguments, ["WINEDEBUG": "-all"])
                if status == 0 {
                    done.append("display modes")
                } else {
                    problems.append("display modes: " + output.trimmingCharacters(in: .whitespacesAndNewlines).suffix(400))
                }
            } catch {
                problems.append("display modes: \(error.localizedDescription)")
            }
        }

        let state: GameFix.State = !problems.isEmpty ? .failed : !done.isEmpty ? .applied : waiting ? .waiting : .inPlace
        var detail: [String] = []
        if !done.isEmpty { detail.append("Applied " + ListFormatter.localizedString(byJoining: done) + ".") }
        if !inPlace.isEmpty && done.isEmpty && !waiting { detail.append("In place.") }
        if waiting { detail.append("The display modes apply once the programs running in \(target.name) quit.") }
        detail += problems
        return GameFix(game: Game.quake4.title, place: target.name, state: state, detail: detail.joined(separator: " "))
    }

    /// user.reg has [Software\\Wine\\X11 Driver] "EmulateModeset"="Y".
    static func emulatesModeset(_ prefix: URL) -> Bool {
        guard let text = try? String(contentsOf: prefix.appendingPathComponent("user.reg"), encoding: .utf8) else { return false }
        var inDriver = false
        for line in text.split(separator: "\n") {
            let lower = line.lowercased()
            if line.hasPrefix("[") {
                inDriver = lower.hasPrefix("[software\\\\wine\\\\x11 driver]")
            } else if inDriver, lower.hasPrefix("\"emulatemodeset\"=") {
                return ["\"y", "\"1", "\"t"].contains { lower.dropFirst("\"emulatemodeset\"=".count).hasPrefix($0) }
            }
        }
        return false
    }

    /// apply-display-modes.sh stops the prefix's wineserver, and with it what
    /// runs there, so it waits while the prefix's programs run: while its
    /// wineserver's socket is there, /tmp/.wine-<uid>/server-<device>-<inode>
    /// (of the prefix directory), and a wineserver runs.
    static func wineRunning(_ prefix: URL) -> Bool {
        var info = stat()
        guard stat(prefix.path, &info) == 0 else { return false }
        let socket = "/tmp/.wine-\(getuid())/server-\(String(UInt64(info.st_dev), radix: 16))-\(String(UInt64(info.st_ino), radix: 16))/socket"
        guard FileManager.default.fileExists(atPath: socket) else { return false }
        return Installations.executeNow(URL(fileURLWithPath: "/usr/bin/pgrep"), ["-x", "wineserver"]).0 == 0
    }

    // MARK: SpaceEngine, Wolfenstein: The New Order

    private func wineFix(_ game: Game, _ target: WineTarget) -> GameFix {
        func result(_ state: GameFix.State, _ detail: String) -> GameFix {
            GameFix(game: game.title, place: target.name, state: state, detail: detail)
        }
        let metalsharp = home.appendingPathComponent(".metalsharp/runtime/wine").standardizedFileURL.path
        guard target.kind == .prefix, let wine = target.wine, wine.standardizedFileURL.path == metalsharp else {
            return result(.notApplicable, "Its fix is built for MetalSharp's Wine 11.17; \(target.name) runs another Wine.")
        }
        let (status, version) = Installations.executeNow(wine.appendingPathComponent("bin/wine"), ["--version"], ["WINEDEBUG": "-all"])
        guard status == 0, version.hasPrefix("wine-11.17") else {
            return result(.notApplicable, "Its fix is built for MetalSharp's Wine 11.17, not \(version.trimmingCharacters(in: .whitespacesAndNewlines)).")
        }
        guard let file = game.wineFile else { return result(.failed, "No file.") }
        let source = fixes.appendingPathComponent(game.folder).appendingPathComponent((file as NSString).lastPathComponent)
        let destination = wine.appendingPathComponent("lib/wine").appendingPathComponent(file)
        guard FileManager.default.fileExists(atPath: destination.path) else {
            return result(.failed, "\(destination.path) is missing.")
        }
        let installed = Self.sha256(destination)
        let fixed = [Self.sha256(source), sourceHashes()["\(game.folder)/\(source.lastPathComponent)"]]
        if let installed, fixed.contains(installed) {
            return result(.inPlace, "In place in MetalSharp's Wine.")
        }
        do {
            try backUp(destination)
            try Self.replace(destination, with: source)
            // A copy keeps its signature (the release's Developer ID one); a
            // build without one gets an ad hoc one, as the install scripts do.
            if destination.pathExtension == "so",
               Installations.executeNow(URL(fileURLWithPath: "/usr/bin/codesign"), ["--verify", destination.path]).0 != 0 {
                _ = Installations.executeNow(URL(fileURLWithPath: "/usr/bin/codesign"), ["-f", "-s", "-", destination.path])
            }
            return result(.applied, "Installed \((file as NSString).lastPathComponent) into MetalSharp's Wine.")
        } catch {
            return result(.failed, error.localizedDescription)
        }
    }

    /// SOURCE-SHA256: the fixed files before they were signed for the app,
    /// as their folders' READMEs give them (installed by hand, they are in place).
    private func sourceHashes() -> [String: String] {
        let text = (try? String(contentsOf: fixes.appendingPathComponent("SOURCE-SHA256"), encoding: .utf8)) ?? ""
        var hashes: [String: String] = [:]
        for line in text.split(separator: "\n") {
            let parts = line.split(separator: " ", maxSplits: 1)
            guard parts.count == 2 else { continue }
            hashes[parts[1].trimmingCharacters(in: CharacterSet(charactersIn: " *"))] = String(parts[0])
        }
        return hashes
    }

    // MARK: Files

    /// The file as it was before any fix: copied the first time only.
    private func backUp(_ file: URL) throws {
        let fm = FileManager.default
        guard fm.fileExists(atPath: file.path) else { return }
        let backup = backups.appendingPathComponent(String(file.resolvingSymlinksInPath().path.dropFirst()))
        guard !fm.fileExists(atPath: backup.path) else { return }
        try fm.createDirectory(at: backup.deletingLastPathComponent(), withIntermediateDirectories: true)
        try fm.copyItem(at: file, to: backup)
    }

    /// In one step (rename), so a program that has the old one open keeps it.
    static func replace(_ destination: URL, with source: URL) throws {
        let fm = FileManager.default
        let staged = destination.deletingLastPathComponent().appendingPathComponent(".\(destination.lastPathComponent).winemetalgl")
        try? fm.removeItem(at: staged)
        try fm.copyItem(at: source, to: staged)
        if let mode = try fm.attributesOfItem(atPath: destination.path)[.posixPermissions] {
            try fm.setAttributes([.posixPermissions: mode], ofItemAtPath: staged.path)
        }
        guard rename(staged.path, destination.path) == 0 else {
            let error = NSError(domain: NSPOSIXErrorDomain, code: Int(errno))
            try? fm.removeItem(at: staged)
            throw error
        }
    }

    static func sha256(_ file: URL) -> String? {
        guard let handle = try? FileHandle(forReadingFrom: file) else { return nil }
        defer { try? handle.close() }
        var hash = SHA256()
        while let chunk = try? handle.read(upToCount: 1 << 20), !chunk.isEmpty { hash.update(data: chunk) }
        return hash.finalize().map { String(format: "%02x", $0) }.joined()
    }

    /// A directory's entry by name, whatever its case.
    static func child(_ dir: URL, named name: String) -> URL? {
        let entries = (try? FileManager.default.contentsOfDirectory(atPath: dir.path)) ?? []
        return entries.first { $0.lowercased() == name.lowercased() }.map { dir.appendingPathComponent($0) }
    }
}

/// Where a bottle or prefix has its games: Steam's libraries (from
/// libraryfolders.vdf, wherever they are), GOG's folders, Program Files, and
/// the game folders MetalSharp records for it.
struct GameFinder {
    let home: URL

    func games(in target: WineTarget) -> [GamePatcher.Game: [URL]] {
        var found: [GamePatcher.Game: [URL]] = [:]
        var seen = Set<String>()
        for folder in gameFolders(target.prefix) {
            for (game, at) in Self.look(in: folder, depth: 2) where seen.insert(game.folder + at.path).inserted {
                found[game, default: []].append(at)
            }
        }
        return found
    }

    /// Folders that may each be a game's.
    private func gameFolders(_ prefix: URL) -> [URL] {
        let fm = FileManager.default
        let driveC = prefix.appendingPathComponent("drive_c")
        var containers: [URL] = []
        var folders: [URL] = []
        for steam in ["Program Files (x86)/Steam", "Program Files/Steam"].map({ driveC.appendingPathComponent($0) })
        where fm.fileExists(atPath: steam.path) {
            var libraries = [steam]
            for vdf in ["steamapps/libraryfolders.vdf", "config/libraryfolders.vdf"] {
                let text = (try? String(contentsOf: steam.appendingPathComponent(vdf), encoding: .utf8)) ?? ""
                libraries += Self.vdfPaths(text).compactMap { unixPath($0, prefix: prefix) }
            }
            containers += libraries.map { $0.appendingPathComponent("steamapps/common") }
        }
        containers += ["GOG Games", "Program Files (x86)/GOG Galaxy/Games", "Program Files/GOG Galaxy/Games",
                       "Program Files (x86)", "Program Files", "Games"].map { driveC.appendingPathComponent($0) }
        folders += metalsharpFolders(prefix)

        for container in containers {
            for entry in (try? fm.contentsOfDirectory(at: container, includingPropertiesForKeys: [.isDirectoryKey])) ?? [] {
                if (try? entry.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true { folders.append(entry) }
            }
        }
        var seen = Set<String>()
        return folders.filter { seen.insert($0.resolvingSymlinksInPath().path).inserted }
    }

    /// MetalSharp's records: bottles/*/bottle.json (game_install_path, for
    /// its prefix_path) and, for the GOG prefix, gog/library.json.
    private func metalsharpFolders(_ prefix: URL) -> [URL] {
        let metalsharp = home.appendingPathComponent(".metalsharp")
        var folders: [URL] = []
        let bottles = metalsharp.appendingPathComponent("bottles")
        for bottle in (try? FileManager.default.contentsOfDirectory(at: bottles, includingPropertiesForKeys: nil)) ?? [] {
            guard let data = try? Data(contentsOf: bottle.appendingPathComponent("bottle.json")),
                  let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let prefixPath = json["prefix_path"] as? String,
                  URL(fileURLWithPath: prefixPath).standardizedFileURL.path == prefix.standardizedFileURL.path,
                  let path = json["game_install_path"] as? String, !path.isEmpty else { continue }
            folders.append(URL(fileURLWithPath: path))
        }
        if prefix.standardizedFileURL.path == bottles.appendingPathComponent("gog-prefix/prefix").standardizedFileURL.path,
           let data = try? Data(contentsOf: metalsharp.appendingPathComponent("gog/library.json")),
           let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
           let games = json["games"] as? [[String: Any]] {
            for game in games where game["installed"] as? Bool == true {
                if let path = game["gameFolder"] as? String, !path.isEmpty { folders.append(URL(fileURLWithPath: path)) }
            }
        }
        return folders
    }

    /// "path" values in a libraryfolders.vdf (C:\\Program Files (x86)\\Steam, Z:\\Volumes\\…).
    static func vdfPaths(_ text: String) -> [String] {
        text.split(separator: "\n").compactMap { line in
            let fields = line.split(separator: "\"").map(String.init).filter { !$0.trimmingCharacters(in: .whitespaces).isEmpty }
            guard fields.count == 2, fields[0].lowercased() == "path" else { return nil }
            return fields[1].replacingOccurrences(of: "\\\\", with: "\\")
        }
    }

    /// A Windows path in the prefix (C:\…, Z:\…), through its dosdevices.
    func unixPath(_ windows: String, prefix: URL) -> URL? {
        let chars = Array(windows)
        guard chars.count >= 2, chars[1] == ":", chars[0].isLetter else { return nil }
        let drive = prefix.appendingPathComponent("dosdevices/\(chars[0].lowercased()):")
        let rest = String(chars.dropFirst(2)).split(whereSeparator: { $0 == "\\" || $0 == "/" }).joined(separator: "/")
        return rest.isEmpty ? drive : drive.appendingPathComponent(rest)
    }

    /// The games whose program is in the folder, or in one up to `depth` below it.
    static func look(in folder: URL, depth: Int) -> [(GamePatcher.Game, URL)] {
        let fm = FileManager.default
        guard let entries = try? fm.contentsOfDirectory(at: folder, includingPropertiesForKeys: [.isDirectoryKey]) else { return [] }
        var found: [(GamePatcher.Game, URL)] = []
        let names = Set(entries.map { $0.lastPathComponent.lowercased() })
        for game in GamePatcher.Game.allCases where names.contains(game.executable) { found.append((game, folder)) }
        guard found.isEmpty, depth > 0 else { return found }
        for entry in entries where (try? entry.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true {
            let name = entry.lastPathComponent.lowercased()
            if name.hasPrefix("_") || name.hasPrefix(".") || name == "windows" { continue }
            found += look(in: entry, depth: depth - 1)
        }
        return found
    }
}

// MARK: - The card

struct GameFixesCard: View {
    @ObservedObject var fixes: GamePatches
    @ObservedObject var installations: Installations

    private var summary: String {
        if fixes.carried == nil || !fixes.enabled { return "Off" }
        if fixes.busy { return "Applying…" }
        let fixed = fixes.results.filter { $0.state == .applied || $0.state == .inPlace }.count
        if fixes.results.contains(where: { $0.state == .failed }) { return "A fix failed" }
        return fixed == 0 ? "No games to fix" : fixed == 1 ? "1 game fixed" : "\(fixed) fixed"
    }

    var body: some View {
        CollapsibleCard("Game Fixes", systemImage: "wrench.and.screwdriver", summary: summary) {
            Toggle("Apply Game Fixes", isOn: $fixes.enabled)
                .toggleStyle(.switch)
                .labelsHidden()
                .disabled(fixes.carried == nil)
                .onChange(of: fixes.enabled) { _, on in
                    if on { Task { await fixes.apply(to: installations.targets) } }
                }
        } content: {
            Text(fixes.carried == nil ? "This build of WineMetalGL Settings carries no game fixes."
                 : !fixes.enabled ? "Game fixes are not applied."
                 : fixes.results.isEmpty ? "Fixes Quake 4, SpaceEngine and Wolfenstein: The New Order where they are installed. None of them is installed here."
                 : "Fixes Quake 4, SpaceEngine and Wolfenstein: The New Order where they are installed, keeping a copy of each file it replaces.")
                .font(.callout)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            if fixes.enabled {
                ForEach(fixes.results) { fix in
                    HStack(alignment: .firstTextBaseline, spacing: 8) {
                        Image(systemName: symbol(fix.state)).foregroundStyle(color(fix.state)).frame(width: 18)
                        VStack(alignment: .leading, spacing: 1) {
                            Text("\(fix.game) · \(fix.place)").lineLimit(1)
                            Text(fix.detail)
                                .font(.caption)
                                .foregroundStyle(.secondary)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                }
            }
        }
    }

    private func symbol(_ state: GameFix.State) -> String {
        switch state {
        case .applied, .inPlace: "checkmark.circle"
        case .waiting: "clock"
        case .notApplicable: "minus.circle"
        case .failed: "exclamationmark.triangle"
        }
    }

    private func color(_ state: GameFix.State) -> Color {
        switch state {
        case .applied, .inPlace: .green
        case .waiting, .notApplicable: .secondary
        case .failed: .orange
        }
    }
}
