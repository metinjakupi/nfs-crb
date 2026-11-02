#pragma once
#include "webgpu_device.h"
#include "../../web_performance.h"
#include <emscripten/emscripten.h>
#include <memory>

// At most one frame sampled per 120 presents, one readback in flight, 512 passes.
// Timestamp every render pass in that frame across all its encoder submissions.
// Copies/final composition are excluded; report that scope rather than "GPU time".
class WebGpuPerformanceTiming {
    static constexpr uint32_t capacity = 1024;
    struct Resources {
        WGPUQuerySet queries = nullptr;
        WGPUBuffer resolve = nullptr, read = nullptr;
        bool busy = false;
        ~Resources() {
            if (read)
                wgpuBufferRelease(read);
            if (resolve)
                wgpuBufferRelease(resolve);
            if (queries)
                wgpuQuerySetRelease(queries);
        }
    };
    std::shared_ptr<Resources> resources;
    uint32_t used = 0, frames = 0;
    bool active = false, overflow = false, unavailable = false;
    double started = 0;

  public:
    void pass(WGPURenderPassDescriptor &descriptor, WGPUPassTimestampWrites &writes) {
        if (!active || !host_performance_reports_enabled())
            return;
        if (used + 2 > capacity) {
            overflow = true;
            return;
        }
        writes.querySet = resources->queries;
        writes.beginningOfPassWriteIndex = used++;
        writes.endOfPassWriteIndex = used++;
        descriptor.timestampWrites = &writes;
    }
    // Called after the last game command buffer of a frame has been submitted.
    void presented(gpu::WebGpuDevice *device) {
        if (!host_performance_reports_enabled() || unavailable)
            return;
        const auto dev = device->native_device();
        const auto queue = device->native_queue();
        if (active) {
            active = false;
            if (used && !overflow) {
                auto r = resources;
                auto encoder = wgpuDeviceCreateCommandEncoder(dev, nullptr);
                wgpuCommandEncoderResolveQuerySet(encoder, r->queries, 0, used, r->resolve, 0);
                wgpuCommandEncoderCopyBufferToBuffer(encoder, r->resolve, 0, r->read, 0, used * 8);
                auto commands = wgpuCommandEncoderFinish(encoder, nullptr);
                wgpuCommandEncoderRelease(encoder);
                wgpuQueueSubmit(queue, 1, &commands);
                wgpuCommandBufferRelease(commands);
                r->busy = true;
                struct Job {
                    std::shared_ptr<Resources> r;
                    uint32_t used;
                    double started;
                };
                auto *job = new Job{r, used, started};
                WGPUBufferMapCallbackInfo info = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
                info.mode = WGPUCallbackMode_AllowSpontaneous;
                info.userdata1 = job;
                info.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void *data, void *) {
                    // emdawnwebgpu holds its event lock in the callback; unmap on the event loop.
                    gpu::WebGpuDevice::later([status, data] {
                        std::unique_ptr<Job> j(static_cast<Job *>(data));
                        if (status == WGPUMapAsyncStatus_Success) {
                            auto *p = static_cast<const uint64_t *>(
                                wgpuBufferGetConstMappedRange(j->r->read, 0, j->used * 8));
                            double sum = 0;
                            bool valid = p != nullptr;
                            if (p)
                                for (uint32_t i = 0; i < j->used; i += 2) {
                                    if (p[i + 1] < p[i] || p[i + 1] - p[i] > 10000000000ull)
                                        valid = false;
                                    else
                                        sum += double(p[i + 1] - p[i]) / 1e6;
                                }
                            wgpuBufferUnmap(j->r->read);
                            if (valid)
                                host_performance_gpu(j->started, sum, int(j->used / 2));
                        }
                        j->r->busy = false;
                    });
                };
                wgpuBufferMapAsync(r->read, WGPUMapMode_Read, 0, used * 8, info);
            }
        }
        if (++frames % 120 != 0 || (resources && resources->busy))
            return;
        if (!resources) {
            if (!device->has_feature(WGPUFeatureName_TimestampQuery)) {
                unavailable = true;
                return;
            }
            auto r = std::make_shared<Resources>();
            WGPUQuerySetDescriptor q = WGPU_QUERY_SET_DESCRIPTOR_INIT;
            q.type = WGPUQueryType_Timestamp;
            q.count = capacity;
            r->queries = wgpuDeviceCreateQuerySet(dev, &q);
            WGPUBufferDescriptor b = WGPU_BUFFER_DESCRIPTOR_INIT;
            b.size = capacity * 8;
            b.usage = WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc;
            r->resolve = wgpuDeviceCreateBuffer(dev, &b);
            b.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
            r->read = wgpuDeviceCreateBuffer(dev, &b);
            if (!r->queries || !r->resolve || !r->read) {
                unavailable = true;
                return;
            }
            resources = std::move(r);
        }
        used = 0;
        overflow = false;
        active = true;
        started = emscripten_get_now();
    }
};
