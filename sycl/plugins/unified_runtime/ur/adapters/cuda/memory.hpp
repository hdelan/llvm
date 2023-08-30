//===--------- memory.hpp - CUDA Adapter ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#pragma once

#include <cassert>
#include <cuda.h>
#include <ur_api.h>

#include "common.hpp"
#include "context.hpp"
#include "event.hpp"

/// UR Mem mapping to CUDA memory allocations, both data and texture/surface.
/// \brief Represents non-SVM allocations on the CUDA backend.
/// Keeps tracks of all mapped regions used for Map/Unmap calls.
/// Only one region can be active at the same time per allocation.
struct ur_mem_handle_t_ {
  // Context where the memory object is accessible.
  ur_context_handle_t Context;

  // If we make a ur_mem_handle_t_ from a native allocation, it can be useful to
  // associate it with the device that holds the native allocation.
  ur_device_handle_t DeviceWithNativeAllocation{nullptr};

  /// Reference counting of the handler
  std::atomic_uint32_t RefCount{1};

  // Original mem flags passed
  ur_mem_flags_t MemFlags;

  // Enumerates all possible types of accesses.
  enum access_mode_t { unknown, read_write, read_only, write_only };

  // Methods to get type of the derived object (image or buffer)
  virtual bool isBuffer() const = 0;
  virtual bool isImage() const = 0;

  ur_context_handle_t getContext() const noexcept { return Context; }

  uint32_t incrementReferenceCount() noexcept { return ++RefCount; }

  uint32_t decrementReferenceCount() noexcept { return --RefCount; }

  uint32_t getReferenceCount() const noexcept { return RefCount; }

  virtual ur_result_t allocateMemObjOnDeviceIfNeeded(ur_device_handle_t) = 0;
  virtual ur_result_t migrateMemoryToDeviceIfNeeded(ur_device_handle_t) = 0;

  virtual ur_result_t clear() = 0;

  virtual ~ur_mem_handle_t_() = 0;

  ur_mutex MemoryAllocationMutex; // A mutex for allocations
  ur_mutex MemoryMigrationMutex;  // A mutex for memory transfers

protected:
  ur_mem_handle_t_(ur_context_handle_t Context, ur_mem_flags_t MemFlags)
      : Context{Context}, MemFlags{MemFlags} {
    urContextRetain(Context);
  };

  // In the case that a ur_mem_handle_t is created with a native allocation,
  // it can be useful to keep track of the device that has the original native
  // allocation so that we know not to free the memory on destruction
  ur_mem_handle_t_(ur_context_handle_t Context, ur_device_handle_t Device,
                   ur_mem_flags_t MemFlags)
      : Context{Context}, DeviceWithNativeAllocation{Device},
        MemFlags{MemFlags} {
    urContextRetain(Context);
    urDeviceRetain(Device);
  };
};

// Handler for plain, pointer-based CUDA allocations.
//
// Since a ur_buffer_ is associated with a ur_context_handle_t_, which may
// contain multiple devices, each ur_buffer_ contains a vector of native
// allocations, one allocation for each device in the ur_context_handle_t_.
// Native allocations are made lazily, before a `ur_buffer_` is needed on a
// particular device.
//
// The ur_buffer_ is also responsible for migrating memory between native
// allocations. This migration happens lazily. The ur_buffer_ relies on knowing
// which event was the last to write to the mem obj `LastEventWritingToMemObj`.
// All subsequent reads must wait on this event.
//
struct ur_buffer_ final : ur_mem_handle_t_ {
  using native_type = CUdeviceptr;

  // If this is a subbuffer then this will point to the parent buffer
  ur_buffer_ *Parent{nullptr};

  // CUDA handler for the pointers. We hold a ptr for each device in our
  // context. Each device in the context is identified by its index
  std::vector<native_type> Ptrs;

  // Has the memory been migrated to a device since the last write?
  std::vector<bool> HaveMigratedToDeviceSinceLastWrite;

  /// Pointer associated with this device on the host
  void *HostPtr;
  /// Size of the allocation in bytes
  size_t Size;
  /// Size of the active mapped region.
  size_t MapSize;
  /// Offset of the active mapped region.
  size_t MapOffset;
  /// Pointer to the active mapped region, if any
  void *MapPtr;
  /// Original flags for the mapped region
  ur_map_flags_t MapFlags;

  // We should wait on this event prior to migrating memory across allocations
  // in this ur_buffer_
  ur_event_handle_t LastEventWritingToMemObj{nullptr};

  /** AllocMode
   * classic: Just a normal buffer allocated on the device via cuda malloc
   * use_host_ptr: Use an address on the host for the device
   * copy_in: The data for the device comes from the host but the host
   pointer is not available later for re-use
   * alloc_host_ptr: Uses pinned-memory allocation
  */
  enum class AllocMode {
    Classic,
    UseHostPtr,
    CopyIn,
    AllocHostPtr,
  } MemAllocMode;

  ur_buffer_(ur_context_handle_t Context, ur_buffer_ *Parent,
             ur_mem_flags_t MemFlags, AllocMode Mode, void *HostPtr,
             size_t Size)
      : ur_mem_handle_t_{Context, MemFlags}, Parent{Parent},
        Ptrs(Context->NumDevices, native_type{0}),
        HaveMigratedToDeviceSinceLastWrite(Context->NumDevices, false),
        HostPtr{HostPtr}, Size{Size}, MapSize{0}, MapOffset{0}, MapPtr{nullptr},
        MapFlags{UR_MAP_FLAG_WRITE}, MemAllocMode{Mode} {
    if (isSubBuffer()) {
      urMemRetain(Parent);
    }
  }

  ur_buffer_(ur_context_handle_t Context, ur_device_handle_t Device,
             ur_buffer_ *Parent, ur_mem_flags_t MemFlags, AllocMode Mode,
             void *HostPtr, size_t Size)
      : ur_mem_handle_t_{Context, Device, MemFlags}, Parent{Parent},
        Ptrs(Context->NumDevices, native_type{0}),
        HaveMigratedToDeviceSinceLastWrite(Context->NumDevices, false),
        HostPtr{HostPtr}, Size{Size}, MapSize{0}, MapOffset{0}, MapPtr{nullptr},
        MapFlags{UR_MAP_FLAG_WRITE}, MemAllocMode{Mode} {
    if (isSubBuffer()) {
      urMemRetain(Parent);
    }
  }

  ~ur_buffer_() override {
    if (isSubBuffer()) {
      urMemRelease(Parent);
    }
    if (LastEventWritingToMemObj != nullptr) {
      urEventRelease(LastEventWritingToMemObj);
    }
  }

  bool isBuffer() const noexcept override { return true; }
  bool isImage() const noexcept override { return false; }
  bool isSubBuffer() const noexcept { return Parent != nullptr; }

  std::vector<native_type> &getPtrs() noexcept { return Ptrs; }

  size_t getSize() const noexcept { return Size; }

  void *getMapPtr() const noexcept { return MapPtr; }

  size_t getMapSize() const noexcept { return MapSize; }

  size_t getMapOffset() const noexcept { return MapOffset; }

  /// Returns a pointer to data visible on the host that contains
  /// the data on the device associated with this allocation.
  /// The offset is used to index into the CUDA allocation.
  void *mapToPtr(size_t Size, size_t Offset, ur_map_flags_t Flags) noexcept {
    assert(MapPtr == nullptr);
    MapSize = Size;
    MapOffset = Offset;
    MapFlags = Flags;
    if (HostPtr) {
      MapPtr = static_cast<char *>(HostPtr) + Offset;
    } else {
      // TODO: Allocate only what is needed based on the offset
      MapPtr = static_cast<void *>(malloc(this->getSize()));
    }
    return MapPtr;
  }

  /// Detach the allocation from the host memory.
  void unmap(void *) noexcept {
    assert(MapPtr != nullptr);

    if (MapPtr != HostPtr) {
      free(MapPtr);
    }
    MapSize = 0;
    MapPtr = nullptr;
    MapOffset = 0;
  }

  ur_map_flags_t getMapFlags() const noexcept {
    assert(MapPtr != nullptr);
    return MapFlags;
  }

  native_type &getNativePtr(ur_device_handle_t hDevice) noexcept {
    assert(hDevice != nullptr);
    return Ptrs[hDevice->getIndex()];
  }

  ur_result_t clear() override {
    if (isSubBuffer()) {
      return UR_RESULT_SUCCESS;
    }
    ur_result_t Result = UR_RESULT_SUCCESS;

    switch (MemAllocMode) {
    case ur_buffer_::AllocMode::CopyIn:
    case ur_buffer_::AllocMode::Classic:
      for (auto i = 0u; i < getContext()->NumDevices; ++i) {
        if (getPtrs()[i] != ur_buffer_::native_type{0}) {
          ScopedDevice Active(getContext()->getDevices()[i]);
          Result = UR_CHECK_ERROR(cuMemFree(Ptrs[i]));
        }
      }
      break;
    case ur_buffer_::AllocMode::UseHostPtr:
      Result = UR_CHECK_ERROR(cuMemHostUnregister(HostPtr));
      break;
    case ur_buffer_::AllocMode::AllocHostPtr:
      Result = UR_CHECK_ERROR(cuMemFreeHost(HostPtr));
    };
    return Result;
  };

  ur_result_t allocateMemObjOnDeviceIfNeeded(ur_device_handle_t) override;
  ur_result_t migrateMemoryToDeviceIfNeeded(ur_device_handle_t) override;

  struct {
    size_t Origin; // only valid if Parent != nullptr
  } SubBuffer;

  void setLastEventWritingToMemObj(ur_event_handle_t NewEvent) {
    assert(NewEvent && "Invalid event!");
    if (LastEventWritingToMemObj != nullptr) {
      urEventRelease(LastEventWritingToMemObj);
    }
    urEventRetain(NewEvent);
    LastEventWritingToMemObj = NewEvent;
    for (auto i = 0u; i < Context->NumDevices; ++i) {
      if (i == NewEvent->getDevice()->getIndex()) {
        HaveMigratedToDeviceSinceLastWrite[i] = true;
      } else {
        HaveMigratedToDeviceSinceLastWrite[i] = false;
      }
    }
  }
};

// Handler data for image object (i.e. surface/textures)
struct ur_image_ final : ur_mem_handle_t_ {

  /// Constructs the UR allocation for an Image object (surface in CUDA)
  ur_image_(ur_context_handle_t Context, CUarray Array, CUsurfObject Surf,
            ur_mem_flags_t MemFlags, ur_mem_type_t ImageType, void *HostPtr)
      : ur_mem_handle_t_{Context, MemFlags}, MemType{Type::Surface} {

    (void)HostPtr;
    Mem.SurfaceMem.Array = Array;
    Mem.SurfaceMem.SurfObj = Surf;
    Mem.SurfaceMem.ImageType = ImageType;
  }

  /// Constructs the UR allocation for an unsampled image object
  ur_image_(ur_context_handle_t Context, CUarray Array, CUsurfObject Surf,
            ur_mem_type_t ImageType)
      : ur_mem_handle_t_{Context, 0}, MemType{Type::Surface} {
    Mem.ImageMem.Array = Array;
    Mem.ImageMem.Handle = (void *)Surf;
    Mem.ImageMem.ImageType = ImageType;
    Mem.ImageMem.Sampler = nullptr;
  }

  /// Constructs the UR allocation for a sampled image object
  ur_image_(ur_context_handle_t Context, CUarray Array, CUtexObject Tex,
            ur_sampler_handle_t Sampler, ur_mem_type_t ImageType)
      : ur_mem_handle_t_{Context, 0}, MemType{Type::Texture} {
    Mem.ImageMem.Array = Array;
    Mem.ImageMem.Handle = (void *)Tex;
    Mem.ImageMem.ImageType = ImageType;
    Mem.ImageMem.Sampler = Sampler;
  }

  ~ur_image_() override{};

  enum class Type { Surface, Texture } MemType;

  union MemImpl {
    // Handler data for surface object (i.e. Images)
    struct SurfaceMem {
      CUarray Array;
      CUsurfObject SurfObj;
      ur_mem_type_t ImageType;

      CUarray getArray() const noexcept { return Array; }

      CUsurfObject getSurface() const noexcept { return SurfObj; }

      ur_mem_type_t getImageType() const noexcept { return ImageType; }
    } SurfaceMem;

    struct ImageMem {
      CUarray Array;
      void *Handle;
      ur_mem_type_t ImageType;
      ur_sampler_handle_t Sampler;

      CUarray getArray() const noexcept { return Array; }

      void *getHandle() const noexcept { return Handle; }

      ur_mem_type_t getImageType() const noexcept { return ImageType; }

      ur_sampler_handle_t getSampler() const noexcept { return Sampler; }
    } ImageMem;
  } Mem;

  bool isBuffer() const noexcept override { return false; }
  bool isImage() const noexcept override { return true; }

  ur_result_t clear() override {
    // Images are allocated on the first device in a context
    ur_result_t Result = UR_RESULT_SUCCESS;
    ScopedDevice Active(getContext()->getDevices()[0]);
    if (Mem.SurfaceMem.getSurface() != CUsurfObject{0}) {
      Result = UR_CHECK_ERROR(cuSurfObjectDestroy(Mem.SurfaceMem.getSurface()));
    }
    if (Mem.SurfaceMem.getArray() != CUarray{0}) {
      Result = UR_CHECK_ERROR(cuArrayDestroy(Mem.SurfaceMem.getArray()));
    }
    return Result;
  };

  ur_result_t allocateMemObjOnDeviceIfNeeded(ur_device_handle_t) override;
  ur_result_t migrateMemoryToDeviceIfNeeded(ur_device_handle_t) override;
};
