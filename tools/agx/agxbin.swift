// agxbin <file.metal> <out>: writes the AGX machine code of kernel `main0`
// (private _MTLPipelineStateBinary, found by introspection on macOS 27).
import Metal
import Foundation
let dev = MTLCreateSystemDefaultDevice()!
let src = try! String(contentsOfFile: CommandLine.arguments[1], encoding: .utf8)
let lib = try! dev.makeLibrary(source: src, options: nil)
let p = try! dev.makeComputePipelineState(function: lib.makeFunction(name: "main0")!) as AnyObject
let d = p.perform(NSSelectorFromString("pipelineBinaries")).takeUnretainedValue() as! NSDictionary
let b = (d["compute"] as! NSArray)[0] as AnyObject
let bin = b.perform(NSSelectorFromString("binary")).takeUnretainedValue()
guard let data = bin as? NSData else {
  FileHandle.standardError.write("binary is \(type(of: bin)), not data\n".data(using: .utf8)!)
  exit(1)
}
data.write(toFile: CommandLine.arguments[2], atomically: true)
