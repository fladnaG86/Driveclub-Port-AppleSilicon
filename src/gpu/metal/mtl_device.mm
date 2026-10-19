#include "gpu/metal/mtl_device.h"

#include <cstdlib>
#include <stdexcept>
#include <string>

#include <sys/sysctl.h>

#import <IOKit/IOKitLib.h>
#import <Metal/Metal.h>

#include "common/log.h"

namespace Gpu::Metal {

struct Device::Impl {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
};

static id<MTLDevice> PickDevice() {
    NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
    const char* wanted = std::getenv("DC_GPU");

    id<MTLDevice> best = nil;
    for (id<MTLDevice> dev in devices) {
        DC_INFO("gpu candidate: %s (low power: %d, metal3: %d)", dev.name.UTF8String,
                dev.isLowPower, [dev supportsFamily:MTLGPUFamilyMetal3]);
        if (![dev supportsFamily:MTLGPUFamilyMetal3]) {
            continue;
        }
        if (wanted) {
            if ([dev.name rangeOfString:@(wanted) options:NSCaseInsensitiveSearch].location !=
                NSNotFound) {
                return dev;
            }
            continue;
        }
        // Prefer the discrete GPU; among equals, the one with more memory.
        if (!best || (best.isLowPower && !dev.isLowPower) ||
            (best.isLowPower == dev.isLowPower &&
             dev.recommendedMaxWorkingSetSize > best.recommendedMaxWorkingSetSize)) {
            best = dev;
        }
    }
    return best;
}

static std::string SysctlString(const char* name) {
    char buf[256] = {};
    size_t len = sizeof(buf) - 1;
    return sysctlbyname(name, buf, &len, nullptr, 0) == 0 ? buf : "?";
}

static long long SysctlInt(const char* name) {
    long long v = 0;
    size_t len = sizeof(v);
    return sysctlbyname(name, &v, &len, nullptr, 0) == 0 ? v : -1;
}

static long long GpuCoreCount() {
    long long cores = -1;
    io_service_t gpu = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AGXAccelerator"));
    if (gpu) {
        if (CFTypeRef v = IORegistryEntryCreateCFProperty(gpu, CFSTR("gpu-core-count"), kCFAllocatorDefault, 0)) {
            if (CFGetTypeID(v) == CFNumberGetTypeID()) {
                CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberLongLongType, &cores);
            }
            CFRelease(v);
        }
        IOObjectRelease(gpu);
    }
    return cores;
}

// One line that identifies the machine in bug reports.
static void LogSystem() {
    NSString* version = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
    DC_INFO("system: Driveclub Native %s, %s, %s, %lld GPU cores, %lldP+%lldE CPU cores, %lld GB, "
            "macOS %s (%s)%s",
            version ? version.UTF8String : "dev build", SysctlString("hw.model").c_str(),
            SysctlString("machdep.cpu.brand_string").c_str(), GpuCoreCount(),
            SysctlInt("hw.perflevel0.physicalcpu"), SysctlInt("hw.perflevel1.physicalcpu"),
            SysctlInt("hw.memsize") >> 30, SysctlString("kern.osproductversion").c_str(),
            SysctlString("kern.osversion").c_str(),
            SysctlInt("sysctl.proc_translated") == 1 ? ", Rosetta" : "");
}

Device::Device() : impl(std::make_unique<Impl>()) {
    LogSystem();
    impl->device = PickDevice();
    if (!impl->device) {
        throw std::runtime_error("no Metal 3 capable GPU found");
    }
    // One queue for everything keeps ordering trivial; the PS4 compute rings
    // get mapped onto it later.
    impl->queue = [impl->device newCommandQueueWithMaxCommandBufferCount:64];

    id<MTLDevice> dev = impl->device;
    info.name = dev.name.UTF8String;
    info.unified_memory = dev.hasUnifiedMemory;
    info.argument_buffers_tier2 = dev.argumentBuffersSupport == MTLArgumentBuffersTier2;
    info.working_set_bytes = dev.recommendedMaxWorkingSetSize;

    DC_INFO("using gpu: %s, unified memory: %d, argument buffers tier2: %d, working set: %llu MB",
            info.name.c_str(), info.unified_memory, info.argument_buffers_tier2,
            info.working_set_bytes >> 20);
}

Device::~Device() = default;

void* Device::Handle() const {
    return (__bridge void*)impl->device;
}

void* Device::Queue() const {
    return (__bridge void*)impl->queue;
}

} // namespace Gpu::Metal
