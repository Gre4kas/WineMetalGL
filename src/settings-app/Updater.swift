// Updates: when the app opens it asks GitHub for the latest WineMetalGL
// release, and when that is newer than the app, offers it. On Yes it
// downloads the release's WineMetalGL-Settings-<version>.dmg, checks it
// against the release's SHA256SUMS.txt and its signature (the app's own
// Developer ID team, notarized), installs the app in it in place of this one
// and opens it. The new app then installs its release into every bottle and
// prefix that has WineMetalGL (Installations.updateEverywhere), with its own
// install scripts and module.
//
// To try it without a release: WINEMETALGL_UPDATE_FEED is a URL (file:// is
// fine) of a release in GitHub's JSON (tag_name, assets' name and
// browser_download_url), and WINEMETALGL_UPDATE_TESTING=1 accepts disk images
// and apps signed ad hoc, as local builds are.

import AppKit
import CryptoKit
import Security

@MainActor
final class Updater: ObservableObject {
    @Published private(set) var status: String?
    @Published var report: Installations.Report?

    struct Release: Sendable {
        let version: String
        let dmgName: String
        let dmg: URL
        let sums: URL
    }

    struct Failure: LocalizedError {
        let errorDescription: String?
        init(_ text: String) { errorDescription = text }
    }

    private static let pendingKey = "pendingModuleUpdate"
    private let environment = ProcessInfo.processInfo.environment

    var currentVersion: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "0"
    }

    private var feed: URL {
        environment["WINEMETALGL_UPDATE_FEED"].flatMap(URL.init(string:))
            ?? URL(string: "https://api.github.com/repos/metalsharp/WineMetalGL/releases/latest")!
    }

    private var testing: Bool { environment["WINEMETALGL_UPDATE_TESTING"] == "1" }

    /// Set by the app that installed this one: the version whose release goes
    /// into the Wines next. Read once.
    func takePendingModuleUpdate() -> String? {
        guard let version = AppEnvironment.defaults.string(forKey: Self.pendingKey) else { return nil }
        AppEnvironment.defaults.removeObject(forKey: Self.pendingKey)
        return version == currentVersion ? version : nil
    }

    // MARK: Looking

    /// Offers a newer release; true when it is being installed (the app then quits).
    func offerUpdate() async -> Bool {
        guard let release = try? await latest(), Self.isNewer(release.version, than: currentVersion) else { return false }
        let alert = NSAlert()
        alert.messageText = "WineMetalGL \(release.version) is available"
        alert.informativeText = "You have \(currentVersion). Update WineMetalGL Settings, and WineMetalGL wherever it is installed, now?"
        alert.addButton(withTitle: "Yes")
        alert.addButton(withTitle: "No")
        guard alert.runModal() == .alertFirstButtonReturn else { return false }
        do {
            try await install(release)
            return true
        } catch {
            status = nil
            report = .init(title: "WineMetalGL \(release.version) was not installed", text: error.localizedDescription)
            return false
        }
    }

    func latest() async throws -> Release {
        var request = URLRequest(url: feed, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: 15)
        request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        request.setValue("WineMetalGL-Settings/\(currentVersion)", forHTTPHeaderField: "User-Agent")
        let (data, _) = try await URLSession.shared.data(for: request)
        guard let json = try JSONSerialization.jsonObject(with: data) as? [String: Any],
              let tag = json["tag_name"] as? String, let assets = json["assets"] as? [[String: Any]] else {
            throw Failure("GitHub's answer is not a release.")
        }
        let version = tag.hasPrefix("winemetalgl-") ? String(tag.dropFirst("winemetalgl-".count)) : tag
        func asset(_ name: String) -> URL? {
            assets.first { $0["name"] as? String == name }
                .flatMap { $0["browser_download_url"] as? String }.flatMap(URL.init(string:))
        }
        let dmgName = "WineMetalGL-Settings-\(version).dmg"
        // A release whose disk image is not up yet (the workflow adds it last) is not offered.
        guard let dmg = asset(dmgName), let sums = asset("SHA256SUMS.txt") else {
            throw Failure("WineMetalGL \(version) has no \(dmgName) yet.")
        }
        return Release(version: version, dmgName: dmgName, dmg: dmg, sums: sums)
    }

    /// 3.10.0 is newer than 3.9.2.
    nonisolated static func isNewer(_ version: String, than current: String) -> Bool {
        let a = version.split(separator: ".").map { Int($0) ?? 0 }
        let b = current.split(separator: ".").map { Int($0) ?? 0 }
        for i in 0..<max(a.count, b.count) {
            let x = i < a.count ? a[i] : 0, y = i < b.count ? b[i] : 0
            if x != y { return x > y }
        }
        return false
    }

    // MARK: Installing

    func install(_ release: Release) async throws {
        let work = FileManager.default.temporaryDirectory.appendingPathComponent("WineMetalGL-update-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: work, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: work) }

        status = "Downloading WineMetalGL \(release.version)…"
        let dmg = work.appendingPathComponent(release.dmgName)
        try await download(release.dmg, to: dmg)
        let sums = work.appendingPathComponent("SHA256SUMS.txt")
        try await download(release.sums, to: sums)

        status = "Checking WineMetalGL \(release.version)…"
        let text = try String(contentsOf: sums, encoding: .utf8)
        let expected = text.split(separator: "\n").compactMap { line -> String? in
            let parts = line.split(separator: " ", maxSplits: 1)
            guard parts.count == 2, parts[1].trimmingCharacters(in: CharacterSet(charactersIn: " *")) == release.dmgName else { return nil }
            return parts[0].lowercased()
        }.first
        guard let expected else { throw Failure("The release's SHA256SUMS.txt does not list \(release.dmgName).") }
        guard GamePatcher.sha256(dmg) == expected else {
            throw Failure("\(release.dmgName) does not match the release's SHA256SUMS.txt.")
        }
        let requirement = try signingRequirement()
        try await verify(dmg, requirement: requirement, gatekeeper: ["--type", "open", "--context", "context:primary-signature"])

        let mount = work.appendingPathComponent("mount")
        try FileManager.default.createDirectory(at: mount, withIntermediateDirectories: true)
        try await tool("/usr/bin/hdiutil", ["attach", dmg.path, "-nobrowse", "-readonly", "-noautoopen", "-mountpoint", mount.path])
        defer { _ = Installations.executeNow(URL(fileURLWithPath: "/usr/bin/hdiutil"), ["detach", mount.path, "-force"]) }

        let newApp = mount.appendingPathComponent("WineMetalGL Settings.app")
        guard let info = Bundle(url: newApp)?.infoDictionary,
              info["CFBundleIdentifier"] as? String == Bundle.main.bundleIdentifier,
              info["CFBundleShortVersionString"] as? String == release.version else {
            throw Failure("\(release.dmgName) does not hold WineMetalGL Settings \(release.version).")
        }
        try await verify(newApp, requirement: requirement, gatekeeper: ["--type", "execute"])

        status = "Installing WineMetalGL Settings \(release.version)…"
        let destination = installLocation()
        let staged = destination.deletingLastPathComponent().appendingPathComponent(".WineMetalGL Settings \(release.version).app")
        try? FileManager.default.removeItem(at: staged)
        try await tool("/usr/bin/ditto", [newApp.path, staged.path])
        do {
            if FileManager.default.fileExists(atPath: destination.path) {
                _ = try FileManager.default.replaceItemAt(destination, withItemAt: staged)
            } else {
                try FileManager.default.moveItem(at: staged, to: destination)
            }
        } catch {
            try? FileManager.default.removeItem(at: staged)
            throw Failure("Could not replace \(destination.path): \(error.localizedDescription)")
        }

        AppEnvironment.defaults.set(release.version, forKey: Self.pendingKey)
        AppEnvironment.defaults.synchronize()
        relaunch(destination)
    }

    /// Where this app is, unless that cannot be written (the disk image, or a
    /// copy macOS moved aside to run it: App Translocation): /Applications.
    private func installLocation() -> URL {
        let here = Bundle.main.bundleURL
        let readOnly = (try? here.resourceValues(forKeys: [.volumeIsReadOnlyKey]).volumeIsReadOnly) ?? false
        if readOnly || here.path.contains("/AppTranslocation/")
            || !FileManager.default.isWritableFile(atPath: here.deletingLastPathComponent().path) {
            return URL(fileURLWithPath: "/Applications/WineMetalGL Settings.app")
        }
        return here
    }

    private func download(_ url: URL, to file: URL) async throws {
        var request = URLRequest(url: url, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: 60)
        request.setValue("WineMetalGL-Settings/\(currentVersion)", forHTTPHeaderField: "User-Agent")
        let (temporary, response) = try await URLSession.shared.download(for: request)
        if let http = response as? HTTPURLResponse, http.statusCode != 200 {
            throw Failure("\(url.lastPathComponent): HTTP \(http.statusCode)")
        }
        try? FileManager.default.removeItem(at: file)
        try FileManager.default.moveItem(at: temporary, to: file)
    }

    // MARK: Signatures

    /// The update must be signed by this app's Developer ID team. A build of
    /// the app without one (from source) accepts any Developer ID, or, with
    /// WINEMETALGL_UPDATE_TESTING=1, any signature at all.
    private func signingRequirement() throws -> String? {
        let developerID = "anchor apple generic and certificate 1[field.1.2.840.113635.100.6.2.6] exists"
            + " and certificate leaf[field.1.2.840.113635.100.6.1.13] exists"
        if let team = Self.ownTeam() { return developerID + " and certificate leaf[subject.OU] = \"\(team)\"" }
        return testing ? nil : developerID
    }

    nonisolated static func ownTeam() -> String? {
        var code: SecCode?
        var staticCode: SecStaticCode?
        var info: CFDictionary?
        guard SecCodeCopySelf([], &code) == errSecSuccess, let code,
              SecCodeCopyStaticCode(code, [], &staticCode) == errSecSuccess, let staticCode,
              SecCodeCopySigningInformation(staticCode, SecCSFlags(rawValue: kSecCSSigningInformation), &info) == errSecSuccess,
              let info = info as? [String: Any] else { return nil }
        return info[kSecCodeInfoTeamIdentifier as String] as? String
    }

    /// codesign's check (with the requirement), then Gatekeeper's (signed by a
    /// Developer ID and notarized), which a testing build skips.
    private func verify(_ item: URL, requirement: String?, gatekeeper: [String]) async throws {
        var arguments = ["--verify", "--strict", "--deep"]
        if let requirement { arguments += ["-R=" + requirement] }
        let (status, output) = await Installations.execute(URL(fileURLWithPath: "/usr/bin/codesign"), arguments + [item.path])
        guard status == 0 else { throw Failure("\(item.lastPathComponent) is not signed by WineMetalGL's developer: \(output)") }
        guard requirement != nil || !testing else { return }
        let (assessed, verdict) = await Installations.execute(URL(fileURLWithPath: "/usr/sbin/spctl"), ["--assess"] + gatekeeper + [item.path])
        guard assessed == 0 else { throw Failure("Gatekeeper does not accept \(item.lastPathComponent): \(verdict)") }
    }

    private func tool(_ path: String, _ arguments: [String]) async throws {
        let (status, output) = await Installations.execute(URL(fileURLWithPath: path), arguments)
        guard status == 0 else { throw Failure("\((path as NSString).lastPathComponent) failed: \(output)") }
    }

    /// Opens the new app once this one has quit, with this one's WINEMETALGL_
    /// variables (open passes on no others).
    private func relaunch(_ app: URL) {
        let passed = environment.filter { $0.key.hasPrefix("WINEMETALGL_") }.sorted { $0.key < $1.key }
            .flatMap { ["--env", "\($0.key)=\($0.value)"] }
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/sh")
        process.arguments = ["-c", "pid=$1; shift; while /bin/kill -0 \"$pid\" 2>/dev/null; do /bin/sleep 0.2; done; exec /usr/bin/open \"$@\"",
                             "sh", String(ProcessInfo.processInfo.processIdentifier)] + passed + [app.path]
        try? process.run()
        NSApp.terminate(nil)
    }
}
