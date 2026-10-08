import Foundation

// On-disk XGF2, matching lib/XgfFont/XgfFormat.h and tools/xgf2/gen_xgf2.py.
// Glyphs are fixed em boxes, 2 bits per pixel, MSB first, four pixels per byte.
// 0 is paper. 1, 2, and 3 are light, dark, and black ink.

enum XgfEncode {
    static let magic: UInt32 = 0x32464758
    static let version: UInt16 = 1
    static let headerSize = 64
    static let maxGlyphs = 32767
    static let ppiX3 = 259.0
    static let ppiX4 = 219.0

    static func emSize(ppi: Double, pt: Double) -> Int {
        min(64, max(4, Int((pt * ppi / 72.0).rounded())))
    }

    static func rubySize(_ em: Int) -> Int {
        max(1, (em + 1) / 2)
    }

    static func slotStride(em: Int) -> Int {
        ((em + 3) / 4) * em
    }

    static func align(_ n: Int) -> Int {
        (n + 511) & ~511
    }

    // Bright coverage (255 = solid ink on a black raster) to a 2-bit level.
    static func quantize(_ g: UInt8) -> UInt8 {
        if g >= 192 { return 3 }
        if g >= 128 { return 2 }
        if g >= 64 { return 1 }
        return 0
    }

    static func pack(gray: UnsafePointer<UInt8>, bytesPerRow: Int, em: Int) -> Data {
        let rowBytes = (em + 3) / 4
        var out = Data(count: rowBytes * em)
        out.withUnsafeMutableBytes { raw in
            let dst = raw.bindMemory(to: UInt8.self).baseAddress!
            for y in 0..<em {
                let row = gray + y * bytesPerRow
                for x in 0..<em {
                    let v = quantize(row[x]) & 3
                    let shift = 6 - (x % 4) * 2
                    dst[y * rowBytes + x / 4] |= v << shift
                }
            }
        }
        return out
    }

    static func needsRuby(_ cp: UInt32) -> Bool {
        if cp >= 0x20 && cp <= 0x7E { return true }
        if cp >= 0x3000 && cp <= 0x303F { return true }
        if cp >= 0x3040 && cp <= 0x30FF { return true }
        if cp >= 0x31F0 && cp <= 0x31FF { return true }
        if cp >= 0xFF00 && cp <= 0xFFEF { return true }
        return false
    }

    static func bucket(_ cp: UInt32) -> Int {
        if (0x20...0x7E).contains(cp) { return 0 }
        if (0x3000...0x30FF).contains(cp) || (0xFF00...0xFFEF).contains(cp) { return 1 }
        if (0x4E00...0x9FFF).contains(cp) || (0x3400...0x4DBF).contains(cp) || (0xF900...0xFAFF).contains(cp) {
            return 2
        }
        return 3
    }

    struct Plan {
        var byId: [UInt32]
        var intervals: [(first: UInt16, last: UInt16, glyph: UInt16)]
        var rubyIds: [UInt16]

        func fileBytes(em: Int, rubyEm: Int) -> Int {
            let bodyStride = XgfEncode.slotStride(em: em)
            let rubyStride = rubyEm > 0 ? XgfEncode.slotStride(em: rubyEm) : 0
            let rubyMap = XgfEncode.headerSize + intervals.count * 6
            let bodyOff = XgfEncode.align(rubyMap + rubyIds.count * 2)
            let bodyBytes = byId.count * bodyStride
            if rubyIds.isEmpty || rubyStride == 0 {
                return bodyOff + bodyBytes
            }
            let rubyOff = XgfEncode.align(bodyOff + bodyBytes)
            return rubyOff + rubyIds.count * rubyStride
        }
    }

    static func plan(_ cps: [UInt32]) -> Plan {
        var unique = Set<UInt32>()
        unique.reserveCapacity(cps.count + 1)
        for cp in cps where cp <= 0xFFFF {
            unique.insert(cp)
        }
        unique.insert(0xFFFD)
        let byId = unique.sorted { a, b in
            let ba = bucket(a)
            let bb = bucket(b)
            if ba != bb { return ba < bb }
            return a < b
        }
        var idOf: [UInt32: Int] = [:]
        idOf.reserveCapacity(byId.count)
        for (i, cp) in byId.enumerated() {
            idOf[cp] = i
        }
        let intervals = merge(idOf)
        var rubyIds: [UInt16] = []
        rubyIds.reserveCapacity(1024)
        for (gid, cp) in byId.enumerated() where needsRuby(cp) {
            rubyIds.append(UInt16(gid))
        }
        return Plan(byId: byId, intervals: intervals, rubyIds: rubyIds)
    }

    static func write(
        to url: URL,
        em: Int,
        rubyEm: Int,
        cps: [UInt32],
        raster: (UInt32, Int) -> Data,
        progress: (Int, Int) -> Void,
        cancelled: () -> Bool
    ) throws {
        if em < 4 || em > 64 || rubyEm < 1 || rubyEm > 64 {
            throw XgfError.io("Glyph size must be 4 to 64 pixels.")
        }
        let plan = plan(cps)
        if plan.byId.isEmpty || plan.byId.count > maxGlyphs || plan.intervals.isEmpty || plan.intervals.count > maxGlyphs {
            throw XgfError.noGlyphs
        }
        var covered = 0
        var prevLast: UInt16 = 0
        for (i, iv) in plan.intervals.enumerated() {
            if iv.first > iv.last || (i > 0 && iv.first <= prevLast) {
                throw XgfError.coverage
            }
            let span = Int(iv.last - iv.first) + 1
            if Int(iv.glyph) + span > plan.byId.count {
                throw XgfError.coverage
            }
            covered += span
            prevLast = iv.last
        }
        if covered != plan.byId.count {
            throw XgfError.coverage
        }

        let bodyStride = slotStride(em: em)
        let rubyStride = slotStride(em: rubyEm)
        let rubyMapOff = headerSize + plan.intervals.count * 6
        let bodyOff = align(rubyMapOff + plan.rubyIds.count * 2)
        let bodyBytes = plan.byId.count * bodyStride
        let rubyOff = plan.rubyIds.isEmpty ? 0 : align(bodyOff + bodyBytes)
        let total = plan.rubyIds.isEmpty ? bodyOff + bodyBytes : rubyOff + plan.rubyIds.count * rubyStride

        let tmp = url
        if FileManager.default.fileExists(atPath: tmp.path) {
            try FileManager.default.removeItem(at: tmp)
        }
        let sink = try Sink(tmp)
        try sink.write(header(
            em: em,
            rubyEm: rubyEm,
            bodyStride: bodyStride,
            rubyStride: rubyStride,
            bodyCount: plan.byId.count,
            rubyCount: plan.rubyIds.count,
            intervalCount: plan.intervals.count,
            intervalsOff: headerSize,
            rubyMapOff: rubyMapOff,
            bodyOff: bodyOff,
            rubyOff: rubyOff
        ))
        var table = Data()
        table.reserveCapacity(plan.intervals.count * 6 + plan.rubyIds.count * 2)
        for iv in plan.intervals {
            appendU16(iv.first, to: &table)
            appendU16(iv.last, to: &table)
            appendU16(iv.glyph, to: &table)
        }
        for gid in plan.rubyIds {
            appendU16(gid, to: &table)
        }
        try sink.write(table)
        try sink.writeZeros(bodyOff - sink.written)

        let steps = plan.byId.count + plan.rubyIds.count
        var done = 0
        for cp in plan.byId {
            if cancelled() { throw XgfError.cancelled }
            let slot = raster(cp, em)
            if slot.count != bodyStride { throw XgfError.badSlot }
            try sink.write(slot)
            done += 1
            progress(done, steps)
        }
        if rubyOff > sink.written {
            try sink.writeZeros(rubyOff - sink.written)
        }
        for gid in plan.rubyIds {
            if cancelled() { throw XgfError.cancelled }
            let cp = plan.byId[Int(gid)]
            let slot = raster(cp, rubyEm)
            if slot.count != rubyStride { throw XgfError.badSlot }
            try sink.write(slot)
            done += 1
            progress(done, steps)
        }
        sink.close()
        if sink.written != total {
            throw XgfError.io("Short write (\(sink.written) of \(total)).")
        }
    }

    private static func merge(_ idOf: [UInt32: Int]) -> [(first: UInt16, last: UInt16, glyph: UInt16)] {
        let cps = idOf.keys.sorted()
        guard let firstCp = cps.first, let firstId = idOf[firstCp] else { return [] }
        var out: [(first: UInt16, last: UInt16, glyph: UInt16)] = []
        var start = firstCp
        var prev = firstCp
        var startId = firstId
        var expected = firstId
        for cp in cps.dropFirst() {
            guard let gid = idOf[cp] else { continue }
            if cp == prev + 1 && gid == expected + 1 {
                prev = cp
                expected = gid
                continue
            }
            out.append((UInt16(start), UInt16(prev), UInt16(startId)))
            start = cp
            prev = cp
            startId = gid
            expected = gid
        }
        out.append((UInt16(start), UInt16(prev), UInt16(startId)))
        return out
    }

    private static func header(
        em: Int,
        rubyEm: Int,
        bodyStride: Int,
        rubyStride: Int,
        bodyCount: Int,
        rubyCount: Int,
        intervalCount: Int,
        intervalsOff: Int,
        rubyMapOff: Int,
        bodyOff: Int,
        rubyOff: Int
    ) -> Data {
        var h = Data(count: headerSize)
        poke32(&h, 0, magic)
        poke16(&h, 4, version)
        poke16(&h, 6, 1 | 2 | 4)
        h[8] = UInt8(em)
        h[9] = UInt8(rubyEm)
        h[10] = 2
        poke16(&h, 12, UInt16(bodyStride))
        poke16(&h, 14, UInt16(rubyStride))
        poke16(&h, 16, UInt16(bodyCount))
        poke16(&h, 18, UInt16(rubyCount))
        poke16(&h, 20, UInt16(intervalCount))
        poke32(&h, 24, UInt32(intervalsOff))
        poke32(&h, 28, UInt32(rubyMapOff))
        poke32(&h, 32, UInt32(bodyOff))
        poke32(&h, 36, UInt32(rubyOff))
        h[40] = 0
        h[41] = 96
        h[42] = 160
        h[43] = 255
        return h
    }

    private static func appendU16(_ v: UInt16, to data: inout Data) {
        data.append(UInt8(v & 0xFF))
        data.append(UInt8(v >> 8))
    }

    private static func poke16(_ data: inout Data, _ offset: Int, _ v: UInt16) {
        data[offset] = UInt8(v & 0xFF)
        data[offset + 1] = UInt8(v >> 8)
    }

    private static func poke32(_ data: inout Data, _ offset: Int, _ v: UInt32) {
        data[offset] = UInt8(v & 0xFF)
        data[offset + 1] = UInt8((v >> 8) & 0xFF)
        data[offset + 2] = UInt8((v >> 16) & 0xFF)
        data[offset + 3] = UInt8((v >> 24) & 0xFF)
    }
}

enum XgfError: LocalizedError {
    case noGlyphs
    case badSlot
    case coverage
    case cancelled
    case io(String)

    var errorDescription: String? {
        switch self {
        case .noGlyphs:
            return "This face has no characters the reader can store."
        case .badSlot:
            return "A glyph bitmap came out the wrong size."
        case .coverage:
            return "Internal font table error."
        case .cancelled:
            return "Cancelled."
        case .io(let message):
            return message
        }
    }
}

private final class Sink {
    let stream: OutputStream
    private(set) var written = 0

    init(_ url: URL) throws {
        guard let stream = OutputStream(url: url, append: false) else {
            throw XgfError.io("Could not create \(url.lastPathComponent).")
        }
        stream.open()
        if stream.streamStatus == .error {
            throw XgfError.io(stream.streamError?.localizedDescription ?? "Could not create the file.")
        }
        self.stream = stream
    }

    func write(_ data: Data) throws {
        try data.withUnsafeBytes { raw in
            guard let base = raw.bindMemory(to: UInt8.self).baseAddress else { return }
            try write(base, raw.count)
        }
    }

    func writeZeros(_ count: Int) throws {
        if count <= 0 { return }
        let chunk = [UInt8](repeating: 0, count: min(count, 8192))
        var left = count
        while left > 0 {
            let n = min(left, chunk.count)
            try chunk.withUnsafeBufferPointer { buf in
                try write(buf.baseAddress!, n)
            }
            left -= n
        }
    }

    func close() {
        stream.close()
    }

    private func write(_ ptr: UnsafePointer<UInt8>, _ count: Int) throws {
        var off = 0
        while off < count {
            let n = stream.write(ptr + off, maxLength: count - off)
            if n <= 0 {
                throw XgfError.io(stream.streamError?.localizedDescription ?? "Write failed.")
            }
            off += n
            written += n
        }
    }
}
