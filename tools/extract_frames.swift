// Grab frames from video files as PNG (code values as stored, no colour conversion), for the
// consistency test: swift tools/extract_frames.swift OUTDIR WIDTH FRAMES_PER_CLIP clip1.mov clip2.mp4 ...
import AVFoundation
import AppKit

let a = CommandLine.arguments
guard a.count >= 5, let width = Int(a[2]), let count = Int(a[3]) else {
    fputs("usage: extract_frames OUTDIR WIDTH FRAMES_PER_CLIP clip...\n", stderr)
    exit(1)
}
let outDir = URL(fileURLWithPath: a[1])
try? FileManager.default.createDirectory(at: outDir, withIntermediateDirectories: true)

for path in a[4...] {
    let url = URL(fileURLWithPath: path)
    let asset = AVURLAsset(url: url)
    let gen = AVAssetImageGenerator(asset: asset)
    gen.appliesPreferredTrackTransform = true
    gen.requestedTimeToleranceBefore = .zero
    gen.requestedTimeToleranceAfter = .zero
    let dur = asset.duration.seconds
    let stem = url.deletingPathExtension().lastPathComponent.replacingOccurrences(of: " ", with: "_")
    for i in 0..<count {
        let t = CMTime(seconds: dur * (Double(i) + 0.5) / Double(count), preferredTimescale: 600)
        guard let img = try? gen.copyCGImage(at: t, actualTime: nil) else {
            fputs("\(stem): no frame at \(t.seconds)s\n", stderr)
            continue
        }
        let h = Int((Double(img.height) * Double(width) / Double(img.width)).rounded())
        // draw in the image's own colour space so the stored code values pass through unchanged
        let cs = img.colorSpace ?? CGColorSpace(name: CGColorSpace.sRGB)!
        guard let ctx = CGContext(data: nil, width: width, height: h, bitsPerComponent: 8, bytesPerRow: 0, space: cs,
                                  bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue) else { continue }
        ctx.interpolationQuality = .high
        ctx.draw(img, in: CGRect(x: 0, y: 0, width: width, height: h))
        let rep = NSBitmapImageRep(cgImage: ctx.makeImage()!)
        let file = outDir.appendingPathComponent("\(stem)-\(i).png")
        try! rep.representation(using: .png, properties: [:])!.write(to: file)
        print("\(file.path) (\(img.width)x\(img.height) at \(String(format: "%.2f", t.seconds))s, \(cs.name ?? "?" as CFString))")
    }
}
