// Draws the app icon (a generic steering wheel, no game branding) as a 1024x1024 PNG.
// Usage: swift tools/app/make_icon.swift <out.png>

import AppKit

let size: CGFloat = 1024
let out = CommandLine.arguments[1]

let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(size), pixelsHigh: Int(size),
                           bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                           colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
let ctx = NSGraphicsContext.current!.cgContext

// macOS icon grid: 824 pt rounded square centred in 1024.
let tile = CGRect(x: 100, y: 100, width: 824, height: 824)
let shape = CGPath(roundedRect: tile, cornerWidth: 185, cornerHeight: 185, transform: nil)
ctx.addPath(shape)
ctx.clip()
let bg = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(),
                    colors: [CGColor(red: 0.16, green: 0.17, blue: 0.20, alpha: 1),
                             CGColor(red: 0.05, green: 0.05, blue: 0.07, alpha: 1)] as CFArray,
                    locations: [0, 1])!
ctx.drawLinearGradient(bg, start: CGPoint(x: 0, y: 924), end: CGPoint(x: 0, y: 100), options: [])

// Diagonal red racing stripe.
ctx.setFillColor(CGColor(red: 0.89, green: 0.16, blue: 0.22, alpha: 1))
ctx.move(to: CGPoint(x: 100, y: 330))
ctx.addLine(to: CGPoint(x: 924, y: 760))
ctx.addLine(to: CGPoint(x: 924, y: 560))
ctx.addLine(to: CGPoint(x: 100, y: 130))
ctx.closePath()
ctx.fillPath()

// Steering wheel: rim, hub, three spokes.
let c = CGPoint(x: 512, y: 512)
ctx.setStrokeColor(.white)
ctx.setFillColor(.white)
ctx.setLineWidth(58)
ctx.strokeEllipse(in: CGRect(x: c.x - 265, y: c.y - 265, width: 530, height: 530))
ctx.fillEllipse(in: CGRect(x: c.x - 70, y: c.y - 70, width: 140, height: 140))
ctx.setLineWidth(52)
ctx.setLineCap(.round)
for angle in [180.0, 0.0, 270.0] {
    let r = angle * .pi / 180
    ctx.move(to: c)
    ctx.addLine(to: CGPoint(x: c.x + 250 * cos(r), y: c.y + 250 * sin(r)))
}
ctx.strokePath()

NSGraphicsContext.current = nil
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: out))
