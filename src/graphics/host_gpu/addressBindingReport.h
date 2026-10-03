#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_ADDRESSBINDINGREPORT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_ADDRESSBINDINGREPORT_H_

#include <cstdint>
#include <string>

namespace Libs::Graphics {

// Device-loss triage with VK_EXT_device_address_binding_report: the driver reports every GPU
// virtual address range it binds or releases for an object (image, buffer, memory). The ranges,
// the objects' descriptions and their recent history (unpinned, freed, ...) are kept so that a
// faulting address can be named: which object owned it, and how long ago it was released.

// KYTY_ADDRESS_BINDING_REPORT=0 (environment, read at device creation) turns it off.
[[nodiscard]] bool AddressBindingReportRequested();

// From the debug messenger callback.
void AddressBindingNote(uint32_t object_type, uint64_t handle, uint64_t base, uint64_t size,
                        bool bind);
// What a Vulkan object is (set when it is created), and events in its life.
void AddressBindingDescribeObject(uint64_t handle, std::string description);
void AddressBindingAnnotate(uint64_t handle, const char* event);

// Image views by the image they belong to, so a fault on a released image can list who still
// refers to one of its views.
void AddressBindingNoteView(uint64_t view, uint64_t image);
[[nodiscard]] uint64_t AddressBindingImageOfView(uint64_t view);
// A draw wrote a view of the image into its descriptor set (GPU thread); the report lists the
// recent draws that bound a released image.
void AddressBindingNoteDescriptor(uint64_t image, uint64_t shader_hash);
// Set by the bindless table: prints the slots (and keys) whose descriptor holds a view of the
// image.
void AddressBindingSetImageUserReporter(void (*reporter)(uint64_t image));

// Prints what owned the address: a live object, or the most recently released one.
void AddressBindingDescribe(uint64_t address);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_ADDRESSBINDINGREPORT_H_
