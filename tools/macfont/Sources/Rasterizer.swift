import AppKit
import CoreGraphics
import CoreText
import Foundation

// Draws one character into an em square. Ink is centered: the reader stores
// every glyph that way, and shifts 。、 at draw time.
final class Rasterizer {
    let em: Int
    private let nsFont: NSFont
    private let ctx: CGContext
    private let bytesPerRow: Int
    private let pixels: UnsafeMutablePointer<UInt8>

    init?(postscript: String, em: Int) {
        guard em >= 4, em <= 64, let nsFont = NSFont(name: postscript, size: CGFloat(em)) else {
            return nil
        }
        guard let ctx = CGContext(
            data: nil,
            width: em,
            height: em,
            bitsPerComponent: 8,
            bytesPerRow: 0,
            space: CGColorSpaceCreateDeviceGray(),
            bitmapInfo: CGImageAlphaInfo.none.rawValue
        ), let data = ctx.data else {
            return nil
        }
        self.em = em
        self.nsFont = nsFont
        self.ctx = ctx
        self.bytesPerRow = ctx.bytesPerRow
        self.pixels = data.assumingMemoryBound(to: UInt8.self)
        ctx.setAllowsAntialiasing(true)
        ctx.setShouldAntialias(true)
        // Same as cookxtch. Off, a 47px 明朝 stem loses about a quarter of its
        // solid-black pixels and the page looks light.
        ctx.setShouldSmoothFonts(true)
        ctx.setAllowsFontSmoothing(true)
        ctx.interpolationQuality = .high
    }

    func packed(cp: UInt32) -> Data {
        clear()
        let drew = Unicode.Scalar(cp).map { draw(String($0)) } ?? false
        if !drew && cp == 0xFFFD {
            drawBox()
        }
        return packCurrent()
    }

    func fillUser(_ rect: CGRect) {
        clear()
        ctx.setFillColor(gray: 1, alpha: 1)
        ctx.fill(rect)
    }

    func packCurrent() -> Data {
        XgfEncode.pack(gray: pixels, bytesPerRow: bytesPerRow, em: em)
    }

    func pageImage(cp: UInt32) -> CGImage? {
        let packed = packed(cp: cp)
        return Self.pageImage(packed: packed, em: em)
    }

    static func pageImage(packed: Data, em: Int) -> CGImage? {
        let rowBytes = (em + 3) / 4
        var rgb = [UInt8](repeating: 255, count: em * em * 4)
        for y in 0..<em {
            for x in 0..<em {
                let byte = packed[y * rowBytes + x / 4]
                let v = (byte >> (6 - (x % 4) * 2)) & 3
                let g: UInt8
                switch v {
                case 1: g = 170
                case 2: g = 85
                case 3: g = 0
                default: g = 255
                }
                let i = (y * em + x) * 4
                rgb[i] = g
                rgb[i + 1] = g
                rgb[i + 2] = g
                rgb[i + 3] = 255
            }
        }
        let data = Data(rgb) as CFData
        guard let provider = CGDataProvider(data: data) else { return nil }
        return CGImage(
            width: em,
            height: em,
            bitsPerComponent: 8,
            bitsPerPixel: 32,
            bytesPerRow: em * 4,
            space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue),
            provider: provider,
            decode: nil,
            shouldInterpolate: false,
            intent: .defaultIntent
        )
    }

    private func clear() {
        ctx.setFillColor(gray: 0, alpha: 1)
        ctx.fill(CGRect(x: 0, y: 0, width: em, height: em))
    }

    @discardableResult
    private func draw(_ text: String) -> Bool {
        let line = CTLineCreateWithAttributedString(NSAttributedString(string: text, attributes: [
            .font: nsFont,
            .foregroundColor: NSColor.white,
        ]))
        let bounds = CTLineGetBoundsWithOptions(line, .useGlyphPathBounds)
        if bounds.isNull || bounds.isInfinite || bounds.width < 0.3 || bounds.height < 0.3 {
            return false
        }
        ctx.textMatrix = .identity
        ctx.textPosition = CGPoint(x: CGFloat(em) / 2 - bounds.midX, y: CGFloat(em) / 2 - bounds.midY)
        CTLineDraw(line, ctx)
        return true
    }

    private func drawBox() {
        let inset = max(2, em / 8)
        ctx.setStrokeColor(gray: 1, alpha: 1)
        ctx.setLineWidth(max(1, CGFloat(em) / 16))
        let side = CGFloat(em - inset * 2)
        ctx.stroke(CGRect(x: CGFloat(inset), y: CGFloat(inset), width: side, height: side))
    }
}
