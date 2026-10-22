// Arm64 fixes: games that run natively on the arm64 core, outside Wine,
// through the glue in Game Specific Patched Files. The app carries the core
// and the glue prebuilt (Contents/Resources/arm64-core: lib/ and glue/,
// scripts/build-arm64-glue.sh), and each fix has a switch of its own:
//
//   Minecraft: the core and the glue go into ~/.local/share/winemetalgl/
//     minecraft, beside the SDL3 and GLFW of the player's LWJGL (from its
//     natives jars), and each Prism Launcher instance, official launcher
//     installation and Modrinth App instance gets the JVM arguments that load
//     them: the SDL3 glue for LWJGL 3.4 and later (Minecraft 26.x), the GLFW
//     glue before it. Their earlier arguments are kept, and recorded
//     (NativeFixes.json) so that turning the fix off restores them. A
//     launcher that is running would write its settings over the change: the
//     fix waits until it quits. (Modrinth's Java, Zulu, loads the glue as
//     Mojang's does: it may load libraries signed ad hoc.)
//   OpenMW: OpenMW.app (in /Applications or ~/Applications) gets the glue as
//     its libSDL2-2.0.0.dylib (the real one renamed libSDL2-real.dylib), the
//     GL stubs and the core, and openmw and OpenSceneGraph's libraries load
//     the stubs instead of OpenGL.framework (their load commands rewritten in
//     place: the Command Line Tools' install_name_tool may not be there). The
//     app is signed ad hoc again. Turning it off puts the files back.
//
// Files replaced are first copied to ~/Library/Application Support/
// WineMetalGL/Backups, as the Wine fixes' are (GamePatches.swift).

import Foundation
import SQLite3
import SwiftUI

@MainActor
final class NativeFixes: ObservableObject {
    @Published var minecraft: Bool {
        didSet { AppEnvironment.defaults.set(minecraft, forKey: Self.minecraftKey) }
    }
    @Published var openmw: Bool {
        didSet { AppEnvironment.defaults.set(openmw, forKey: Self.openmwKey) }
    }
    @Published private(set) var results: [GameFix] = []
    @Published private(set) var busy = false
    private static let minecraftKey = "applyMinecraftFix"
    private static let openmwKey = "applyOpenMWFix"

    init() {
        minecraft = AppEnvironment.defaults.object(forKey: Self.minecraftKey) as? Bool ?? false
        openmw = AppEnvironment.defaults.object(forKey: Self.openmwKey) as? Bool ?? false
    }

    /// The carried arm64 core and its glue.
    var carried: URL? {
        guard let dir = Bundle.main.resourceURL?.appendingPathComponent("arm64-core"),
              FileManager.default.fileExists(atPath: dir.appendingPathComponent("lib/libwinemetalgl.dylib").path),
              FileManager.default.fileExists(atPath: dir.appendingPathComponent("glue").path) else { return nil }
        return dir
    }

    func apply() async {
        guard !busy, let carried else { return }
        busy = true
        defer { busy = false }
        let patcher = NativePatcher(carried: carried, home: AppEnvironment.home)
        let (minecraft, openmw) = (self.minecraft, self.openmw)
        results = await Task.detached { patcher.apply(minecraft: minecraft, openmw: openmw) }.value
        // A test home's run says what it did (WINEMETALGL_SETTINGS_HOME).
        if AppEnvironment.testHome != nil {
            for fix in results {
                FileHandle.standardError.write(Data("arm64 fix: \(fix.game) · \(fix.place): \(fix.state) \(fix.detail)\n".utf8))
            }
        }
    }
}

/// The work, off the main thread.
struct NativePatcher: Sendable {
    let carried: URL
    let home: URL

    static let minecraftTitle = "Minecraft"
    static let openmwTitle = "OpenMW"

    var core: URL { carried.appendingPathComponent("lib/libwinemetalgl.dylib") }
    var support: URL { home.appendingPathComponent("Library/Application Support/WineMetalGL", isDirectory: true) }
    var backups: URL { support.appendingPathComponent("Backups", isDirectory: true) }
    var records: URL { support.appendingPathComponent("NativeFixes.json") }
    var minecraftLibraries: URL { home.appendingPathComponent(".local/share/winemetalgl/minecraft", isDirectory: true) }
    var minecraftHome: URL { home.appendingPathComponent("Library/Application Support/minecraft", isDirectory: true) }
    var prismHome: URL { home.appendingPathComponent("Library/Application Support/PrismLauncher", isDirectory: true) }

    func apply(minecraft: Bool, openmw: Bool) -> [GameFix] {
        minecraftFix(minecraft) + openmwFix(openmw)
    }

    // MARK: Minecraft

    enum Glue: Sendable {
        case sdl3, glfw

        var library: String { self == .sdl3 ? "libSDL3.dylib" : "libglfw.dylib" }
        var property: String { self == .sdl3 ? "org.lwjgl.sdl.libname" : "org.lwjgl.glfw.libname" }
        var module: String { self == .sdl3 ? "lwjgl-sdl" : "lwjgl-glfw" }
        var real: String { self == .sdl3 ? "libSDL3-real.dylib" : "libglfw-real.dylib" }
        var jarEntry: String { self == .sdl3 ? "macos/arm64/org/lwjgl/sdl/libSDL3.dylib" : "macos/arm64/org/lwjgl/glfw/libglfw.dylib" }
    }

    /// The JVM arguments that load the glue: Java options split at spaces,
    /// so the libraries' folder must have none.
    func arguments(_ glue: Glue) -> [String] {
        ["-D\(glue.property)=\(minecraftLibraries.path)/\(glue.library)",
         "-Dorg.lwjgl.opengl.libname=\(minecraftLibraries.path)/libwmglhost.dylib"]
    }

    /// Whether a JVM argument is one of the fix's (any glue, any version of it).
    func isOurs(_ argument: Substring) -> Bool {
        argument.hasPrefix("-Dorg.lwjgl.") && argument.contains(".libname=") && argument.contains(minecraftLibraries.path)
    }

    private func minecraftFix(_ on: Bool) -> [GameFix] {
        var record = Records(file: records)
        defer { record.save() }
        var results: [GameFix] = []
        let prismRunning = Self.running("Prism Launcher.app/Contents/MacOS/prismlauncher")
        let launcherRunning = Self.running("Minecraft.app/Contents/MacOS/launcher")
        let modrinthRunning = Self.running("Modrinth App.app/Contents/MacOS/Modrinth App")

        guard on else {
            // Off: the launchers get their own arguments back.
            for (cfg, saved) in record.prism {
                let name = "Prism · " + (Ini(URL(fileURLWithPath: cfg)).value("name")
                    ?? URL(fileURLWithPath: cfg).deletingLastPathComponent().lastPathComponent)
                if prismRunning {
                    results.append(GameFix(game: Self.minecraftTitle, place: name, state: .waiting,
                                           detail: "Its own JVM arguments come back once Prism Launcher quits."))
                    continue
                }
                do {
                    try restorePrism(URL(fileURLWithPath: cfg), saved)
                    record.prism[cfg] = nil
                    results.append(GameFix(game: Self.minecraftTitle, place: name, state: .notApplicable,
                                           detail: "Removed: its own JVM arguments are back."))
                } catch {
                    results.append(GameFix(game: Self.minecraftTitle, place: name, state: .failed, detail: error.localizedDescription))
                }
            }
            if !record.launcher.isEmpty {
                let file = minecraftHome.appendingPathComponent("launcher_profiles.json")
                if launcherRunning {
                    results.append(GameFix(game: Self.minecraftTitle, place: "Minecraft Launcher", state: .waiting,
                                           detail: "Its own JVM arguments come back once the Minecraft Launcher quits."))
                } else {
                    do {
                        try restoreLauncher(file, record.launcher)
                        record.launcher = [:]
                        results.append(GameFix(game: Self.minecraftTitle, place: "Minecraft Launcher", state: .notApplicable,
                                               detail: "Removed: its installations' own JVM arguments are back."))
                    } catch {
                        results.append(GameFix(game: Self.minecraftTitle, place: "Minecraft Launcher", state: .failed,
                                               detail: error.localizedDescription))
                    }
                }
            }
            if !record.modrinth.isEmpty {
                if modrinthRunning {
                    results.append(GameFix(game: Self.minecraftTitle, place: "Modrinth App", state: .waiting,
                                           detail: "Its instances' own JVM arguments come back once Modrinth App quits."))
                } else {
                    do {
                        try restoreModrinth(record.modrinth)
                        record.modrinth = [:]
                        results.append(GameFix(game: Self.minecraftTitle, place: "Modrinth App", state: .notApplicable,
                                               detail: "Removed: its instances' own JVM arguments are back."))
                    } catch {
                        results.append(GameFix(game: Self.minecraftTitle, place: "Modrinth App", state: .failed,
                                               detail: error.localizedDescription))
                    }
                }
            }
            return results
        }

        let instances = prismInstances()
        let profiles = launcherProfiles()
        let modrinth = modrinthInstances()
        guard !instances.isEmpty || !profiles.isEmpty || !modrinth.isEmpty else { return [] }
        guard !minecraftLibraries.path.contains(" ") else {
            return [GameFix(game: Self.minecraftTitle, place: "Java", state: .failed,
                            detail: "\(minecraftLibraries.path) has a space, which JVM arguments cannot hold.")]
        }

        // The libraries: the core, the glue, and LWJGL's own SDL3 and GLFW.
        var available = Set<String>()
        var problems: [String] = []
        do {
            try FileManager.default.createDirectory(at: minecraftLibraries, withIntermediateDirectories: true)
            try copyIfChanged(core, to: minecraftLibraries.appendingPathComponent("libwinemetalgl.dylib"))
            for name in ["libwmglhost.dylib", "libSDL3.dylib", "libglfw.dylib"] {
                try copyIfChanged(carried.appendingPathComponent("glue/minecraft").appendingPathComponent(name),
                                  to: minecraftLibraries.appendingPathComponent(name))
            }
            for glue in [Glue.sdl3, .glfw] {
                guard let jar = nativesJar(glue) else { continue }
                if try extract(glue.jarEntry, from: jar, to: minecraftLibraries.appendingPathComponent(glue.real)) {
                    available.insert(glue.module)
                }
            }
        } catch {
            problems.append(error.localizedDescription)
        }
        if !problems.isEmpty {
            return [GameFix(game: Self.minecraftTitle, place: minecraftLibraries.path, state: .failed,
                            detail: problems.joined(separator: " "))]
        }

        for instance in instances {
            let name = "Prism · " + instance.name
            guard let glue = instance.glue else {
                results.append(GameFix(game: Self.minecraftTitle, place: name, state: .notApplicable,
                                       detail: "Its LWJGL is not one the glue is for (LWJGL 3.3 or later)."))
                continue
            }
            guard available.contains(glue.module) else {
                results.append(GameFix(game: Self.minecraftTitle, place: name, state: .waiting,
                                       detail: "LWJGL's \(glue == .sdl3 ? "SDL3" : "GLFW") is not downloaded yet: launch the instance once."))
                continue
            }
            if prismHasArguments(instance.cfg, glue) {
                results.append(GameFix(game: Self.minecraftTitle, place: name, state: .inPlace, detail: "In place."))
                continue
            }
            if prismRunning {
                results.append(GameFix(game: Self.minecraftTitle, place: name, state: .waiting,
                                       detail: "The JVM arguments go in once Prism Launcher quits."))
                continue
            }
            do {
                try backUp(instance.cfg)
                let saved = try setPrism(instance.cfg, glue)
                if record.prism[instance.cfg.path] == nil { record.prism[instance.cfg.path] = saved }
                results.append(GameFix(game: Self.minecraftTitle, place: name, state: .applied,
                                       detail: "Added the JVM arguments for the \(glue == .sdl3 ? "SDL3" : "GLFW") glue."))
            } catch {
                results.append(GameFix(game: Self.minecraftTitle, place: name, state: .failed, detail: error.localizedDescription))
            }
        }

        if !profiles.isEmpty {
            let file = minecraftHome.appendingPathComponent("launcher_profiles.json")
            var pending: [(Profile, Glue)] = []
            for profile in profiles {
                let name = "Launcher · " + profile.name
                guard let glue = profile.glue else {
                    results.append(GameFix(game: Self.minecraftTitle, place: name, state: .notApplicable,
                                           detail: "Its version is not downloaded, or its LWJGL is not one the glue is for."))
                    continue
                }
                guard available.contains(glue.module) else {
                    results.append(GameFix(game: Self.minecraftTitle, place: name, state: .waiting,
                                           detail: "LWJGL's \(glue == .sdl3 ? "SDL3" : "GLFW") is not downloaded yet: play it once."))
                    continue
                }
                let words = (profile.javaArgs ?? "").split(separator: " ")
                if Set(arguments(glue)).isSubset(of: Set(words.map(String.init))) {
                    results.append(GameFix(game: Self.minecraftTitle, place: name, state: .inPlace, detail: "In place."))
                } else if launcherRunning {
                    results.append(GameFix(game: Self.minecraftTitle, place: name, state: .waiting,
                                           detail: "The JVM arguments go in once the Minecraft Launcher quits."))
                } else {
                    pending.append((profile, glue))
                }
            }
            if !pending.isEmpty {
                do {
                    try backUp(file)
                    let saved = try setLauncher(file, pending)
                    for (id, value) in saved where record.launcher[id] == nil { record.launcher[id] = value }
                    for (profile, glue) in pending {
                        results.append(GameFix(game: Self.minecraftTitle, place: "Launcher · " + profile.name, state: .applied,
                                               detail: "Added the JVM arguments for the \(glue == .sdl3 ? "SDL3" : "GLFW") glue."))
                    }
                } catch {
                    results.append(GameFix(game: Self.minecraftTitle, place: "Minecraft Launcher", state: .failed,
                                           detail: error.localizedDescription))
                }
            }
        }

        var pending: [(ModrinthInstance, Glue)] = []
        for instance in modrinth {
            let name = "Modrinth · " + instance.name
            guard let glue = instance.glue else {
                results.append(GameFix(game: Self.minecraftTitle, place: name, state: .notApplicable,
                                       detail: "Its version is not downloaded, or its LWJGL is not one the glue is for."))
                continue
            }
            guard available.contains(glue.module) else {
                results.append(GameFix(game: Self.minecraftTitle, place: name, state: .waiting,
                                       detail: "LWJGL's \(glue == .sdl3 ? "SDL3" : "GLFW") is not downloaded yet: play it once."))
                continue
            }
            if Set(arguments(glue)).isSubset(of: Set(instance.arguments ?? [])) {
                results.append(GameFix(game: Self.minecraftTitle, place: name, state: .inPlace, detail: "In place."))
            } else if modrinthRunning {
                results.append(GameFix(game: Self.minecraftTitle, place: name, state: .waiting,
                                       detail: "The JVM arguments go in once Modrinth App quits."))
            } else {
                pending.append((instance, glue))
            }
        }
        if !pending.isEmpty {
            do {
                try backUpModrinth()
                let saved = try setModrinth(pending)
                for (id, value) in saved where record.modrinth[id] == nil { record.modrinth[id] = value }
                for (instance, glue) in pending {
                    results.append(GameFix(game: Self.minecraftTitle, place: "Modrinth · " + instance.name, state: .applied,
                                           detail: "Added the JVM arguments for the \(glue == .sdl3 ? "SDL3" : "GLFW") glue."))
                }
            } catch {
                results.append(GameFix(game: Self.minecraftTitle, place: "Modrinth App", state: .failed,
                                       detail: error.localizedDescription))
            }
        }
        return results
    }

    /// The newest natives jar of an LWJGL module, from any of the launchers.
    private func nativesJar(_ glue: Glue) -> URL? {
        let fm = FileManager.default
        var jars: [URL] = []
        for libraries in [minecraftHome, prismHome, modrinthHome.appendingPathComponent("meta")]
            .map({ $0.appendingPathComponent("libraries/org/lwjgl") }) {
            guard let walker = fm.enumerator(at: libraries, includingPropertiesForKeys: nil) else { continue }
            for case let url as URL in walker {
                let name = url.lastPathComponent
                if name.hasPrefix(glue.module + "-"), name.hasSuffix(".jar"), name.contains("natives-macos-arm64") { jars.append(url) }
            }
        }
        return jars.max { $0.path.compare($1.path, options: .numeric) == .orderedAscending }
    }

    /// The entry of a jar (unzip, part of macOS), signed ad hoc; false if it
    /// has none. Unpacked apart: its name is the glue's (libSDL3.dylib).
    private func extract(_ entry: String, from jar: URL, to destination: URL) throws -> Bool {
        let fm = FileManager.default
        let scratch = fm.temporaryDirectory.appendingPathComponent("winemetalgl-natives-\(UUID().uuidString)")
        try fm.createDirectory(at: scratch, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: scratch) }
        let (status, _) = Installations.executeNow(URL(fileURLWithPath: "/usr/bin/unzip"), ["-o", "-j", jar.path, entry, "-d", scratch.path])
        let unpacked = scratch.appendingPathComponent((entry as NSString).lastPathComponent)
        guard status == 0, fm.fileExists(atPath: unpacked.path) else { return false }
        _ = Installations.executeNow(URL(fileURLWithPath: "/usr/bin/codesign"), ["-f", "-s", "-", unpacked.path])
        try copyIfChanged(unpacked, to: destination)
        return true
    }

    // Prism Launcher

    struct Instance {
        let name: String
        let cfg: URL
        let glue: Glue?
    }

    private func prismInstances() -> [Instance] {
        let fm = FileManager.default
        let instances = prismHome.appendingPathComponent("instances")
        var found: [Instance] = []
        for dir in (try? fm.contentsOfDirectory(at: instances, includingPropertiesForKeys: [.isDirectoryKey])) ?? [] {
            let cfg = dir.appendingPathComponent("instance.cfg")
            guard fm.fileExists(atPath: cfg.path) else { continue }
            let name = Ini(cfg).value("name") ?? dir.lastPathComponent
            var glue: Glue?
            if let data = try? Data(contentsOf: dir.appendingPathComponent("mmc-pack.json")),
               let pack = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
               let components = pack["components"] as? [[String: Any]],
               let lwjgl = components.first(where: { $0["uid"] as? String == "org.lwjgl3" }),
               let version = lwjgl["version"] as? String {
                glue = prismGlue(version)
            }
            found.append(Instance(name: name, cfg: cfg, glue: glue))
        }
        return found.sorted { $0.name < $1.name }
    }

    /// The glue for an LWJGL 3 version: by its libraries in Prism's metadata,
    /// else by its version (LWJGL 3.4 brought SDL3, and Minecraft took it).
    private func prismGlue(_ version: String) -> Glue? {
        let meta = prismHome.appendingPathComponent("meta/org.lwjgl3/\(version).json")
        if let data = try? Data(contentsOf: meta),
           let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
           let libraries = json["libraries"] as? [[String: Any]] {
            let names = libraries.compactMap { $0["name"] as? String }
            if names.contains(where: { $0.hasPrefix("org.lwjgl:lwjgl-sdl:") }) { return .sdl3 }
            if names.contains(where: { $0.hasPrefix("org.lwjgl:lwjgl-glfw:") }) { return .glfw }
        }
        let parts = version.split(separator: ".").compactMap { Int($0) }
        guard parts.count >= 2, parts[0] == 3 else { return nil }
        return parts[1] >= 4 ? .sdl3 : parts[1] >= 3 ? .glfw : nil
    }

    private func prismHasArguments(_ cfg: URL, _ glue: Glue) -> Bool {
        let ini = Ini(cfg)
        guard ini.value("OverrideJavaArgs") == "true" else { return false }
        let words = Set((ini.value("JvmArgs") ?? "").split(separator: " ").map(String.init))
        return Set(arguments(glue)).isSubset(of: words)
    }

    /// Sets the instance's JVM arguments: its own, or Prism's global ones
    /// it used, and the glue's. Returns what it had (for restorePrism).
    private func setPrism(_ cfg: URL, _ glue: Glue) throws -> [String: String] {
        var ini = Ini(cfg)
        var saved: [String: String] = [:]
        if let value = ini.value("OverrideJavaArgs") { saved["OverrideJavaArgs"] = value }
        if let value = ini.value("JvmArgs") { saved["JvmArgs"] = value }
        let base = ini.value("OverrideJavaArgs") == "true" ? ini.value("JvmArgs") ?? ""
                                                            : Ini(prismHome.appendingPathComponent("prismlauncher.cfg")).value("JvmArgs") ?? ""
        let kept = base.split(separator: " ").filter { !isOurs($0) }.map(String.init)
        ini.set("OverrideJavaArgs", "true")
        ini.set("JvmArgs", (kept + arguments(glue)).joined(separator: " "))
        try ini.write()
        return saved
    }

    private func restorePrism(_ cfg: URL, _ saved: [String: String]) throws {
        guard FileManager.default.fileExists(atPath: cfg.path) else { return }
        var ini = Ini(cfg)
        let current = (ini.value("JvmArgs") ?? "").split(separator: " ")
        let withoutOurs = current.filter { !isOurs($0) }.map(String.init).joined(separator: " ")
        // Back as it was, unless the player has changed the arguments since:
        // then only the fix's own go.
        let global = Ini(prismHome.appendingPathComponent("prismlauncher.cfg")).value("JvmArgs") ?? ""
        let before = saved["OverrideJavaArgs"] == "true"
            ? (saved["JvmArgs"] ?? "").split(separator: " ").filter { !isOurs($0) }.joined(separator: " ")
            : global
        if withoutOurs == before {
            ini.set("OverrideJavaArgs", saved["OverrideJavaArgs"])
            ini.set("JvmArgs", saved["JvmArgs"])
        } else {
            ini.set("JvmArgs", withoutOurs)
        }
        try ini.write()
    }

    // The official launcher

    struct Profile {
        let id: String
        let name: String
        let javaArgs: String?
        let glue: Glue?
    }

    /// The launcher's own JVM arguments, which a profile without javaArgs uses.
    static let launcherDefaultArguments = "-Xmx2G -XX:+UnlockExperimentalVMOptions -XX:+UseG1GC -XX:G1NewSizePercent=20 "
        + "-XX:G1ReservePercent=20 -XX:MaxGCPauseMillis=50 -XX:G1HeapRegionSize=32M"

    private func launcherProfiles() -> [Profile] {
        let file = minecraftHome.appendingPathComponent("launcher_profiles.json")
        guard let data = try? Data(contentsOf: file),
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let profiles = json["profiles"] as? [String: [String: Any]] else { return [] }
        var latest: [String: String] = [:]
        if let data = try? Data(contentsOf: minecraftHome.appendingPathComponent("versions/version_manifest_v2.json")),
           let manifest = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
            latest = manifest["latest"] as? [String: String] ?? [:]
        }
        return profiles.map { id, profile in
            var version = profile["lastVersionId"] as? String ?? ""
            if version == "latest-release" { version = latest["release"] ?? "" }
            if version == "latest-snapshot" { version = latest["snapshot"] ?? "" }
            let type = profile["type"] as? String
            let name = (profile["name"] as? String).flatMap { $0.isEmpty ? nil : $0 }
                ?? (type == "latest-release" ? "Latest release" : type == "latest-snapshot" ? "Latest snapshot" : version)
            return Profile(id: id, name: name, javaArgs: profile["javaArgs"] as? String, glue: versionGlue(version))
        }.sorted { $0.name < $1.name }
    }

    /// The glue for a downloaded version, by its LWJGL libraries (and those
    /// of the version it inherits from: Fabric's).
    private func versionGlue(_ version: String, depth: Int = 0) -> Glue? {
        guard !version.isEmpty, depth < 4,
              let data = try? Data(contentsOf: minecraftHome.appendingPathComponent("versions/\(version)/\(version).json")),
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
        let names = (json["libraries"] as? [[String: Any]] ?? []).compactMap { $0["name"] as? String }
        if names.contains(where: { $0.hasPrefix("org.lwjgl:lwjgl-sdl:") }) { return .sdl3 }
        if names.contains(where: { $0.hasPrefix("org.lwjgl:lwjgl-glfw:3") }) { return .glfw }
        if let parent = json["inheritsFrom"] as? String { return versionGlue(parent, depth: depth + 1) }
        return nil
    }

    /// Sets the profiles' JVM arguments: theirs (or the launcher's default)
    /// and the glue's. Returns the javaArgs they had ("" for none).
    private func setLauncher(_ file: URL, _ pending: [(Profile, Glue)]) throws -> [String: String] {
        let data = try Data(contentsOf: file)
        guard var json = try JSONSerialization.jsonObject(with: data) as? [String: Any],
              var profiles = json["profiles"] as? [String: [String: Any]] else {
            throw CocoaError(.fileReadCorruptFile, userInfo: [NSFilePathErrorKey: file.path])
        }
        var saved: [String: String] = [:]
        for (profile, glue) in pending {
            guard var entry = profiles[profile.id] else { continue }
            let before = entry["javaArgs"] as? String
            saved[profile.id] = before ?? ""
            let kept = (before ?? Self.launcherDefaultArguments).split(separator: " ").filter { !isOurs($0) }.map(String.init)
            entry["javaArgs"] = (kept + arguments(glue)).joined(separator: " ")
            profiles[profile.id] = entry
        }
        json["profiles"] = profiles
        try JSONSerialization.data(withJSONObject: json, options: [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes])
            .write(to: file, options: .atomic)
        return saved
    }

    private func restoreLauncher(_ file: URL, _ saved: [String: String]) throws {
        guard let data = try? Data(contentsOf: file),
              var json = try JSONSerialization.jsonObject(with: data) as? [String: Any],
              var profiles = json["profiles"] as? [String: [String: Any]] else { return }
        for (id, before) in saved {
            guard var entry = profiles[id] else { continue }
            let withoutOurs = (entry["javaArgs"] as? String ?? "").split(separator: " ").filter { !isOurs($0) }.joined(separator: " ")
            if before.isEmpty && withoutOurs == Self.launcherDefaultArguments {
                entry["javaArgs"] = nil
            } else {
                entry["javaArgs"] = withoutOurs
            }
            profiles[id] = entry
        }
        json["profiles"] = profiles
        try JSONSerialization.data(withJSONObject: json, options: [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes])
            .write(to: file, options: .atomic)
    }

    // Modrinth App

    var modrinthHome: URL { home.appendingPathComponent("Library/Application Support/ModrinthApp", isDirectory: true) }
    var modrinthDatabase: URL { modrinthHome.appendingPathComponent("app.db") }

    struct ModrinthInstance {
        let id: String
        let name: String
        let glue: Glue?
        let overrides: [String: Any]  // its launch overrides (instance_launch_overrides), as JSON
        var arguments: [String]? { overrides["extra_launch_args"] as? [String] }
    }

    /// Modrinth App's instances (its app.db), each with the glue for its
    /// game version's LWJGL, from Modrinth's version metadata.
    private func modrinthInstances() -> [ModrinthInstance] {
        guard let db = SQLiteFile(modrinthDatabase, readOnly: true),
              let rows = db.rows("""
                  SELECT i.id, i.name, c.game_version, c.loader, c.loader_version, o.overrides
                  FROM instances i
                  LEFT JOIN instance_content_sets c ON c.id = i.applied_content_set_id
                  LEFT JOIN instance_launch_overrides o ON o.instance_id = i.id
                  """) else { return [] }
        return rows.compactMap { row in
            guard case let .text(id) = row[0], case let .text(name) = row[1] else { return nil }
            var glue: Glue?
            if case let .text(game) = row[2] {
                let loader = row[3].string ?? "vanilla", loaderVersion = row[4].string
                glue = modrinthGlue(loader == "vanilla" || loaderVersion == nil ? game : "\(game)-\(loaderVersion!)", game: game)
            }
            return ModrinthInstance(id: id, name: name, glue: glue, overrides: row[5].json as? [String: Any] ?? [:])
        }.sorted { $0.name < $1.name }
    }

    /// The glue for a version Modrinth has downloaded, by its LWJGL libraries
    /// (meta/versions/<id>/<id>.json, a loader's merged with the game's).
    private func modrinthGlue(_ id: String, game: String) -> Glue? {
        let versions = modrinthHome.appendingPathComponent("meta/versions")
        let ids = [id] + ((try? FileManager.default.contentsOfDirectory(atPath: versions.path)) ?? []).filter { $0.hasPrefix(game) && $0 != id }
        for id in ids {
            guard let data = try? Data(contentsOf: versions.appendingPathComponent("\(id)/\(id).json")),
                  let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { continue }
            let names = (json["libraries"] as? [[String: Any]] ?? []).compactMap { $0["name"] as? String }
            if names.contains(where: { $0.hasPrefix("org.lwjgl:lwjgl-sdl:") }) { return .sdl3 }
            if names.contains(where: { $0.hasPrefix("org.lwjgl:lwjgl-glfw:3") }) { return .glfw }
        }
        return nil
    }

    /// Modrinth's own JVM arguments (settings), which an instance without its own uses.
    private func modrinthGlobalArguments() -> [String] {
        guard let db = SQLiteFile(modrinthDatabase, readOnly: true),
              let rows = db.rows("SELECT extra_launch_args FROM settings"), let row = rows.first else { return [] }
        return row[0].json as? [String] ?? []
    }

    /// Sets the instances' JVM arguments: theirs (or Modrinth's global ones)
    /// and the glue's. Returns the arguments they had (JSON; "" for none).
    /// Written as JSON text: Modrinth reads the column through json(),
    /// which takes text as well as JSONB.
    private func setModrinth(_ pending: [(ModrinthInstance, Glue)]) throws -> [String: String] {
        guard let db = SQLiteFile(modrinthDatabase, readOnly: false) else {
            throw CocoaError(.fileWriteUnknown, userInfo: [NSFilePathErrorKey: modrinthDatabase.path])
        }
        let global = modrinthGlobalArguments()
        var saved: [String: String] = [:]
        for (instance, glue) in pending {
            saved[instance.id] = instance.arguments.flatMap { Self.jsonText($0) } ?? ""
            var overrides = instance.overrides
            let kept = (instance.arguments ?? global).filter { !isOurs(Substring($0)) }
            overrides["extra_launch_args"] = kept + arguments(glue)
            try writeModrinth(db, instance.id, overrides)
        }
        return saved
    }

    private func restoreModrinth(_ saved: [String: String]) throws {
        guard let db = SQLiteFile(modrinthDatabase, readOnly: false) else { return }
        let global = modrinthGlobalArguments()
        for instance in modrinthInstances() {
            guard let before = saved[instance.id] else { continue }
            var overrides = instance.overrides
            let withoutOurs = (instance.arguments ?? []).filter { !isOurs(Substring($0)) }
            // Back to none (Modrinth's own) when that is all that is left.
            if before.isEmpty && withoutOurs == global {
                overrides["extra_launch_args"] = nil
            } else {
                overrides["extra_launch_args"] = withoutOurs
            }
            try writeModrinth(db, instance.id, overrides)
        }
    }

    private func writeModrinth(_ db: SQLiteFile, _ id: String, _ overrides: [String: Any]) throws {
        guard let text = Self.jsonText(overrides),
              db.run("""
                  INSERT INTO instance_launch_overrides (instance_id, overrides) VALUES (?1, ?2)
                  ON CONFLICT (instance_id) DO UPDATE SET overrides = excluded.overrides
                  """, [id, text]) else {
            throw CocoaError(.fileWriteUnknown, userInfo: [NSFilePathErrorKey: modrinthDatabase.path,
                                                          NSLocalizedDescriptionKey: "Could not write Modrinth App's settings."])
        }
    }

    /// Modrinth's database copied to Backups, as backUp does a file, with
    /// what its write-ahead log holds (VACUUM INTO).
    private func backUpModrinth() throws {
        guard let db = SQLiteFile(modrinthDatabase, readOnly: true) else { return }
        let copy = backups.appendingPathComponent(String(modrinthDatabase.resolvingSymlinksInPath().path.dropFirst()))
        guard !FileManager.default.fileExists(atPath: copy.path) else { return }
        try FileManager.default.createDirectory(at: copy.deletingLastPathComponent(), withIntermediateDirectories: true)
        guard db.run("VACUUM INTO ?1", [copy.path]) else {
            throw CocoaError(.fileWriteUnknown, userInfo: [NSFilePathErrorKey: copy.path])
        }
    }

    static func jsonText(_ value: Any) -> String? {
        (try? JSONSerialization.data(withJSONObject: value, options: [.sortedKeys, .withoutEscapingSlashes]))
            .map { String(decoding: $0, as: UTF8.self) }
    }

    // MARK: OpenMW

    static let openGL = "/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL"
    static let stubs = "@executable_path/../Frameworks/libwmglhost.dylib"

    private func openmwApps() -> [URL] {
        let fm = FileManager.default
        var apps: [URL] = []
        // A test home (WINEMETALGL_SETTINGS_HOME) has its own Applications only.
        let roots = AppEnvironment.testHome == nil ? [URL(fileURLWithPath: "/Applications"), home.appendingPathComponent("Applications")]
                                                   : [home.appendingPathComponent("Applications")]
        for dir in roots {
            for entry in (try? fm.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)) ?? []
            where entry.pathExtension == "app" && fm.fileExists(atPath: entry.appendingPathComponent("Contents/MacOS/openmw").path) {
                apps.append(entry)
            }
        }
        return apps
    }

    /// openmw and OpenSceneGraph's libraries: what loads OpenGL.framework (or the stubs).
    private func openmwBinaries(_ app: URL) -> [URL] {
        let frameworks = app.appendingPathComponent("Contents/Frameworks")
        // The libraries themselves, not their versioned symlinks.
        let osg = ((try? FileManager.default.contentsOfDirectory(atPath: frameworks.path)) ?? [])
            .filter { $0.hasPrefix("libosg") && $0.hasSuffix(".dylib") }
            .map { frameworks.appendingPathComponent($0) }
            .filter { (try? $0.resourceValues(forKeys: [.isSymbolicLinkKey]).isSymbolicLink) != true }
        return [app.appendingPathComponent("Contents/MacOS/openmw")] + osg
    }

    private func openmwFix(_ on: Bool) -> [GameFix] {
        var results: [GameFix] = []
        for app in openmwApps() {
            let name = app.deletingPathExtension().lastPathComponent
            func result(_ state: GameFix.State, _ detail: String) {
                results.append(GameFix(game: Self.openmwTitle, place: name, state: state, detail: detail))
            }
            let frameworks = app.appendingPathComponent("Contents/Frameworks")
            let sdl = frameworks.appendingPathComponent("libSDL2-2.0.0.dylib")
            let real = frameworks.appendingPathComponent("libSDL2-real.dylib")
            let added = ["libwmglhost.dylib", "libwinemetalgl.dylib"].map { frameworks.appendingPathComponent($0) }
            let glue = carried.appendingPathComponent("glue/openmw")
            let fm = FileManager.default
            let patched = fm.fileExists(atPath: real.path)
            if Self.running(app.appendingPathComponent("Contents/MacOS/openmw").path) {
                if on != patched { result(.waiting, "The fix \(on ? "goes in" : "comes out") once OpenMW quits.") }
                continue
            }
            do {
                if on {
                    guard fm.fileExists(atPath: sdl.path) else {
                        result(.notApplicable, "It has no libSDL2-2.0.0.dylib: not a build the glue is for.")
                        continue
                    }
                    let current = [glue.appendingPathComponent("libSDL2-2.0.0.dylib"), glue.appendingPathComponent("libwmglhost.dylib"), core]
                        .map { GamePatcher.sha256($0) }
                    let installed = [sdl, added[0], added[1]].map { GamePatcher.sha256($0) }
                    if patched && current == installed && openmwBinaries(app).allSatisfy({ (try? MachO.loads($0, Self.stubs)) == true }) {
                        result(.inPlace, "In place.")
                        continue
                    }
                    for binary in openmwBinaries(app) { try backUp(binary) }
                    try backUp(sdl)
                    if !patched { try fm.copyItem(at: sdl, to: real) }
                    try GamePatcher.replace(sdl, with: glue.appendingPathComponent("libSDL2-2.0.0.dylib"))
                    try copyIfChanged(glue.appendingPathComponent("libwmglhost.dylib"), to: added[0])
                    try copyIfChanged(core, to: added[1])
                    for binary in openmwBinaries(app) { try MachO.replaceDylib(binary, Self.openGL, with: Self.stubs) }
                    try signAdHoc(app)
                    result(.applied, "Installed the glue and the arm64 core into \(app.lastPathComponent).")
                } else {
                    guard patched else { continue }
                    try GamePatcher.replace(sdl, with: real)
                    try fm.removeItem(at: real)
                    for file in added { try? fm.removeItem(at: file) }
                    for binary in openmwBinaries(app) { try MachO.replaceDylib(binary, Self.stubs, with: Self.openGL) }
                    try signAdHoc(app)
                    result(.notApplicable, "Removed: \(app.lastPathComponent) uses Apple's OpenGL again.")
                }
            } catch {
                result(.failed, error.localizedDescription)
            }
        }
        return results
    }

    private func signAdHoc(_ app: URL) throws {
        let (status, output) = Installations.executeNow(URL(fileURLWithPath: "/usr/bin/codesign"),
                                                        ["--force", "--deep", "--sign", "-", app.path])
        guard status == 0 else {
            throw NSError(domain: "WineMetalGL", code: Int(status),
                          userInfo: [NSLocalizedDescriptionKey: "codesign: " + output.trimmingCharacters(in: .whitespacesAndNewlines)])
        }
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

    private func copyIfChanged(_ source: URL, to destination: URL) throws {
        let fm = FileManager.default
        if fm.fileExists(atPath: destination.path) {
            if GamePatcher.sha256(source) == GamePatcher.sha256(destination) { return }
            try GamePatcher.replace(destination, with: source)
        } else {
            try fm.copyItem(at: source, to: destination)
        }
    }

    /// A program runs whose path contains this.
    static func running(_ path: String) -> Bool {
        Installations.executeNow(URL(fileURLWithPath: "/usr/bin/pgrep"), ["-f", path]).0 == 0
    }
}

/// What the Minecraft fix changed in the launchers, to restore it:
/// NativeFixes.json, {"prism": {cfg path: {key: value before}},
/// "launcher": {profile id: javaArgs before, "" for none},
/// "modrinth": {instance id: its JVM arguments before (JSON), "" for none}}.
struct Records {
    let file: URL
    var prism: [String: [String: String]] = [:]
    var launcher: [String: String] = [:]
    var modrinth: [String: String] = [:]

    init(file: URL) {
        self.file = file
        if let data = try? Data(contentsOf: file),
           let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
            prism = json["prism"] as? [String: [String: String]] ?? [:]
            launcher = json["launcher"] as? [String: String] ?? [:]
            modrinth = json["modrinth"] as? [String: String] ?? [:]
        }
    }

    func save() {
        try? FileManager.default.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
        if let data = try? JSONSerialization.data(withJSONObject: ["prism": prism, "launcher": launcher, "modrinth": modrinth],
                                                  options: [.prettyPrinted, .sortedKeys]) {
            try? data.write(to: file, options: .atomic)
        }
    }
}

/// Prism Launcher's instance.cfg and prismlauncher.cfg: an INI file whose
/// [General] keys are the settings (values may be quoted).
struct Ini {
    let file: URL
    private var lines: [String]

    init(_ file: URL) {
        self.file = file
        lines = ((try? String(contentsOf: file, encoding: .utf8)) ?? "").components(separatedBy: "\n")
    }

    /// The [General] section's lines: from its header to the next section.
    private var general: Range<Int> {
        guard let start = lines.firstIndex(where: { $0.trimmingCharacters(in: .whitespaces) == "[General]" }) else {
            return lines.count..<lines.count
        }
        let end = lines[(start + 1)...].firstIndex { $0.hasPrefix("[") } ?? lines.count
        return (start + 1)..<end
    }

    func value(_ key: String) -> String? {
        for i in general where lines[i].hasPrefix(key + "=") {
            var value = String(lines[i].dropFirst(key.count + 1))
            if value.count >= 2, value.hasPrefix("\""), value.hasSuffix("\"") {
                value = String(value.dropFirst().dropLast()).replacingOccurrences(of: "\\\"", with: "\"")
            }
            return value
        }
        return nil
    }

    /// Sets (or, with nil, removes) a [General] key.
    mutating func set(_ key: String, _ value: String?) {
        let line = value.map { $0.contains(" ") || $0.contains("\"") ? "\(key)=\"\($0.replacingOccurrences(of: "\"", with: "\\\""))\"" : "\(key)=\($0)" }
        if let i = general.first(where: { lines[$0].hasPrefix(key + "=") }) {
            if let line { lines[i] = line } else { lines.remove(at: i) }
        } else if let line {
            if general.isEmpty && !lines.contains(where: { $0.trimmingCharacters(in: .whitespaces) == "[General]" }) {
                lines.insert(contentsOf: ["[General]", line], at: 0)
            } else {
                // After the section's last non-empty line.
                var at = general.upperBound
                while at > general.lowerBound && lines[at - 1].trimmingCharacters(in: .whitespaces).isEmpty { at -= 1 }
                lines.insert(line, at: at)
            }
        }
    }

    func write() throws {
        try lines.joined(separator: "\n").write(to: file, atomically: true, encoding: .utf8)
    }
}

/// Mach-O load commands, edited in place: a dylib a binary loads, renamed
/// within the room its command has (thin 64-bit files, and fat ones' slices).
enum MachO {
    private static let magic64: UInt32 = 0xFEEDFACF
    private static let fatMagic: UInt32 = 0xCAFEBABE
    private static let dylibCommands: Set<UInt32> = [0xC, 0x8000_0018, 0x8000_001F, 0x8000_0023]  // LOAD, WEAK, REEXPORT, LAZY

    /// Whether the binary loads `path`.
    static func loads(_ file: URL, _ path: String) throws -> Bool {
        let data = try Data(contentsOf: file)
        var found = false
        try eachDylib(data) { _, name, _ in if name == path { found = true } }
        return found
    }

    /// Renames the dylib `old` to `new` where the binary loads it; nothing if
    /// it does not (already renamed).
    static func replaceDylib(_ file: URL, _ old: String, with new: String) throws {
        var data = try Data(contentsOf: file)
        var edits: [(Int, Int)] = []  // name offset, room
        try eachDylib(data) { offset, name, room in if name == old { edits.append((offset, room)) } }
        guard !edits.isEmpty else { return }
        let bytes = Array(new.utf8)
        for (offset, room) in edits {
            guard bytes.count < room else {
                throw NSError(domain: "WineMetalGL", code: 1,
                              userInfo: [NSLocalizedDescriptionKey: "\(file.lastPathComponent): no room for \(new)"])
            }
            data.replaceSubrange(offset..<(offset + room), with: bytes + [UInt8](repeating: 0, count: room - bytes.count))
        }
        try data.write(to: file, options: .atomic)
    }

    private static func eachDylib(_ data: Data, _ body: (Int, String, Int) -> Void) throws {
        func u32(_ at: Int, big: Bool = false) -> UInt32 {
            guard at + 4 <= data.count else { return 0 }
            let v = data.subdata(in: at..<(at + 4)).withUnsafeBytes { $0.loadUnaligned(as: UInt32.self) }
            return big ? UInt32(bigEndian: v) : UInt32(littleEndian: v)
        }
        var slices: [Int] = []
        if u32(0, big: true) == fatMagic {
            for i in 0..<Int(u32(4, big: true)) { slices.append(Int(u32(8 + i * 20 + 8, big: true))) }
        } else {
            slices.append(0)
        }
        for base in slices {
            guard u32(base) == magic64 else { continue }
            let count = Int(u32(base + 16))
            var at = base + 32
            for _ in 0..<count {
                let cmd = u32(at), size = Int(u32(at + 4))
                guard size >= 8, at + size <= data.count else { break }
                if dylibCommands.contains(cmd) {
                    let nameOffset = Int(u32(at + 8))
                    let start = at + nameOffset, room = size - nameOffset
                    if room > 0, start + room <= data.count {
                        let raw = data.subdata(in: start..<(start + room))
                        let name = String(decoding: raw.prefix { $0 != 0 }, as: UTF8.self)
                        body(start, name, room)
                    }
                }
                at += size
            }
        }
    }
}

/// A SQLite database (Modrinth App's app.db), through the system's SQLite.
final class SQLiteFile {
    enum Value {
        case null, integer(Int64), real(Double), text(String), blob([UInt8])

        var string: String? { if case let .text(text) = self { return text } else { return nil } }
        /// A JSON column: text, or SQLite's binary JSON (JSONB), read here
        /// so that a SQLite older than JSONB (3.45) can read it too.
        var json: Any? {
            switch self {
            case let .text(text): return try? JSONSerialization.jsonObject(with: Data(text.utf8), options: [.fragmentsAllowed])
            case let .blob(bytes): var at = 0; return JSONB.value(bytes, &at)
            default: return nil
            }
        }
    }

    private var db: OpaquePointer?

    init?(_ file: URL, readOnly: Bool) {
        guard FileManager.default.fileExists(atPath: file.path),
              sqlite3_open_v2(file.path, &db, readOnly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE, nil) == SQLITE_OK else {
            sqlite3_close(db)
            return nil
        }
        sqlite3_busy_timeout(db, 3000)
    }

    deinit { sqlite3_close(db) }

    private func prepare(_ sql: String, _ binds: [String]) -> OpaquePointer? {
        var statement: OpaquePointer?
        guard sqlite3_prepare_v2(db, sql, -1, &statement, nil) == SQLITE_OK else { return nil }
        let transient = unsafeBitCast(-1, to: sqlite3_destructor_type.self)
        for (i, value) in binds.enumerated() { sqlite3_bind_text(statement, Int32(i + 1), value, -1, transient) }
        return statement
    }

    func rows(_ sql: String, _ binds: [String] = []) -> [[Value]]? {
        guard let statement = prepare(sql, binds) else { return nil }
        defer { sqlite3_finalize(statement) }
        var rows: [[Value]] = []
        while sqlite3_step(statement) == SQLITE_ROW {
            rows.append((0..<sqlite3_column_count(statement)).map { column in
                switch sqlite3_column_type(statement, column) {
                case SQLITE_INTEGER: return .integer(sqlite3_column_int64(statement, column))
                case SQLITE_FLOAT: return .real(sqlite3_column_double(statement, column))
                case SQLITE_TEXT: return .text(String(cString: sqlite3_column_text(statement, column)))
                case SQLITE_BLOB:
                    let count = Int(sqlite3_column_bytes(statement, column))
                    guard count > 0, let bytes = sqlite3_column_blob(statement, column) else { return .blob([]) }
                    return .blob(Array(UnsafeRawBufferPointer(start: bytes, count: count)))
                default: return .null
                }
            })
        }
        return rows
    }

    func run(_ sql: String, _ binds: [String] = []) -> Bool {
        guard let statement = prepare(sql, binds) else { return false }
        defer { sqlite3_finalize(statement) }
        return sqlite3_step(statement) == SQLITE_DONE
    }
}

/// SQLite's JSONB (sqlite.org/jsonb.html): each element a header (its type
/// in the low four bits, its payload's size in the high four, or in the 1,
/// 2, 4 or 8 bytes after) and its payload.
enum JSONB {
    static func value(_ bytes: [UInt8], _ at: inout Int) -> Any? {
        guard at < bytes.count else { return nil }
        let type = bytes[at] & 0x0f
        var size = Int(bytes[at] >> 4)
        at += 1
        if size >= 12 {
            let width = [1, 2, 4, 8][size - 12]
            guard at + width <= bytes.count else { return nil }
            size = bytes[at..<at + width].reduce(0) { $0 << 8 | Int($1) }
            at += width
        }
        guard at + size <= bytes.count else { return nil }
        let payload = Array(bytes[at..<at + size])
        at += size
        let text = String(decoding: payload, as: UTF8.self)
        switch type {
        case 0: return NSNull()
        case 1: return true
        case 2: return false
        case 3, 4: return Int(text) ?? Double(text)
        case 5, 6: return Double(text)
        case 7, 10: return text
        case 8, 9:  // with JSON escapes
            return (try? JSONSerialization.jsonObject(with: Data("\"\(text)\"".utf8), options: [.fragmentsAllowed])) ?? text
        case 11:
            var items: [Any] = [], inner = 0
            while inner < payload.count { guard let item = value(payload, &inner) else { return nil }; items.append(item) }
            return items
        case 12:
            var object: [String: Any] = [:], inner = 0
            while inner < payload.count {
                guard let key = value(payload, &inner) as? String, let item = value(payload, &inner) else { return nil }
                object[key] = item
            }
            return object
        default: return nil
        }
    }
}
