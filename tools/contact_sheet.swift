// Compose labelled PNG tiles into a grid: contact_sheet OUT.png COLUMNS tile1.png "label1" tile2.png "label2" ...
import AppKit

let a = CommandLine.arguments
guard a.count >= 5 else { fputs("usage: contact_sheet OUT.png COLUMNS (tile label)...\n", stderr); exit(1) }
let out = a[1], cols = Int(a[2])!
var tiles: [(NSImage, String)] = []
var i = 3
while i + 1 < a.count { if let img = NSImage(contentsOfFile: a[i]) { tiles.append((img, a[i + 1])) }; i += 2 }
guard let first = tiles.first?.0 else { exit(1) }
let tw = 480.0, th = tw * Double(first.size.height / first.size.width), label = 26.0, pad = 8.0
let rows = (tiles.count + cols - 1) / cols
let W = Double(cols) * (tw + pad) + pad, H = Double(rows) * (th + label + pad) + pad
let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(W), pixelsHigh: Int(H), bitsPerSample: 8,
                           samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
                           bytesPerRow: 0, bitsPerPixel: 0)!
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
NSColor(white: 0.12, alpha: 1).setFill()
NSRect(x: 0, y: 0, width: W, height: H).fill()
let attrs: [NSAttributedString.Key: Any] = [.font: NSFont.systemFont(ofSize: 15, weight: .medium),
                                            .foregroundColor: NSColor(white: 0.9, alpha: 1)]
for (n, (img, text)) in tiles.enumerated() {
    let c = Double(n % cols), r = Double(n / cols)
    let x = pad + c * (tw + pad), yTop = H - pad - r * (th + label + pad)
    img.draw(in: NSRect(x: x, y: yTop - th, width: tw, height: th))
    (text as NSString).draw(at: NSPoint(x: x + 4, y: yTop - th - label + 5), withAttributes: attrs)
}
NSGraphicsContext.restoreGraphicsState()
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: out))
