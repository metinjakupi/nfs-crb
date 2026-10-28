// Firefox rejects a dynamic-offset view when its backing Wasm memory can grow
// beyond 2 GiB, even when WebGPU will read only four offsets. Copy the requested
// words into a small owned array before calling setBindGroup.
addToLibrary({
  recompWgpuRenderPassSetBindGroup__deps: ['$WebGPU'],
  recompWgpuRenderPassSetBindGroup: (passPtr, groupIndex, groupPtr, count, offsetsPtr) => {
    const pass = WebGPU.getJsObject(passPtr);
    const group = WebGPU.getJsObject(groupPtr);
    if (!count) {
      pass.setBindGroup(groupIndex, group);
      return;
    }
    const first = offsetsPtr >>> 2;
    const offsets = HEAPU32.slice(first, first + count);
    pass.setBindGroup(groupIndex, group, offsets);
  },
});
