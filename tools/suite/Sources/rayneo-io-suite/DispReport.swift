//
//  DispReport.swift
//
//  Decodes the revision 5 display-probe report (patches/disp.c, kind 0x19):
//  the stock LVGL fbdev driver's geometry plus the LCDC parallax state.
//

import Foundation

struct DispReport {
    static let kind: UInt8 = 0x19

    let mode: UInt8
    let rc: Int8
    let disp: UInt32
    let drv: UInt32
    let fd: Int32
    let fmt: UInt8
    let xres: UInt16
    let yres: UInt16
    let nplanes: UInt8
    let fbmem: UInt32
    let fblen: UInt32
    let stride: UInt16
    let display: UInt8
    let bpp: UInt8
    let xresVirtual: UInt32
    let yresVirtual: UInt32
    let xoffset: UInt32
    let yoffset: UInt32
    let map: UInt32
    let buf2: UInt32
    let yoffset2: UInt32
    let colorFormat: UInt8
    let width: UInt32
    let height: UInt32
    let drawStride: UInt32
    let refrPass: UInt8
    let leftParallax: Int32
    let rightParallax: Int32
    let depthMode: Int32
    let dispMode: Int32
    let lcdc: [UInt8]

    init?(_ data: Data) {
        let b = [UInt8](data)
        guard b.count >= 95, b[0] == Self.kind else { return nil }
        var i = 1
        func u8() -> UInt8 { defer { i += 1 }; return b[i] }
        func u16() -> UInt16 { defer { i += 2 }; return UInt16(b[i]) | UInt16(b[i + 1]) << 8 }
        func u32() -> UInt32 { defer { i += 4 }; return UInt32(b[i]) | UInt32(b[i + 1]) << 8 | UInt32(b[i + 2]) << 16 | UInt32(b[i + 3]) << 24 }
        mode = u8()
        rc = Int8(bitPattern: u8())
        disp = u32()
        drv = u32()
        fd = Int32(bitPattern: u32())
        fmt = u8()
        xres = u16()
        yres = u16()
        nplanes = u8()
        fbmem = u32()
        fblen = u32()
        stride = u16()
        display = u8()
        bpp = u8()
        xresVirtual = u32()
        yresVirtual = u32()
        xoffset = u32()
        yoffset = u32()
        map = u32()
        buf2 = u32()
        yoffset2 = u32()
        colorFormat = u8()
        width = u32()
        height = u32()
        drawStride = u32()
        refrPass = u8()
        leftParallax = Int32(bitPattern: u32())
        rightParallax = Int32(bitPattern: u32())
        depthMode = Int32(bitPattern: u32())
        dispMode = Int32(bitPattern: u32())
        lcdc = Array(b[i ..< min(i + 4, b.count)])
    }

    var summary: String {
        let h = { (v: UInt32) in String(format: "0x%08x", v) }
        return "mode \(mode) rc \(rc) fd \(fd) fmt \(fmt) \(xres)x\(yres) planes \(nplanes) fbmem \(h(fbmem)) len \(fblen) stride \(stride) bpp \(bpp) virtual \(xresVirtual)x\(yresVirtual) off \(xoffset),\(yoffset) map \(h(map)) buf2 \(h(buf2)) yoff2 \(yoffset2) cf \(colorFormat) draw \(width)x\(height)/\(drawStride) pass \(refrPass) parallax \(leftParallax)/\(rightParallax) depthMode \(depthMode) dispMode \(dispMode) lcdc \(lcdc.map { String($0) }.joined(separator: ","))"
    }
}
