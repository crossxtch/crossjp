import AppKit
import Foundation

private func fail(_ message: String) -> Never {
    fputs("fail: \(message)\n", stderr)
    exit(1)
}

private func inkCount(_ packed: Data, em: Int) -> (n: Int, cx: Int, cy: Int) {
    let rowBytes = (em + 3) / 4
    var n = 0
    var sx = 0
    var sy = 0
    for y in 0..<em {
        for x in 0..<em {
            let v = (packed[y * rowBytes + x / 4] >> (6 - (x % 4) * 2)) & 3
            if v >= 1 {
                n += 1
                sx += x
                sy += y
            }
        }
    }
    if n == 0 { return (0, 0, 0) }
    return (n, sx / n, sy / n)
}

private func testPack() {
    if XgfEncode.emSize(ppi: XgfEncode.ppiX3, pt: 13) != 47 {
        fail("x3 em")
    }
    if XgfEncode.emSize(ppi: XgfEncode.ppiX4, pt: 13) != 40 {
        fail("x4 em")
    }
    if XgfEncode.quantize(255) != 3 || XgfEncode.quantize(192) != 3 || XgfEncode.quantize(191) != 2 {
        fail("quantize")
    }
    if XgfEncode.quantize(160) != 2 || XgfEncode.quantize(96) != 1 || XgfEncode.quantize(63) != 0 {
        fail("quantize edge")
    }
    var gray = [UInt8](repeating: 0, count: 16 * 16)
    gray[0] = 255
    gray[1] = 160
    gray[2] = 96
    gray[3] = 0
    let packed = gray.withUnsafeBufferPointer { buf in
        XgfEncode.pack(gray: buf.baseAddress!, bytesPerRow: 16, em: 16)
    }
    if packed[0] != 0xE4 {
        fail(String(format: "pack byte %#x", packed[0]))
    }
}

private func testFile() throws {
    let url = URL(fileURLWithPath: "/tmp/crossjp-font-sample.xgf2")
    let cps: [UInt32] = [0x41, 0x42, 0x43]
    try XgfEncode.write(
        to: url,
        em: 16,
        rubyEm: 8,
        cps: cps,
        raster: { _, size in
            var data = Data(count: XgfEncode.slotStride(em: size))
            data[0] = size == 16 ? 0x11 : 0x22
            return data
        },
        progress: { _, _ in },
        cancelled: { false }
    )
    let data = try Data(contentsOf: url)
    let plan = XgfEncode.plan(cps)
    if plan.byId.count != 4 || plan.rubyIds.count != 3 || plan.intervals.count != 2 {
        fail("plan counts \(plan.byId.count) \(plan.rubyIds.count) \(plan.intervals.count)")
    }
    if plan.fileBytes(em: 16, rubyEm: 8) != 1072 || data.count != 1072 {
        fail("size \(data.count) plan \(plan.fileBytes(em: 16, rubyEm: 8))")
    }
    if data[0] != 0x58 || data[1] != 0x47 || data[2] != 0x46 || data[3] != 0x32 {
        fail("magic")
    }
    if data[40] != 0 || data[41] != 96 || data[42] != 160 || data[43] != 255 {
        fail("lut")
    }
    let interval: [UInt8] = [0x41, 0x00, 0x43, 0x00, 0x00, 0x00, 0xFD, 0xFF, 0xFD, 0xFF, 0x03, 0x00]
    if Array(data[64..<76]) != interval {
        fail("intervals \(Array(data[64..<76]))")
    }
    let rubyMap: [UInt8] = [0x00, 0x00, 0x01, 0x00, 0x02, 0x00]
    if Array(data[76..<82]) != rubyMap {
        fail("ruby map")
    }
    if data[512] != 0x11 || data[512 + 64] != 0x11 || data[1024] != 0x22 {
        fail("slots")
    }
}

private func testRaster() {
    guard let raster = Rasterizer(postscript: "HiraMinProN-W3", em: 32) else {
        fail("HiraMinProN-W3 missing")
    }
    raster.fillUser(CGRect(x: 0, y: 31, width: 1, height: 1))
    let top = raster.packCurrent()
    if top[0] >> 6 != 3 {
        fail("top row is not file row 0")
    }
    let a = raster.packed(cp: 0x3042)
    let stats = inkCount(a, em: 32)
    if stats.n < 30 {
        fail("あ ink \(stats.n)")
    }
    if abs(stats.cx - 16) > 6 || abs(stats.cy - 16) > 6 {
        fail("あ center \(stats.cx),\(stats.cy)")
    }
    guard let ruby = Rasterizer(postscript: "HiraMinProN-W3", em: 24) else {
        fail("ruby face")
    }
    let small = inkCount(ruby.packed(cp: 0x3042), em: 24)
    if small.n < 10 {
        fail("ruby あ ink \(small.n)")
    }
}

private func testRealWrite() throws {
    let cps: [UInt32] = [0x3042, 0x6F22, 0x3002, 0x41]
    let url = URL(fileURLWithPath: "/tmp/crossjp-font-real.xgf2")
    guard let body = Rasterizer(postscript: "HiraMinProN-W3", em: 32),
          let ruby = Rasterizer(postscript: "HiraMinProN-W3", em: 16) else {
        fail("real face")
    }
    try XgfEncode.write(
        to: url,
        em: 32,
        rubyEm: 16,
        cps: cps,
        raster: { cp, size in
            size == 32 ? body.packed(cp: cp) : ruby.packed(cp: cp)
        },
        progress: { _, _ in },
        cancelled: { false }
    )
    let data = try Data(contentsOf: url)
    let plan = XgfEncode.plan(cps)
    if data.count != plan.fileBytes(em: 32, rubyEm: 16) {
        fail("real size \(data.count)")
    }
    guard let gid = plan.byId.firstIndex(of: 0x3042) else { fail("あ id") }
    let rubyMap = XgfEncode.headerSize + plan.intervals.count * 6
    let bodyOff = XgfEncode.align(rubyMap + plan.rubyIds.count * 2)
    let stride = XgfEncode.slotStride(em: 32)
    let start = bodyOff + gid * stride
    let slot = data.subdata(in: start..<(start + stride))
    if inkCount(slot, em: 32).n < 30 {
        fail("written あ is blank")
    }
}

private func testCoverage() {
    let joyoURL = URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent()
        .deletingLastPathComponent()
        .deletingLastPathComponent()
        .appendingPathComponent("joyo_kanji.txt")
    let joyo = FaceCatalog.joyo(from: joyoURL)
    if joyo.count < 2000 {
        fail("joyo \(joyo.count) from \(joyoURL.path)")
    }
    let cps = FaceCatalog.coverage(postscript: "HiraMinProN-W3", joyo: joyo)
    if cps.count < 8000 || cps.count > XgfEncode.maxGlyphs {
        fail("coverage \(cps.count)")
    }
    if !cps.contains(0x6F22) || !cps.contains(0x3042) || !cps.contains(0xFFFD) {
        fail("missing 漢 or あ")
    }
    let faces = FaceCatalog.list()
    if !faces.contains(where: { $0.postscript == "HiraMinProN-W3" }) {
        fail("catalog missed Hiragino Mincho")
    }
    if faces.first?.postscript != "HiraMinProN-W3" {
        fail("default face \(faces.first?.postscript ?? "")")
    }
    print("ok faces=\(faces.count) glyphs=\(cps.count) joyo=\(joyo.count)")
}

@main
struct SelfTest {
    static func main() {
        let app = NSApplication.shared
        app.setActivationPolicy(.prohibited)
        testPack()
        do {
            try testFile()
            testRaster()
            try testRealWrite()
        } catch {
            fail(error.localizedDescription)
        }
        testCoverage()
    }
}
