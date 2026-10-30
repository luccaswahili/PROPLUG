// One frame of a video as a 16-bit PNG, decoded at full precision (10-bit log footage keeps its
// code values; 8-bit extraction would quantise them before the test sees them):
// swift tools/extract_frame16.swift CLIP SECONDS OUT.png [WIDTH]
import AVFoundation
import AppKit

let a = CommandLine.arguments
guard a.count >= 4, let seconds = Double(a[2]) else {
    fputs("usage: extract_frame16 CLIP SECONDS OUT.png [WIDTH]\n", stderr)
    exit(1)
}
let asset = AVURLAsset(url: URL(fileURLWithPath: a[1]))
guard let track = asset.tracks(withMediaType: .video).first, let reader = try? AVAssetReader(asset: asset) else {
    fputs("no video track\n", stderr)
    exit(1)
}
// code values as stored: Y'CbCr -> R'G'B' with the clip's matrix, no transfer or gamut conversion
let out = AVAssetReaderTrackOutput(track: track, outputSettings: [
    kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_64RGBALE,
])
let fps = Double(track.nominalFrameRate > 0 ? track.nominalFrameRate : 25)
reader.timeRange = CMTimeRange(start: CMTime(seconds: seconds, preferredTimescale: 600),
                               duration: CMTime(seconds: 2 / fps, preferredTimescale: 600))
reader.add(out)
reader.startReading()
guard let sample = out.copyNextSampleBuffer(), let pb = CMSampleBufferGetImageBuffer(sample) else {
    fputs("no frame at \(seconds)s (\(reader.error?.localizedDescription ?? "?"))\n", stderr)
    exit(1)
}
CVPixelBufferLockBaseAddress(pb, .readOnly)
let w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb)
let cs = CGColorSpace(name: CGColorSpace.itur_709)!
let ctx = CGContext(data: CVPixelBufferGetBaseAddress(pb), width: w, height: h, bitsPerComponent: 16,
                    bytesPerRow: CVPixelBufferGetBytesPerRow(pb), space: cs,
                    bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue | CGBitmapInfo.byteOrder16Little.rawValue)!
var img = ctx.makeImage()!
CVPixelBufferUnlockBaseAddress(pb, .readOnly)
if a.count >= 5, let width = Int(a[4]), width < w {
    let hh = Int((Double(h) * Double(width) / Double(w)).rounded())
    let c2 = CGContext(data: nil, width: width, height: hh, bitsPerComponent: 16, bytesPerRow: 0, space: cs,
                       bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue | CGBitmapInfo.byteOrder16Little.rawValue)!
    c2.interpolationQuality = .high
    c2.draw(img, in: CGRect(x: 0, y: 0, width: width, height: hh))
    img = c2.makeImage()!
}
try! NSBitmapImageRep(cgImage: img).representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: a[3]))
print("\(a[3]) \(img.width)x\(img.height) 16-bit, \(CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample)))s")
