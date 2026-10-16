#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SHADERRESOURCEBARRIER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SHADERRESOURCEBARRIER_H_

#include "graphics/host_gpu/graphicContext.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

namespace Libs::Graphics {

struct ShaderStageRuntime;
class CommandRecorder;

vk::ShaderStageFlagBits NativeShaderStage(ShaderType stage);
vk::PipelineStageFlags  ShaderPipelineStages(vk::ShaderStageFlags stages);
vk::MemoryBarrier       MakeShaderAccessDependency();
vk::MemoryBarrier       MakeShaderWriteHazardDependency();
vk::MemoryBarrier       MakeShaderWriteDependency();
vk::BufferMemoryBarrier MakeGdsDependency(vk::Buffer buffer);
bool HasShaderBufferWrites(const ShaderStageRuntime& runtime);
void ShaderAccessBarrier(vk::CommandBuffer vk_buffer, vk::PipelineStageFlags source_stages);
void ShaderWriteHazardBarrier(vk::CommandBuffer      vk_buffer,
                              vk::PipelineStageFlags destination_stages);
void ShaderWriteBarrier(vk::CommandBuffer vk_buffer, vk::PipelineStageFlags source_stages);
// Recorder versions (the raw ones forward to them).
void ShaderAccessBarrier(const CommandRecorder& vk_buffer, vk::PipelineStageFlags source_stages);
void ShaderWriteHazardBarrier(const CommandRecorder& vk_buffer,
                              vk::PipelineStageFlags destination_stages);
void ShaderWriteBarrier(const CommandRecorder& vk_buffer, vk::PipelineStageFlags source_stages);

// KYTY_SERIALIZE_GPU (live, research, default 0): 1 = a full memory barrier before every draw and
// dispatch (each waits for all earlier GPU work); 2 = one after every draw whose shaders write
// storage images or memory by address, which ShaderWriteBarrier does not cover. Tests whether
// one-frame glitches are ordering races (Wolverine 2026-10-13: effect geometry stretched across
// the screen for single frames: white/orange beams, orange floods).
int  SerializeGpuMode();
void FullMemoryBarrier(const CommandRecorder& vk_buffer);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SHADERRESOURCEBARRIER_H_
