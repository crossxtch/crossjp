import AppKit
import os
import SwiftUI
import UniformTypeIdentifiers

enum ReaderDevice: String, CaseIterable, Identifiable {
    case x3
    case x4

    var id: String { rawValue }
    var title: String { self == .x3 ? "X3" : "X4" }
    var page: String { self == .x3 ? "528×792" : "480×800" }
    var ppi: Double { self == .x3 ? XgfEncode.ppiX3 : XgfEncode.ppiX4 }

    func em(pt: Double) -> Int {
        XgfEncode.emSize(ppi: ppi, pt: pt)
    }
}

@MainActor
final class FontModel: ObservableObject {
    @Published var faces: [Face] = []
    @Published var faceID = ""
    @Published var device: ReaderDevice = .x3
    @Published var pt = 13.0
    @Published var preview: [NSImage] = []
    @Published var glyphCount = 0
    @Published var rubyCount = 0
    @Published var byteCount = 0
    @Published var busy = false
    @Published var fraction = 0.0
    @Published var status = "Looking through installed fonts…"
    @Published var failed = false
    @Published var ready = false

    private var ticket = 0
    private var cps: [UInt32] = []
    private var cachedFace = ""
    private var cancelFlag: OSAllocatedUnfairLock<Bool>?
    private var debounce: DispatchWorkItem?

    var em: Int { device.em(pt: pt) }
    var rubyEm: Int { XgfEncode.rubySize(em) }
    var rawEm: Int { Int((pt * device.ppi / 72.0).rounded()) }

    var faceTitle: String {
        faces.first { $0.id == faceID }?.title ?? faceID
    }

    func boot() {
        DispatchQueue.global(qos: .userInitiated).async {
            let faces = FaceCatalog.list()
            DispatchQueue.main.async {
                self.faces = faces
                if let pick = FaceCatalog.preferred.first(where: { id in faces.contains { $0.id == id } }) ?? faces.first?.id {
                    self.faceID = pick
                }
                if faces.isEmpty {
                    self.status = "No Japanese font is installed."
                    self.failed = true
                    return
                }
                self.refreshSoon()
            }
        }
    }

    func refreshSoon() {
        debounce?.cancel()
        let work = DispatchWorkItem { [weak self] in
            self?.refresh()
        }
        debounce = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.12, execute: work)
    }

    func refresh() {
        guard !busy, !faceID.isEmpty else { return }
        ticket += 1
        let ticket = ticket
        let ps = faceID
        let em = em
        let rubyEm = rubyEm
        let recount = ps != cachedFace
        let existing = cps
        status = recount ? "Counting characters…" : ""
        failed = false
        if recount {
            ready = false
        }
        DispatchQueue.global(qos: .userInitiated).async {
            let cps = recount ? FaceCatalog.coverage(postscript: ps, joyo: FaceCatalog.joyoFromBundle()) : existing
            let plan = XgfEncode.plan(cps)
            let sample = Array("春は、あけぼの。漢字".unicodeScalars.map(\.value))
            var images: [NSImage] = []
            if let raster = Rasterizer(postscript: ps, em: em) {
                for cp in sample {
                    if let cg = raster.pageImage(cp: cp) {
                        images.append(NSImage(cgImage: cg, size: NSSize(width: em, height: em)))
                    }
                }
            }
            let bytes = plan.fileBytes(em: em, rubyEm: rubyEm)
            DispatchQueue.main.async {
                guard ticket == self.ticket, !self.busy else { return }
                self.cachedFace = ps
                self.cps = cps
                self.glyphCount = plan.byId.count
                self.rubyCount = plan.rubyIds.count
                self.byteCount = bytes
                self.preview = images
                self.ready = plan.byId.count > 1000
                self.status = ""
                self.failed = !self.ready
                if !self.ready {
                    self.status = "This face does not cover enough Japanese."
                }
            }
        }
    }

    /// Collapse a save panel that appended ".xgf2" onto a name that already had it.
    private static func xgf2URL(_ url: URL) -> URL {
        var name = url.deletingPathExtension().lastPathComponent
        let ext = url.pathExtension.lowercased()
        if ext == "xgf2" {
            while name.lowercased().hasSuffix(".xgf2") {
                name = String(name.dropLast(5))
            }
            name += ".xgf2"
        } else if !name.lowercased().hasSuffix(".xgf2") {
            name += ".xgf2"
        }
        return url.deletingLastPathComponent().appendingPathComponent(name)
    }

    func save() {
        guard ready, !busy, !cps.isEmpty else { return }
        let panel = NSSavePanel()
        panel.canCreateDirectories = true
        panel.isExtensionHidden = false
        panel.title = "Save reading font"
        // Suggested name includes .xgf2. Declaring the type in Info.plist lets the
        // panel treat that suffix as the extension instead of appending it again.
        panel.nameFieldStringValue = FaceCatalog.fileName(postscript: faceID, device: device.rawValue)
        if let type = UTType(filenameExtension: "xgf2") {
            panel.allowedContentTypes = [type]
        }
        guard panel.runModal() == .OK, var url = panel.url else { return }
        url = Self.xgf2URL(url)
        let parent = url.deletingLastPathComponent()
        let partial = parent.appendingPathComponent("." + url.lastPathComponent + ".partial")
        let cps = cps
        let ps = faceID
        let em = em
        let rubyEm = rubyEm
        let flag = OSAllocatedUnfairLock(initialState: false)
        cancelFlag = flag
        busy = true
        fraction = 0
        failed = false
        status = "Rasterizing…"
        let gate = ProgressGate()
        DispatchQueue.global(qos: .userInitiated).async {
            do {
                guard let body = Rasterizer(postscript: ps, em: em),
                      let ruby = Rasterizer(postscript: ps, em: rubyEm) else {
                    throw XgfError.io("Could not open \(ps).")
                }
                try XgfEncode.write(
                    to: partial,
                    em: em,
                    rubyEm: rubyEm,
                    cps: cps,
                    raster: { cp, size in
                        size == em ? body.packed(cp: cp) : ruby.packed(cp: cp)
                    },
                    progress: { done, total in
                        guard gate.allow(done, total: total) else { return }
                        DispatchQueue.main.async {
                            self.fraction = Double(done) / Double(max(total, 1))
                            self.status = "Rasterizing \(done.formatted()) of \(total.formatted())"
                        }
                    },
                    cancelled: { flag.withLock { $0 } }
                )
                if FileManager.default.fileExists(atPath: url.path) {
                    _ = try FileManager.default.replaceItemAt(url, withItemAt: partial)
                } else {
                    try FileManager.default.moveItem(at: partial, to: url)
                }
                let bytes = (try? FileManager.default.attributesOfItem(atPath: url.path)[.size] as? Int) ?? 0
                DispatchQueue.main.async {
                    self.busy = false
                    self.fraction = 1
                    self.failed = false
                    self.status = "Saved \(url.lastPathComponent) (\(Self.formatBytes(bytes)))."
                    NSWorkspace.shared.activateFileViewerSelecting([url])
                }
            } catch {
                try? FileManager.default.removeItem(at: partial)
                DispatchQueue.main.async {
                    self.busy = false
                    let cancelled = (error as? XgfError).map { if case .cancelled = $0 { return true }; return false } ?? false
                    self.failed = !cancelled
                    self.status = cancelled ? "Cancelled." : (error.localizedDescription)
                }
            }
        }
    }

    func cancel() {
        cancelFlag?.withLock { $0 = true }
    }

    static func formatBytes(_ n: Int) -> String {
        let mb = Double(n) / 1024 / 1024
        if mb >= 0.1 {
            return String(format: "%.1f MB", mb)
        }
        return String(format: "%.0f KB", Double(n) / 1024)
    }
}

private final class ProgressGate: @unchecked Sendable {
    private var last = -1000

    func allow(_ done: Int, total: Int) -> Bool {
        if done == total || done - last >= 200 {
            last = done
            return true
        }
        return false
    }
}

struct ContentView: View {
    @StateObject private var model = FontModel()

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            VStack(alignment: .leading, spacing: 4) {
                Text("Crossjp Font")
                    .font(.title2.weight(.semibold))
                Text("Make a reading font from a font on this Mac. The reader draws each character from this file, at one fixed size, in four gray levels.")
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }

            Grid(alignment: .leading, horizontalSpacing: 12, verticalSpacing: 10) {
                GridRow {
                    Text("Reader")
                    Picker("Reader", selection: $model.device) {
                        ForEach(ReaderDevice.allCases) { device in
                            Text("\(device.title)  \(device.page)").tag(device)
                        }
                    }
                    .pickerStyle(.segmented)
                    .labelsHidden()
                }
                GridRow {
                    Text("Size")
                    VStack(alignment: .leading, spacing: 2) {
                        HStack {
                            Slider(value: $model.pt, in: 10...18, step: 0.5)
                            Text(sizeLabel)
                                .frame(width: 196, alignment: .leading)
                                .monospacedDigit()
                        }
                        if model.rawEm > 64 {
                            Text("The reader accepts at most 64 px, so this is saved as 64.")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                    }
                }
                GridRow {
                    Text("Font")
                    Picker("Font", selection: $model.faceID) {
                        ForEach(model.faces) { face in
                            Text(face.title).tag(face.id)
                        }
                    }
                    .labelsHidden()
                    .disabled(model.faces.isEmpty || model.busy)
                }
            }

            preview

            if model.ready {
                Text("\(model.glyphCount.formatted()) glyphs, \(model.rubyCount.formatted()) ruby, \(FontModel.formatBytes(model.byteCount)). Characters above U+FFFF, including 𠮟, are left out.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }

            HStack {
                Button("Save .xgf2…") { model.save() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(!model.ready || model.busy)
                if model.busy {
                    Button("Cancel") { model.cancel() }
                }
            }

            if model.busy {
                ProgressView(value: model.fraction)
            }
            if !model.status.isEmpty {
                Text(model.status)
                    .foregroundStyle(model.failed ? Color.red : Color.primary)
                    .textSelection(.enabled)
                    .fixedSize(horizontal: false, vertical: true)
            }

            Text("Put the file on the reader from the Wi-Fi page, or copy it to the SD card at /.crossjp/fonts/. On the device, open 設定 → 本文フォント and choose it. The book lays out again at this size.")
                .font(.callout)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(24)
        .frame(minWidth: 640, minHeight: 560)
        .onAppear { model.boot() }
        .onChange(of: model.faceID) { _ in model.refreshSoon() }
        .onChange(of: model.device) { _ in model.refreshSoon() }
        .onChange(of: model.pt) { _ in model.refreshSoon() }
    }

    private var sizeLabel: String {
        let ptText = model.pt.truncatingRemainder(dividingBy: 1) == 0
            ? String(format: "%.0f pt", model.pt)
            : String(format: "%.1f pt", model.pt)
        return "\(ptText) · \(model.em)×\(model.em), ruby \(model.rubyEm)"
    }

    @ViewBuilder
    private var preview: some View {
        if model.preview.isEmpty {
            RoundedRectangle(cornerRadius: 8)
                .fill(Color.white)
                .frame(height: 72)
                .overlay(Text(model.ready ? "" : " ").foregroundStyle(.secondary))
        } else {
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 2) {
                    ForEach(Array(model.preview.enumerated()), id: \.offset) { _, image in
                        Image(nsImage: image)
                            .interpolation(.none)
                            .resizable()
                            .frame(width: CGFloat(model.em), height: CGFloat(model.em))
                            .border(Color.black.opacity(0.12))
                    }
                }
                .padding(10)
            }
            .background(Color.white)
            .clipShape(RoundedRectangle(cornerRadius: 8))
        }
    }
}
