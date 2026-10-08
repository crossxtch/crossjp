import AppKit
import CoreText
import Foundation

struct Face: Identifiable, Hashable {
    var postscript: String
    var title: String
    var rank: Int
    var id: String { postscript }
}

enum FaceCatalog {
    static let preferred = ["HiraMinProN-W3", "YuMin-Medium", "HiraginoSans-W3", "ToppanBunkyuMinchoPr6N-Regular"]

    static func list() -> [Face] {
        let manager = NSFontManager.shared
        var faces: [Face] = []
        var seen = Set<String>()
        for family in manager.availableFontFamilies {
            guard let members = manager.availableMembers(ofFontFamily: family) else { continue }
            for member in members {
                guard let ps = member.first as? String, seen.insert(ps).inserted else { continue }
                if ps.localizedCaseInsensitiveContains("LastResort") || ps.localizedCaseInsensitiveContains("Emoji") {
                    continue
                }
                guard let font = NSFont(name: ps, size: 16) else { continue }
                let traits = CTFontGetSymbolicTraits(font as CTFont)
                if traits.contains(.traitColorGlyphs) { continue }
                if !hasGlyph(font, 0x3042) || !hasGlyph(font, 0x6F22) { continue }
                let style = member.count > 1 ? (member[1] as? String ?? "") : ""
                let title = style.isEmpty ? family : "\(family) \(style)"
                faces.append(Face(postscript: ps, title: title, rank: rank(ps)))
            }
        }
        faces.sort { a, b in
            if a.rank != b.rank { return a.rank < b.rank }
            return a.title.localizedStandardCompare(b.title) == .orderedAscending
        }
        return faces
    }

    // BMP only. The reader’s cmap stores 16-bit code points, and at most 32767 glyphs.
    static func coverage(postscript: String, joyo: [UInt32]) -> [UInt32] {
        guard let font = NSFont(name: postscript, size: 32) else { return [0xFFFD] }
        let set = CTFontCopyCharacterSet(font as CTFont) as CharacterSet
        func has(_ cp: UInt32) -> Bool {
            guard let scalar = Unicode.Scalar(cp) else { return false }
            return set.contains(scalar)
        }
        var seen = Set<UInt32>()
        var out: [UInt32] = []
        out.reserveCapacity(16000)
        func add(_ cp: UInt32, force: Bool = false) {
            if out.count >= XgfEncode.maxGlyphs || !seen.insert(cp).inserted { return }
            if !force && !has(cp) {
                seen.remove(cp)
                return
            }
            out.append(cp)
        }
        add(0xFFFD, force: true)
        let ranges: [(UInt32, UInt32)] = [
            (0x0020, 0x007E),
            (0x00A0, 0x024F),
            (0x2010, 0x2050),
            (0x2190, 0x21FF),
            (0x2460, 0x2473),
            (0x3000, 0x30FF),
            (0x31F0, 0x31FF),
            (0xFE10, 0xFE19),
            (0xFE30, 0xFE4F),
            (0xFF00, 0xFFEF),
        ]
        for (lo, hi) in ranges {
            for cp in lo...hi {
                add(cp)
            }
        }
        for cp in joyo {
            add(cp)
        }
        let cjk: [(UInt32, UInt32)] = [
            (0x4E00, 0x9FFF),
            (0x3400, 0x4DBF),
            (0xF900, 0xFAFF),
        ]
        for (lo, hi) in cjk {
            for cp in lo...hi {
                if out.count >= XgfEncode.maxGlyphs { return out }
                add(cp)
            }
        }
        return out
    }

    static func joyo(from url: URL) -> [UInt32] {
        guard let text = try? String(contentsOf: url, encoding: .utf8) else { return [] }
        return text.unicodeScalars.compactMap { scalar in
            scalar.value > 0x20 ? scalar.value : nil
        }
    }

    static func joyoFromBundle() -> [UInt32] {
        guard let url = Bundle.main.url(forResource: "joyo_kanji", withExtension: "txt") else { return [] }
        return joyo(from: url)
    }

    static func fileName(postscript: String, device: String) -> String {
        let suffix = "-\(device).xgf2"
        let room = 79 - suffix.count
        let base = postscript.count > room ? String(postscript.prefix(room)) : postscript
        return base + suffix
    }

    private static func hasGlyph(_ font: NSFont, _ cp: UInt32) -> Bool {
        guard let scalar = Unicode.Scalar(cp) else { return false }
        var units = Array(String(scalar).utf16)
        var glyphs = [CGGlyph](repeating: 0, count: units.count)
        return CTFontGetGlyphsForCharacters(font as CTFont, &units, &glyphs, units.count)
    }

    private static func rank(_ ps: String) -> Int {
        switch ps {
        case "HiraMinProN-W3": return 0
        case "YuMin-Medium": return 1
        case "HiraginoSans-W3": return 2
        case "ToppanBunkyuMinchoPr6N-Regular": return 3
        default: break
        }
        if ps.hasPrefix("HiraMin") { return 10 }
        if ps.hasPrefix("YuMin-") { return 11 }
        if ps.hasPrefix("HiraginoSans-") { return 12 }
        if ps.hasPrefix("YuGo-") { return 13 }
        if ps.hasPrefix("HiraMaru") { return 14 }
        if ps.contains("Mincho") || ps.contains("Songti") { return 20 }
        if ps.contains("Gothic") || ps.contains("Kaku") || ps.contains("Klee") { return 30 }
        return 40
    }
}
