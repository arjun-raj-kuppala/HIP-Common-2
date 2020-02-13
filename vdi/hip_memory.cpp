/* Copyright (c) 2015-present Advanced Micro Devices, Inc.

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE. */

#include <hip/hip_runtime.h>
#include "hip_internal.hpp"
#include "platform/context.hpp"
#include "platform/command.hpp"
#include "platform/memory.hpp"

extern void getChannelOrderAndType(const hipChannelFormatDesc& desc,
                                   enum hipTextureReadMode readMode,
                                   cl_channel_order* channelOrder,
                                   cl_channel_type* channelType);

extern void getDrvChannelOrderAndType(const enum hipArray_Format Format,
                                      unsigned int NumChannels,
                                      cl_channel_order* channelOrder,
                                      cl_channel_type* channelType);

extern void setDescFromChannelType(cl_channel_type channelType, hipChannelFormatDesc* desc);

extern void getByteSizeFromChannelFormatKind(enum hipChannelFormatKind channelFormatKind, size_t* byteSize);

amd::Memory* getMemoryObject(const void* ptr, size_t& offset) {
  amd::Memory *memObj = amd::MemObjMap::FindMemObj(ptr);
  if (memObj != nullptr) {
    if (memObj->getSvmPtr() != nullptr) {
      // SVM pointer
      offset = reinterpret_cast<size_t>(ptr) - reinterpret_cast<size_t>(memObj->getSvmPtr());
    } else if (memObj->getHostMem() != nullptr) {
      // Prepinned memory
      offset = reinterpret_cast<size_t>(ptr) - reinterpret_cast<size_t>(memObj->getHostMem());
    } else {
      ShouldNotReachHere();
    }
  }
  return memObj;
}

hipError_t ihipMalloc(void** ptr, size_t sizeBytes, unsigned int flags)
{
  if (sizeBytes == 0) {
    *ptr = nullptr;
    return hipSuccess;
  }
  else if (ptr == nullptr) {
    return hipErrorInvalidValue;
  }

  amd::Context* amdContext = ((flags & CL_MEM_SVM_FINE_GRAIN_BUFFER) != 0)?
    hip::host_context : hip::getCurrentContext();

  if (amdContext == nullptr) {
    return hipErrorOutOfMemory;
  }

  if (amdContext->devices()[0]->info().maxMemAllocSize_ < sizeBytes) {
    return hipErrorOutOfMemory;
  }

  *ptr = amd::SvmBuffer::malloc(*amdContext, flags, sizeBytes, amdContext->devices()[0]->info().memBaseAddrAlign_);
  if (*ptr == nullptr) {

    hip::syncStreams();
    hip::getNullStream()->finish();

    *ptr = amd::SvmBuffer::malloc(*amdContext, flags, sizeBytes, amdContext->devices()[0]->info().memBaseAddrAlign_);
    if (*ptr == nullptr) {
      return hipErrorOutOfMemory;
    }
  }
  ClPrint(amd::LOG_INFO, amd::LOG_API, "ihipMalloc ptr=0x%zx", *ptr);
  return hipSuccess;
}

hipError_t ihipMemcpy(void* dst, const void* src, size_t sizeBytes, hipMemcpyKind kind,
                      amd::HostQueue& queue, bool isAsync = false) {
  if (sizeBytes == 0) {
    // Skip if nothing needs writing.
    return hipSuccess;
  }

  amd::Command* command = nullptr;
  amd::Command::EventWaitList waitList;

  size_t sOffset = 0;
  amd::Memory *srcMemory = getMemoryObject(src, sOffset);
  size_t dOffset = 0;
  amd::Memory *dstMemory = getMemoryObject(dst, dOffset);
  amd::Device* queueDevice = &queue.device();

  if (((srcMemory == nullptr) && (dstMemory == nullptr)) ||
      (kind == hipMemcpyHostToHost)) {
    queue.finish();
    memcpy(dst, src, sizeBytes);
    return hipSuccess;
  } else if ((srcMemory == nullptr) && (dstMemory != nullptr)) {
    amd::HostQueue* pQueue = &queue;
    if (queueDevice != dstMemory->getContext().devices()[0]) {
      pQueue = hip::getNullStream(dstMemory->getContext());
    }
    command = new amd::WriteMemoryCommand(*pQueue, CL_COMMAND_WRITE_BUFFER, waitList,
              *dstMemory->asBuffer(), dOffset, sizeBytes, src);
    isAsync = false;
  } else if ((srcMemory != nullptr) && (dstMemory == nullptr)) {
    amd::HostQueue* pQueue = &queue;
    if (queueDevice != srcMemory->getContext().devices()[0]) {
      pQueue = hip::getNullStream(srcMemory->getContext());
    }
    command = new amd::ReadMemoryCommand(*pQueue, CL_COMMAND_READ_BUFFER, waitList,
              *srcMemory->asBuffer(), sOffset, sizeBytes, dst);
    isAsync = false;
  } else if ((srcMemory != nullptr) && (dstMemory != nullptr)) {
    if (queueDevice != srcMemory->getContext().devices()[0]) {
      amd::Coord3D srcOffset(sOffset, 0, 0);
      amd::Coord3D dstOffset(dOffset, 0, 0);
      amd::Coord3D copySize(sizeBytes, 1, 1);
      command = new amd::CopyMemoryP2PCommand(queue, CL_COMMAND_COPY_BUFFER, waitList,
                *srcMemory->asBuffer(),*dstMemory->asBuffer(), srcOffset, dstOffset, copySize);
      command->enqueue();
      if (!isAsync) {
        command->awaitCompletion();
      }
      command->release();
      return hipSuccess;
    }
    if (queueDevice != dstMemory->getContext().devices()[0]) {
      amd::Coord3D srcOffset(sOffset, 0, 0);
      amd::Coord3D dstOffset(dOffset, 0, 0);
      amd::Coord3D copySize(sizeBytes, 1, 1);
      command = new amd::CopyMemoryP2PCommand(queue, CL_COMMAND_COPY_BUFFER, waitList,
                *srcMemory->asBuffer(),*dstMemory->asBuffer(), srcOffset, dstOffset, copySize);
      command->enqueue();
      if (!isAsync) {
        command->awaitCompletion();
      }
      command->release();
      return hipSuccess;
    }
    command = new amd::CopyMemoryCommand(queue, CL_COMMAND_COPY_BUFFER, waitList,
              *srcMemory->asBuffer(),*dstMemory->asBuffer(), sOffset, dOffset, sizeBytes);
  }

  if (command == nullptr) {
    return hipErrorOutOfMemory;
  }

  command->enqueue();
  if (!isAsync) {
    command->awaitCompletion();
  }
  command->release();

  return hipSuccess;
}

hipError_t hipExtMallocWithFlags(void** ptr, size_t sizeBytes, unsigned int flags) {
  HIP_INIT_API(hipExtMallocWithFlags, ptr, sizeBytes, flags);

  if (flags != hipDeviceMallocDefault &&
      flags != hipDeviceMallocFinegrained) {
    HIP_RETURN(hipErrorInvalidValue);
  }

  HIP_RETURN(ihipMalloc(ptr, sizeBytes, (flags & hipDeviceMallocFinegrained)? CL_MEM_SVM_ATOMICS: 0));
}

hipError_t hipMalloc(void** ptr, size_t sizeBytes) {
  HIP_INIT_API(hipMalloc, ptr, sizeBytes);

  HIP_RETURN(ihipMalloc(ptr, sizeBytes, 0));
}

hipError_t hipHostMalloc(void** ptr, size_t sizeBytes, unsigned int flags) {
  HIP_INIT_API(hipHostMalloc, ptr, sizeBytes, flags);

  if (ptr == nullptr) {
    HIP_RETURN(hipErrorInvalidValue);
  }
  *ptr = nullptr;

  const unsigned int coherentFlags = hipHostMallocCoherent | hipHostMallocNonCoherent;

  // can't have both Coherent and NonCoherent flags set at the same time
  if ((flags & coherentFlags) == coherentFlags) {
    HIP_RETURN(hipErrorInvalidValue);
  }

  unsigned int ihipFlags = CL_MEM_SVM_FINE_GRAIN_BUFFER | (flags << 16);
  if (flags & (hipHostMallocCoherent | hipHostMallocMapped) ||
     (!(flags & hipHostMallocNonCoherent) && HIP_HOST_COHERENT)) {
    ihipFlags |= CL_MEM_SVM_ATOMICS;
  }

  HIP_RETURN(ihipMalloc(ptr, sizeBytes, ihipFlags));
}

hipError_t hipMallocManaged(void** devPtr, size_t size,
                            unsigned int flags) {
  HIP_INIT_API(hipMallocManaged, devPtr, size, flags);

  if (flags != hipMemAttachGlobal) {
    HIP_RETURN(hipErrorInvalidValue);
  }

  HIP_RETURN(ihipMalloc(devPtr, size, CL_MEM_SVM_FINE_GRAIN_BUFFER));
}

hipError_t hipFree(void* ptr) {
  HIP_INIT_API(hipFree, ptr);

  if (ptr == nullptr) {
    HIP_RETURN(hipSuccess);
  }
  if (amd::SvmBuffer::malloced(ptr)) {
    hip::syncStreams();
    for (size_t i=0; i<g_devices.size(); ++i) {
      hip::getNullStream(*g_devices[i])->finish();
    }
    amd::SvmBuffer::free(*hip::getCurrentContext(), ptr);
    HIP_RETURN(hipSuccess);
  }
  HIP_RETURN(hipErrorInvalidValue);
}

hipError_t hipMemcpy(void* dst, const void* src, size_t sizeBytes, hipMemcpyKind kind) {
  HIP_INIT_API(hipMemcpy, dst, src, sizeBytes, kind);

  hip::syncStreams();
  amd::HostQueue* queue = hip::getNullStream();
  HIP_RETURN(ihipMemcpy(dst, src, sizeBytes, kind, *queue));
}

hipError_t hipMemcpyWithStream(void* dst, const void* src, size_t sizeBytes,
                               hipMemcpyKind kind, hipStream_t stream) {
  HIP_INIT_API(hipMemcpyWithStream, dst, src, sizeBytes, kind, stream);

  amd::HostQueue* queue = hip::getQueue(stream);

  HIP_RETURN(ihipMemcpy(dst, src, sizeBytes, kind, *queue, true));
}

hipError_t hipMemPtrGetInfo(void *ptr, size_t *size) {
  HIP_INIT_API(hipMemPtrGetInfo, ptr, size);

  size_t offset = 0;
  amd::Memory* svmMem = getMemoryObject(ptr, offset);

  if (svmMem == nullptr) {
    HIP_RETURN(hipErrorInvalidValue);
  }

  *size = svmMem->getSize();

  HIP_RETURN(hipSuccess);
}

hipError_t hipHostFree(void* ptr) {
  HIP_INIT_API(hipHostFree, ptr);

  if (amd::SvmBuffer::malloced(ptr)) {
    amd::SvmBuffer::free(*hip::getCurrentContext(), ptr);
    HIP_RETURN(hipSuccess);
  }
  HIP_RETURN(hipErrorInvalidValue);
}

hipError_t hipFreeArray(hipArray* array) {
  HIP_INIT_API(hipFreeArray, array);

  if (amd::SvmBuffer::malloced(array->data)) {
    amd::SvmBuffer::free(*hip::getCurrentContext(), array->data);
    HIP_RETURN(hipSuccess);
  }
  HIP_RETURN(hipErrorInvalidValue);
}

hipError_t hipMemGetAddressRange(hipDeviceptr_t* pbase, size_t* psize, hipDeviceptr_t dptr) {
  HIP_INIT_API(hipMemGetAddressRange, pbase, psize, dptr);

  // Since we are using SVM buffer DevicePtr and HostPtr is the same
  void* ptr = dptr;
  size_t offset = 0;
  amd::Memory* svmMem = getMemoryObject(ptr, offset);

  if (svmMem == nullptr) {
    HIP_RETURN(hipErrorInvalidDevicePointer);
  }

  *pbase = svmMem->getSvmPtr();
  *psize = svmMem->getSize();

  HIP_RETURN(hipSuccess);
}

hipError_t hipMemGetInfo(size_t* free, size_t* total) {
  HIP_INIT_API(hipMemGetInfo, free, total);

  size_t freeMemory[2];
  amd::Device* device = hip::getCurrentContext()->devices()[0];
  if(device == nullptr) {
    HIP_RETURN(hipErrorInvalidDevice);
  }

  if(!device->globalFreeMemory(freeMemory)) {
    HIP_RETURN(hipErrorInvalidValue);
  }

  *free = freeMemory[0] * Ki;
  *total = device->info().globalMemSize_;

  HIP_RETURN(hipSuccess);
}

hipError_t ihipMallocPitch(void** ptr, size_t* pitch, size_t width, size_t height, size_t depth,
                           cl_mem_object_type imageType, const cl_image_format* image_format) {

  amd::Device* device = hip::getCurrentContext()->devices()[0];

  if ((width == 0) || (height == 0)) {
    *ptr = nullptr;
    return hipSuccess;
  }
  else if (!(device->info().image2DMaxWidth_ >= width &&
           device->info().image2DMaxHeight_ >= height ) || (ptr == nullptr)) {
    return hipErrorInvalidValue;
  }

  if (device->info().maxMemAllocSize_ < (width * height)) {
    return hipErrorOutOfMemory;
  }

  const amd::Image::Format imageFormat(*image_format);

  *pitch = amd::alignUp(width * imageFormat.getElementSize(), device->info().imagePitchAlignment_);

  size_t sizeBytes = *pitch * height * depth;
  *ptr = amd::SvmBuffer::malloc(*hip::getCurrentContext(), 0, sizeBytes,
                                device->info().memBaseAddrAlign_);

  if (*ptr == nullptr) {
    return hipErrorOutOfMemory;
  }

  return hipSuccess;
}


hipError_t hipMallocPitch(void** ptr, size_t* pitch, size_t width, size_t height) {
  HIP_INIT_API(hipMallocPitch, ptr, pitch, width, height);

  const cl_image_format image_format = { CL_R, CL_UNSIGNED_INT8 };
  HIP_RETURN(ihipMallocPitch(ptr, pitch, width, height, 1, CL_MEM_OBJECT_IMAGE2D, &image_format));
}

hipError_t hipMalloc3D(hipPitchedPtr* pitchedDevPtr, hipExtent extent) {
  HIP_INIT_API(hipMalloc3D, pitchedDevPtr, &extent);

  size_t pitch = 0;

  if (pitchedDevPtr == nullptr) {
    HIP_RETURN(hipErrorInvalidValue);
  }

  const cl_image_format image_format = { CL_R, CL_UNSIGNED_INT8 };
  hipError_t status = hipSuccess;
  status = ihipMallocPitch(&pitchedDevPtr->ptr, &pitch, extent.width, extent.height, extent.depth,
                           CL_MEM_OBJECT_IMAGE3D, &image_format);

  if (status == hipSuccess) {
        pitchedDevPtr->pitch = pitch;
        pitchedDevPtr->xsize = extent.width;
        pitchedDevPtr->ysize = extent.height;
  }

  HIP_RETURN(status);
}

hipError_t hipArrayCreate(hipArray** array, const HIP_ARRAY_DESCRIPTOR* pAllocateArray) {
  HIP_INIT_API(hipArrayCreate, array, pAllocateArray);

  if (pAllocateArray->Width == 0) {
    HIP_RETURN(hipErrorInvalidValue);
  }

  *array = (hipArray*)malloc(sizeof(hipArray));
  array[0]->width = pAllocateArray->Width;
  array[0]->height = pAllocateArray->Height;
  array[0]->isDrv = true;
  array[0]->textureType = hipTextureType2D;
  void** ptr = &array[0]->data;

  cl_channel_order channelOrder;
  cl_channel_type channelType;
  getDrvChannelOrderAndType(pAllocateArray->Format, pAllocateArray->NumChannels,
                            &channelOrder, &channelType);

  const cl_image_format image_format = { channelOrder, channelType };
  setDescFromChannelType(channelType, &(array[0]->desc));

  size_t pitch = 0;
  hipError_t status = ihipMallocPitch(ptr, &pitch, array[0]->width, array[0]->height, 1, CL_MEM_OBJECT_IMAGE2D,
                      &image_format);

  HIP_RETURN(status);
}

hipError_t hipMallocArray(hipArray** array, const hipChannelFormatDesc* desc,
                          size_t width, size_t height, unsigned int flags) {
  HIP_INIT_API(hipMallocArray, array, desc, width, height, flags);

  if (width == 0) {
    HIP_RETURN(hipErrorInvalidValue);
  }

  *array = (hipArray*)malloc(sizeof(hipArray));
  array[0]->type = flags;
  array[0]->width = width;
  array[0]->height = height;
  array[0]->depth = 1;
  array[0]->desc = *desc;
  array[0]->isDrv = false;
  array[0]->textureType = hipTextureType2D;
  void** ptr = &array[0]->data;

  cl_channel_order channelOrder;
  cl_channel_type channelType;
  getChannelOrderAndType(*desc, hipReadModeElementType, &channelOrder, &channelType);

  const cl_image_format image_format = { channelOrder, channelType };

 // Dummy flags check
  switch (flags) {
    case hipArrayLayered:
    case hipArrayCubemap:
    case hipArraySurfaceLoadStore:
    case hipArrayTextureGather:
        assert(0 && "Unspported");
        break;
    case hipArrayDefault:
    default:
        break;
  }
  size_t pitch = 0;
  hipError_t status = ihipMallocPitch(ptr, &pitch, width, height, 1, CL_MEM_OBJECT_IMAGE2D,
                      &image_format);

  HIP_RETURN(status);
}

hipError_t hipArray3DCreate(hipArray** array, const HIP_ARRAY3D_DESCRIPTOR* pAllocateArray) {
  HIP_INIT_API(hipArray3DCreate, array, pAllocateArray);

  *array = (hipArray*)malloc(sizeof(hipArray));
  array[0]->type = pAllocateArray->Flags;
  array[0]->width = pAllocateArray->Width;
  array[0]->height = pAllocateArray->Height;
  array[0]->depth = pAllocateArray->Depth;
  array[0]->Format = pAllocateArray->Format;
  array[0]->NumChannels = pAllocateArray->NumChannels;
  array[0]->isDrv = true;
  array[0]->textureType = hipTextureType3D;
  void** ptr = &array[0]->data;

  cl_channel_order channelOrder;
  cl_channel_type channelType;
  getDrvChannelOrderAndType(pAllocateArray->Format, pAllocateArray->NumChannels,
                            &channelOrder, &channelType);

  const cl_image_format image_format = { channelOrder, channelType };
  size_t pitch = 0;
  hipError_t status = ihipMallocPitch(ptr, &pitch, array[0]->width, array[0]->height, array[0]->depth, CL_MEM_OBJECT_IMAGE3D,
                      &image_format);

  HIP_RETURN(status);
}

hipError_t hipMalloc3DArray(hipArray_t* array, const struct hipChannelFormatDesc* desc,
                            struct hipExtent extent, unsigned int flags) {
  HIP_INIT_API(hipMalloc3DArray, array, desc, &extent, flags);

  *array = (hipArray*)malloc(sizeof(hipArray));
  array[0]->type = flags;
  array[0]->width = extent.width;
  array[0]->height = extent.height;
  array[0]->depth = extent.depth;
  array[0]->desc = *desc;
  array[0]->isDrv = false;
  array[0]->textureType = hipTextureType3D;
  void** ptr = &array[0]->data;

  cl_channel_order channelOrder;
  cl_channel_type channelType;
  getChannelOrderAndType(*desc, hipReadModeElementType, &channelOrder, &channelType);

  const cl_image_format image_format = { channelOrder, channelType };

 // Dummy flags check
   switch (flags) {
    case hipArrayCubemap:
    case hipArraySurfaceLoadStore:
    case hipArrayTextureGather:
        assert(0 && "Unspported");
        break;
    case hipArrayLayered:
    case hipArrayDefault:
    default:
        break;
  }
  size_t pitch = 0;
  hipError_t status = ihipMallocPitch(ptr, &pitch, extent.width, extent.height, extent.depth,
                      CL_MEM_OBJECT_IMAGE3D, &image_format);

  HIP_RETURN(status);
}

hipError_t hipHostGetFlags(unsigned int* flagsPtr, void* hostPtr) {
  HIP_INIT_API(hipHostGetFlags, flagsPtr, hostPtr);

  if (flagsPtr == nullptr ||
      hostPtr == nullptr) {
    HIP_RETURN(hipErrorInvalidValue);
  }

  size_t offset = 0;
  amd::Memory* svmMem = getMemoryObject(hostPtr, offset);

  if (svmMem == nullptr) {
    HIP_RETURN(hipErrorInvalidValue);
  }

  *flagsPtr = svmMem->getMemFlags() >> 16;

  HIP_RETURN(hipSuccess);
}

hipError_t hipHostRegister(void* hostPtr, size_t sizeBytes, unsigned int flags) {
  HIP_INIT_API(hipHostRegister, hostPtr, sizeBytes, flags);
  if(hostPtr != nullptr) {
    amd::Memory* mem = new (*hip::host_context) amd::Buffer(*hip::host_context, CL_MEM_USE_HOST_PTR | CL_MEM_SVM_ATOMICS, sizeBytes);

    constexpr bool sysMemAlloc = false;
    constexpr bool skipAlloc = false;
    constexpr bool forceAlloc = true;
    if (!mem->create(hostPtr, sysMemAlloc, skipAlloc, forceAlloc)) {
      mem->release();
      HIP_RETURN(hipErrorOutOfMemory);
    }

    for (const auto& device: hip::getCurrentContext()->devices()) {
      // Since the amd::Memory object is shared between all devices
      // it's fine to have multiple addresses mapped to it
      const device::Memory* devMem = mem->getDeviceMemory(*device);
      amd::MemObjMap::AddMemObj(reinterpret_cast<void*>(devMem->virtualAddress()), mem);
    }

    amd::MemObjMap::AddMemObj(hostPtr, mem);
    HIP_RETURN(hipSuccess);
  } else {
    HIP_RETURN(ihipMalloc(&hostPtr, sizeBytes, flags));
  }
}

hipError_t hipHostUnregister(void* hostPtr) {
  HIP_INIT_API(hipHostUnregister, hostPtr);

  if (amd::SvmBuffer::malloced(hostPtr)) {
    hip::syncStreams();
    hip::getNullStream()->finish();
    amd::SvmBuffer::free(*hip::host_context, hostPtr);
    HIP_RETURN(hipSuccess);
  } else {
    size_t offset = 0;
    amd::Memory* mem = getMemoryObject(hostPtr, offset);

    if(mem) {
      hip::syncStreams();
      hip::getNullStream()->finish();
      for (const auto& device: hip::getCurrentContext()->devices()) {
        const device::Memory* devMem = mem->getDeviceMemory(*device);
        amd::MemObjMap::RemoveMemObj(reinterpret_cast<void*>(devMem->virtualAddress()));
      }
      amd::MemObjMap::RemoveMemObj(hostPtr);
      mem->release();
      HIP_RETURN(hipSuccess);
    }
  }

  HIP_RETURN(hipErrorInvalidValue);
}

// Deprecated function:
hipError_t hipHostAlloc(void** ptr, size_t sizeBytes, unsigned int flags) {
  HIP_RETURN(ihipMalloc(ptr, sizeBytes, flags));
};


hipError_t hipMemcpyToSymbol(const void* symbolName, const void* src, size_t count,
                             size_t offset, hipMemcpyKind kind) {
  HIP_INIT_API(hipMemcpyToSymbol, symbolName, src, count, offset, kind);

  size_t sym_size = 0;
  hipDeviceptr_t device_ptr = nullptr;

  /* Get address and size for the global symbol */
  if (!PlatformState::instance().getGlobalVar(symbolName, ihipGetDevice(), nullptr,
                                              &device_ptr, &sym_size)) {
    HIP_RETURN(hipErrorInvalidSymbol);
  }

  /* Size Check to make sure offset is correct */
  if ((offset + count) != sym_size) {
    return HIP_RETURN(hipErrorInvalidDevicePointer);
  }

  device_ptr = reinterpret_cast<address>(device_ptr) + offset;

  /* Copy memory from source to destination address */
  HIP_RETURN(hipMemcpy(device_ptr, src, count, kind));
}

hipError_t hipMemcpyFromSymbol(void* dst, const void* symbolName, size_t count,
                               size_t offset, hipMemcpyKind kind) {
  HIP_INIT_API(hipMemcpyFromSymbol, symbolName, dst, count, offset, kind);

  size_t sym_size = 0;
  hipDeviceptr_t device_ptr = nullptr;

  /* Get address and size for the global symbol */
  if (!PlatformState::instance().getGlobalVar(symbolName, ihipGetDevice(), nullptr,
                                              &device_ptr, &sym_size)) {
    HIP_RETURN(hipErrorInvalidSymbol);
  }

  /* Size Check to make sure offset is correct */
  if ((offset + count) != sym_size) {
    return HIP_RETURN(hipErrorInvalidDevicePointer);
  }

  device_ptr = reinterpret_cast<address>(device_ptr) + offset;

  /* Copy memory from source to destination address */
  HIP_RETURN(hipMemcpy(dst, device_ptr, count, kind));
}

hipError_t hipMemcpyToSymbolAsync(const void* symbolName, const void* src, size_t count,
                                  size_t offset, hipMemcpyKind kind, hipStream_t stream) {
  HIP_INIT_API(hipMemcpyToSymbolAsync, symbolName, src, count, offset, kind, stream);

  size_t sym_size = 0;
  hipDeviceptr_t device_ptr = nullptr;

  /* Get address and size for the global symbol */
  if (!PlatformState::instance().getGlobalVar(symbolName, ihipGetDevice(), nullptr,
                                              &device_ptr, &sym_size)) {
    HIP_RETURN(hipErrorInvalidSymbol);
  }

  /* Size Check to make sure offset is correct */
  if ((offset + count) != sym_size) {
    return HIP_RETURN(hipErrorInvalidDevicePointer);
  }

  device_ptr = reinterpret_cast<address>(device_ptr) + offset;

  /* Copy memory from source to destination address */
  HIP_RETURN(hipMemcpyAsync(device_ptr, src, count, kind, stream));
}

hipError_t hipMemcpyFromSymbolAsync(void* dst, const void* symbolName, size_t count,
                                    size_t offset, hipMemcpyKind kind, hipStream_t stream) {
  HIP_INIT_API(hipMemcpyFromSymbolAsync, symbolName, dst, count, offset, kind, stream);

  size_t sym_size = 0;
  hipDeviceptr_t device_ptr = nullptr;

  /* Get address and size for the global symbol */
  if (!PlatformState::instance().getGlobalVar(symbolName, ihipGetDevice(), nullptr,
                                              &device_ptr, &sym_size)) {
    HIP_RETURN(hipErrorInvalidSymbol);
  }

  /* Size Check to make sure offset is correct */
  if ((offset + count) != sym_size) {
    return HIP_RETURN(hipErrorInvalidDevicePointer);
  }

  device_ptr = reinterpret_cast<address>(device_ptr) + offset;

  /* Copy memory from source to destination address */
  HIP_RETURN(hipMemcpyAsync(dst, device_ptr, count, kind, stream));
}

hipError_t hipMemcpyHtoD(hipDeviceptr_t dst, void* src, size_t sizeBytes) {
  HIP_INIT_API(hipMemcpyHtoD, dst, src, sizeBytes);

  hip::syncStreams();
  amd::HostQueue* queue = hip::getNullStream();

  HIP_RETURN(ihipMemcpy(reinterpret_cast<void*>(dst), (const void*) src, sizeBytes, hipMemcpyHostToDevice, *queue));
}

hipError_t hipMemcpyDtoH(void* dst, hipDeviceptr_t src, size_t sizeBytes) {
  HIP_INIT_API(hipMemcpyDtoH, dst, src, sizeBytes);

  hip::syncStreams();
  amd::HostQueue* queue = hip::getNullStream();

  HIP_RETURN(ihipMemcpy(reinterpret_cast<void*>(dst), (const void*) src, sizeBytes, hipMemcpyDeviceToHost, *queue));
}

hipError_t hipMemcpyDtoD(hipDeviceptr_t dst, hipDeviceptr_t src, size_t sizeBytes) {
  HIP_INIT_API(hipMemcpyDtoD, dst, src, sizeBytes);

  hip::syncStreams();
  amd::HostQueue* queue = hip::getNullStream();

  HIP_RETURN(ihipMemcpy(reinterpret_cast<void*>(dst), (const void*) src, sizeBytes, hipMemcpyDeviceToDevice, *queue));
}

hipError_t hipMemcpyHtoH(void* dst, void* src, size_t sizeBytes) {
  HIP_INIT_API(NONE, dst, src, sizeBytes);

  hip::syncStreams();
  amd::HostQueue* queue = hip::getNullStream();

  HIP_RETURN(ihipMemcpy(reinterpret_cast<void*>(dst), (const void*) src, sizeBytes, hipMemcpyHostToHost, *queue));
}

hipError_t hipMemcpyAsync(void* dst, const void* src, size_t sizeBytes,
                          hipMemcpyKind kind, hipStream_t stream) {
  HIP_INIT_API(hipMemcpyAsync, dst, src, sizeBytes, kind, stream);

  amd::HostQueue* queue = hip::getQueue(stream);

  HIP_RETURN(ihipMemcpy(dst, src, sizeBytes, kind, *queue, true));
}


hipError_t hipMemcpyHtoDAsync(hipDeviceptr_t dst, void* src, size_t sizeBytes,
                              hipStream_t stream) {
  HIP_INIT_API(hipMemcpyHtoDAsync, dst, src, sizeBytes, stream);

  amd::HostQueue* queue = hip::getQueue(stream);

  HIP_RETURN(ihipMemcpy(reinterpret_cast<void*>(dst), (const void*) src, sizeBytes, hipMemcpyHostToDevice,
                    *queue, true));
}

hipError_t hipMemcpyDtoDAsync(hipDeviceptr_t dst, hipDeviceptr_t src, size_t sizeBytes,
                              hipStream_t stream) {
  HIP_INIT_API(hipMemcpyDtoDAsync, dst, src, sizeBytes, stream);

  amd::HostQueue* queue = hip::getQueue(stream);

  HIP_RETURN(ihipMemcpy(reinterpret_cast<void*>(dst), (const void*) src, sizeBytes, hipMemcpyDeviceToDevice,
                   *queue, true));
}

hipError_t hipMemcpyDtoHAsync(void* dst, hipDeviceptr_t src, size_t sizeBytes,
                              hipStream_t stream) {
  HIP_INIT_API(hipMemcpyDtoHAsync, dst, src, sizeBytes, stream);

  amd::HostQueue* queue = hip::getQueue(stream);

  HIP_RETURN(ihipMemcpy(reinterpret_cast<void*>(dst), (const void*) src, sizeBytes, hipMemcpyDeviceToHost,
                   *queue, true));
}

hipError_t ihipMemcpy2D(void* dst, size_t dpitch, const void* src, size_t spitch, size_t width,
                        size_t height, hipMemcpyKind kind, amd::HostQueue& queue,
                        bool isAsync = false) {
  // Create buffer rectangle info structure
  amd::BufferRect srcRect;
  amd::BufferRect dstRect;

  size_t region[3] = {width, height, 1};
  size_t src_slice_pitch = spitch * height;
  size_t dst_slice_pitch = dpitch * height;
  size_t sOrigin[3] = { };
  size_t dOrigin[3] = { };
  amd::Memory* srcMemory = getMemoryObject(src, sOrigin[0]);
  amd::Memory* dstMemory = getMemoryObject(dst, dOrigin[0]);

  if (src_slice_pitch == 0 ||
      dst_slice_pitch == 0 ||
      dst == nullptr ||
      src == nullptr) {
    return hipSuccess;
  }

  if (!srcRect.create(sOrigin, region, spitch, src_slice_pitch) ||
      !dstRect.create(dOrigin, region, dpitch, dst_slice_pitch)) {
    return hipErrorInvalidValue;
  }

  amd::Command* command = nullptr;
  amd::Command::EventWaitList waitList;

  amd::Coord3D srcStart(srcRect.start_, 0, 0);
  amd::Coord3D dstStart(dstRect.start_, 0, 0);
  amd::Coord3D size(region[0], region[1], region[2]);

  if (((srcMemory == nullptr) && (dstMemory == nullptr)) ||
      (kind == hipMemcpyHostToHost)) {
    for(unsigned int y = 0; y < height; y++) {
      void* pDst = reinterpret_cast<void*>(reinterpret_cast<size_t>(dst) + y * dpitch);
      void* pSrc = reinterpret_cast<void*>(reinterpret_cast<size_t>(src) + y * spitch);
      memcpy(pDst, pSrc, width);
    }
    return hipSuccess;
  } else if ((srcMemory == nullptr) && (dstMemory != nullptr)) {
    command = new amd::WriteMemoryCommand(queue, CL_COMMAND_WRITE_BUFFER_RECT, waitList,
              *dstMemory->asBuffer(), dstStart, size, src, dstRect, srcRect);
  } else if ((srcMemory != nullptr) && (dstMemory == nullptr)) {
    command = new amd::ReadMemoryCommand(queue, CL_COMMAND_READ_BUFFER_RECT, waitList,
              *srcMemory->asBuffer(), srcStart, size, dst, srcRect, dstRect);
  } else if ((srcMemory != nullptr) && (dstMemory != nullptr)) {
    command = new amd::CopyMemoryCommand(queue, CL_COMMAND_COPY_BUFFER_RECT, waitList, *srcMemory->asBuffer(),
              *dstMemory->asBuffer(), srcStart, dstStart, size, srcRect, dstRect);
  }

  if (command == nullptr) {
    return hipErrorOutOfMemory;
  }

  command->enqueue();
  if (!isAsync) {
    command->awaitCompletion();
  }
  command->release();

  return hipSuccess;
}

hipError_t hipMemcpyParam2D(const hip_Memcpy2D* pCopy) {
  HIP_INIT_API(hipMemcpyParam2D, pCopy);
  hipError_t e = hipSuccess;
  if (pCopy == nullptr) {
    e = hipErrorInvalidValue;
  } else {
    hip::syncStreams();
    amd::HostQueue* queue = hip::getNullStream();
    e = ihipMemcpy2D(pCopy->dstArray->data, pCopy->WidthInBytes, pCopy->srcHost, pCopy->srcPitch,
                     pCopy->WidthInBytes, pCopy->Height, hipMemcpyDefault, *queue);
  }
  HIP_RETURN(e);
}

hipError_t hipMemcpy2D(void* dst, size_t dpitch, const void* src, size_t spitch, size_t width,
                       size_t height, hipMemcpyKind kind) {
  HIP_INIT_API(hipMemcpy2D, dst, dpitch, src, spitch, width, height, kind);

  hip::syncStreams();
  amd::HostQueue* queue = hip::getNullStream();

  HIP_RETURN(ihipMemcpy2D(dst, dpitch, src, spitch, width, height, kind, *queue));
}


hipError_t hipMemcpy2DAsync(void* dst, size_t dpitch, const void* src, size_t spitch, size_t width,
                            size_t height, hipMemcpyKind kind, hipStream_t stream) {
  HIP_INIT_API(hipMemcpy2DAsync, dst, dpitch, src, spitch, width, height, kind, stream);

  amd::HostQueue* queue = hip::getQueue(stream);

  HIP_RETURN(ihipMemcpy2D(dst, dpitch, src, spitch, width, height, kind, *queue, true));
}

hipError_t hipMemcpy2DToArray(hipArray* dst, size_t wOffset, size_t hOffset, const void* src,
                              size_t spitch, size_t width, size_t height, hipMemcpyKind kind) {
  HIP_INIT_API(hipMemcpy2DToArray, dst, wOffset, hOffset, src, spitch, width, height, kind);

  if (dst->data == nullptr) {
    HIP_RETURN(hipErrorInvalidValue);
  }

  hip::syncStreams();
  amd::HostQueue* queue = hip::getNullStream();

  size_t dpitch = dst->width;
  getByteSizeFromChannelFormatKind(dst[0].desc.f, &dpitch);

  if ((wOffset + width > (dpitch)) || width > spitch) {
    HIP_RETURN(hipErrorInvalidDevicePointer);
  }

  // Create buffer rectangle info structure
  amd::BufferRect srcRect;
  amd::BufferRect dstRect;

  size_t region[3] = {width, height, 1};
  size_t src_slice_pitch = spitch * height;
  size_t dst_slice_pitch = dpitch * height;
  size_t sOrigin[3] = { };
  size_t dOrigin[3] = {wOffset, hOffset, 0};
  size_t offset = 0;
  amd::Memory* srcMemory = getMemoryObject(src, offset);
  amd::Memory* dstMemory = getMemoryObject(dst->data, offset);

  assert(offset == 0);

  if (!srcRect.create(sOrigin, region, spitch, src_slice_pitch) ||
      !dstRect.create(dOrigin, region, dpitch, dst_slice_pitch)) {
    HIP_RETURN(hipErrorInvalidValue);
  }

  amd::Command* command = nullptr;
  amd::Command::EventWaitList waitList;

  amd::Coord3D srcStart(srcRect.start_, 0, 0);
  amd::Coord3D dstStart(dstRect.start_, 0, 0);
  amd::Coord3D size(region[0], region[1], region[2]);

  if (((srcMemory == nullptr) && (dstMemory == nullptr)) ||
      (kind == hipMemcpyHostToHost)) {
    void* newDst = reinterpret_cast<void*>(reinterpret_cast<size_t>(dst->data)
                                           + dpitch * hOffset + wOffset);
    for(unsigned int y = 0; y < height; y++) {
      void* pDst = reinterpret_cast<void*>(reinterpret_cast<size_t>(newDst) + y * dpitch);
      void* pSrc = reinterpret_cast<void*>(reinterpret_cast<size_t>(src) + y * spitch);
      memcpy(pDst, pSrc, width);
    }
    HIP_RETURN(hipSuccess);
  } else if ((srcMemory == nullptr) && (dstMemory != nullptr)) {
    command = new amd::WriteMemoryCommand(*queue, CL_COMMAND_WRITE_BUFFER_RECT, waitList,
              *dstMemory->asBuffer(), dstStart, size, src, dstRect, srcRect);
  } else if ((srcMemory != nullptr) && (dstMemory == nullptr)) {
    command = new amd::ReadMemoryCommand(*queue, CL_COMMAND_READ_BUFFER_RECT, waitList,
              *srcMemory->asBuffer(), srcStart, size, dst, srcRect, dstRect);
  } else if ((srcMemory != nullptr) && (dstMemory != nullptr)) {
    command = new amd::CopyMemoryCommand(*queue, CL_COMMAND_COPY_BUFFER_RECT, waitList, *srcMemory->asBuffer(),
              *dstMemory->asBuffer(), srcStart, dstStart, size, srcRect, dstRect);
  }

  if (command == nullptr) {
    HIP_RETURN(hipErrorOutOfMemory);
  }

  command->enqueue();
  command->awaitCompletion();
  command->release();

  HIP_RETURN(hipSuccess);

}

hipError_t hipMemcpyToArray(hipArray* dstArray, size_t wOffset, size_t hOffset, const void* src,
                            size_t count, hipMemcpyKind kind) {
  HIP_INIT_API(hipMemcpyToArray, dstArray, wOffset, hOffset, src, count, kind);

  hip::syncStreams();
  amd::HostQueue* queue = hip::getNullStream();

  amd::Command* command = nullptr;
  amd::Command::EventWaitList waitList;

  size_t sOffset = 0;
  amd::Memory* srcMemory = getMemoryObject(src, sOffset);
  size_t dOffset = 0;
  amd::Memory* dstMemory = getMemoryObject(dstArray->data, dOffset);

  assert(dOffset == 0);

  assert((kind == hipMemcpyHostToDevice) && "Invalid case");

  if ((srcMemory == nullptr) && (dstMemory != nullptr)) {
    command = new amd::WriteMemoryCommand(*queue, CL_COMMAND_WRITE_BUFFER, waitList,
              *dstMemory->asBuffer(), {wOffset, hOffset}, count, src);
  } else if ((srcMemory != nullptr) && (dstMemory != nullptr)) {
    command = new amd::CopyMemoryCommand(*queue, CL_COMMAND_COPY_BUFFER, waitList,
              *srcMemory->asBuffer(),*dstMemory->asBuffer(), sOffset, {wOffset, hOffset}, count);
  } else {
    ShouldNotReachHere();
  }

  if (command == nullptr) {
    HIP_RETURN(hipErrorOutOfMemory);
  }

  command->enqueue();
  command->awaitCompletion();
  command->release();

  HIP_RETURN(hipSuccess);
}

hipError_t hipMemcpyFromArray(void* dst, hipArray_const_t srcArray, size_t wOffset, size_t hOffset,
                              size_t count, hipMemcpyKind kind) {
  HIP_INIT_API(hipMemcpyFromArray, dst, srcArray, wOffset, hOffset, count, kind);

  hip::syncStreams();
  amd::HostQueue* queue = hip::getNullStream();

  amd::Command* command = nullptr;
  amd::Command::EventWaitList waitList;

  size_t sOffset = 0;
  amd::Memory* srcMemory = getMemoryObject(srcArray->data, sOffset);
  size_t dOffset = 0;
  amd::Memory* dstMemory = getMemoryObject(dst, dOffset);

  assert(sOffset == 0);

  assert((kind == hipMemcpyDeviceToHost) && "Invalid case");

  if ((srcMemory != nullptr) && (dstMemory == nullptr)) {
    command = new amd::ReadMemoryCommand(*queue, CL_COMMAND_READ_BUFFER, waitList,
              *srcMemory->asBuffer(), {wOffset, hOffset}, count, dst);
  } else if ((srcMemory != nullptr) && (dstMemory != nullptr)) {
    command = new amd::CopyMemoryCommand(*queue, CL_COMMAND_COPY_BUFFER, waitList,
              *srcMemory->asBuffer(), *dstMemory->asBuffer(), {wOffset, hOffset}, dOffset, count);
  } else {
    ShouldNotReachHere();
  }

  if (command == nullptr) {
    HIP_RETURN(hipErrorOutOfMemory);
  }

  command->enqueue();
  command->awaitCompletion();
  command->release();

  HIP_RETURN(hipSuccess);
}

hipError_t hipMemcpyHtoA(hipArray* dstArray, size_t dstOffset, const void* srcHost, size_t count) {
  HIP_INIT_API(hipMemcpyHtoA, dstArray, dstOffset, srcHost, count);

  hip::syncStreams();
  amd::HostQueue* queue = hip::getNullStream();

  amd::Command* command = nullptr;
  amd::Command::EventWaitList waitList;

  size_t sOffset = 0;
  amd::Memory* srcMemory = getMemoryObject(srcHost, sOffset);
  size_t dOffset = 0;
  amd::Memory* dstMemory = getMemoryObject(dstArray->data, dOffset);

  assert(dOffset == 0);

  if ((srcMemory == nullptr) && (dstMemory != nullptr)) {
    command = new amd::WriteMemoryCommand(*queue, CL_COMMAND_WRITE_BUFFER, waitList,
              *dstMemory->asBuffer(), dstOffset, count, srcHost);
  } else if ((srcMemory != nullptr) && (dstMemory != nullptr)) {
    command = new amd::CopyMemoryCommand(*queue, CL_COMMAND_COPY_BUFFER, waitList,
              *srcMemory->asBuffer(), *dstMemory->asBuffer(), sOffset, dstOffset, count);
  } else {
    ShouldNotReachHere();
  }

  if (command == nullptr) {
    HIP_RETURN(hipErrorOutOfMemory);
  }

  command->enqueue();
  command->awaitCompletion();
  command->release();

  HIP_RETURN(hipSuccess);
}

hipError_t hipMemcpyAtoH(void* dst, hipArray* srcArray, size_t srcOffset, size_t count) {
  HIP_INIT_API(hipMemcpyAtoH, dst, srcArray, srcOffset, count);

  hip::syncStreams();
  amd::HostQueue* queue = hip::getNullStream();

  amd::Command* command = nullptr;
  amd::Command::EventWaitList waitList;

  size_t sOffset = 0;
  amd::Memory* srcMemory = getMemoryObject(srcArray->data, sOffset);
  size_t dOffset = 0;
  amd::Memory* dstMemory = getMemoryObject(dst, dOffset);

  assert(sOffset == 0);

  if ((srcMemory != nullptr) && (dstMemory == nullptr)) {
    command = new amd::ReadMemoryCommand(*queue, CL_COMMAND_READ_BUFFER, waitList,
              *srcMemory->asBuffer(), srcOffset, count, dst);
  } else if ((srcMemory != nullptr) && (dstMemory != nullptr)) {
    command = new amd::CopyMemoryCommand(*queue, CL_COMMAND_COPY_BUFFER, waitList,
              *srcMemory->asBuffer(), *dstMemory->asBuffer(), srcOffset, dOffset, count);
  } else {
    ShouldNotReachHere();
  }

  if (command == nullptr) {
    HIP_RETURN(hipErrorOutOfMemory);
  }

  command->enqueue();
  command->awaitCompletion();
  command->release();

  HIP_RETURN(hipSuccess);
}

hipError_t ihipMemcpy3D_V1(const struct hipMemcpy3DParms* p, hipStream_t stream, bool isAsync = false) {
  const void* srcPtr = nullptr;
  size_t srcElementSizeInBytes = sizeof(unsigned char);
  size_t srcRowPitchInBytes = 0;
  size_t srcSlicePitchInBytes = 0;
  if (p->srcMemoryType == hipMemoryTypeHost) {
    srcPtr = p->srcHost;
    srcRowPitchInBytes = p->srcPitch;
    srcSlicePitchInBytes = srcRowPitchInBytes * p->srcHeight;
  } else if ((p->srcMemoryType == hipMemoryTypeDevice) ||
             (p->srcMemoryType == hipMemoryTypeUnified)) {
    srcPtr = p->srcDevice;
    srcRowPitchInBytes = p->srcPitch;
    srcSlicePitchInBytes = srcRowPitchInBytes * p->srcHeight;
  } else if (p->srcMemoryType == hipMemoryTypeArray) {
    srcPtr = p->srcArray->data;
    getByteSizeFromChannelFormatKind(p->srcArray->desc.f, &srcElementSizeInBytes);
    srcElementSizeInBytes *= p->srcArray->NumChannels;
    srcRowPitchInBytes = srcElementSizeInBytes * p->srcArray->width;
    srcSlicePitchInBytes = srcRowPitchInBytes * p->srcArray->height;
  } else {
    ShouldNotReachHere();
  }

  void* dstPtr = nullptr;
  size_t dstElementSizeInBytes = sizeof(unsigned char);
  size_t dstRowPitchInBytes = 0;
  size_t dstSlicePitchInBytes = 0;
  if (p->dstMemoryType == hipMemoryTypeHost) {
    dstPtr = p->dstHost;
    dstRowPitchInBytes = p->dstPitch;
    dstSlicePitchInBytes = dstRowPitchInBytes * p->dstHeight;
  } else if ((p->dstMemoryType == hipMemoryTypeDevice) ||
             (p->dstMemoryType == hipMemoryTypeUnified)) {
    dstPtr = p->dstDevice;
    dstRowPitchInBytes = p->dstPitch;
    dstSlicePitchInBytes = dstRowPitchInBytes * p->dstHeight;
  } else if (p->dstMemoryType == hipMemoryTypeArray) {
    dstPtr = p->dstArray->data;
    getByteSizeFromChannelFormatKind(p->dstArray->desc.f, &dstElementSizeInBytes);
    dstElementSizeInBytes *= p->dstArray->NumChannels;
    dstRowPitchInBytes = dstElementSizeInBytes * p->dstArray->width;
    dstSlicePitchInBytes = dstRowPitchInBytes * p->dstArray->height;
  } else {
    ShouldNotReachHere();
  }

  // For HIP arrays, srcXInBytes must be evenly divisible by the array element size.
  if ((p->srcMemoryType == hipMemoryTypeArray) &&
      ((p->srcXInBytes % srcElementSizeInBytes) != 0)) {
    return hipErrorInvalidValue;
  }

  // If specified, srcPitch must be greater than or equal to WidthInBytes + srcXInBytes
  if ((p->srcMemoryType != hipMemoryTypeArray) &&
      (p->srcPitch < (p->WidthInBytes + p->srcXInBytes))) {
    return hipErrorInvalidValue;
  }

  // If specified, srcHeight must be greater than or equal to Height + srcY
  if ((p->srcMemoryType != hipMemoryTypeArray) &&
      (p->srcHeight < (p->Height + p->srcY))) {
    return hipErrorInvalidValue;
  }

  // For HIP arrays, dstXInBytes must be evenly divisible by the array element size.
  if ((p->dstMemoryType == hipMemoryTypeArray) &&
      ((p->dstXInBytes % dstElementSizeInBytes) != 0)) {
    return hipErrorInvalidValue;
  }

  // If specified, srcPitch must be greater than or equal to WidthInBytes + srcXInBytes
  if ((p->dstMemoryType != hipMemoryTypeArray) &&
      (p->dstPitch < (p->WidthInBytes + p->dstXInBytes))) {
    return hipErrorInvalidValue;
  }

  // If specified, srcHeight must be greater than or equal to Height + srcY
  if ((p->dstMemoryType != hipMemoryTypeArray) &&
      (p->dstHeight < (p->Height + p->dstY))) {
    return hipErrorInvalidValue;
  }

  // The srcLOD and dstLOD members of the CUDA_MEMCPY3D structure must be set to 0.
  if ((p->srcLOD != 0) || (p->dstLOD != 0)) {
    return hipErrorInvalidValue;
  }

  size_t region[3];
  region[0] = p->WidthInBytes;
  region[1] = p->Height;
  region[2] = p->Depth;

  size_t srcOrigin[3];
  srcOrigin[0] = p->srcXInBytes;
  srcOrigin[1] = p->srcY;
  srcOrigin[2] = p->srcZ;

  size_t dstOrigin[3];
  dstOrigin[0] = p->dstXInBytes;
  dstOrigin[1] = p->dstY;
  dstOrigin[2] = p->dstZ;

  amd::BufferRect srcRect;
  if (!srcRect.create(srcOrigin, region, srcRowPitchInBytes, srcSlicePitchInBytes)) {
    return hipErrorInvalidValue;
  }

  size_t srcMemoryOffset = 0;
  amd::Memory* srcMemory = getMemoryObject(srcPtr, srcMemoryOffset);
  amd::Coord3D srcStart(srcRect.start_ + srcMemoryOffset, 0, 0);

  amd::BufferRect dstRect;
  if (!dstRect.create(dstOrigin, region, dstRowPitchInBytes, dstSlicePitchInBytes)) {
    return hipErrorInvalidValue;
  }

  size_t dstMemoryOffset = 0;
  amd::Memory* dstMemory = getMemoryObject(dstPtr, dstMemoryOffset);
  amd::Coord3D dstStart(dstRect.start_ + dstMemoryOffset, 0, 0);

  amd::Command* command = nullptr;
  amd::Command::EventWaitList waitList;
  amd::HostQueue* queue = hip::getQueue(stream);
  amd::Coord3D regionSize(region[0], region[1], region[2]);

  if (((srcMemory == nullptr) && (dstMemory == nullptr)) ||
      (p->kind == hipMemcpyHostToHost)) {
    memcpy(dstPtr, srcPtr, region[0] * region[1] * region[2]);
    return hipSuccess;
  } else if ((srcMemory == nullptr) && (dstMemory != nullptr)) {
    command = new amd::WriteMemoryCommand(*queue, CL_COMMAND_WRITE_BUFFER_RECT, waitList,
              *dstMemory->asBuffer(), srcStart, regionSize, srcPtr, srcRect, dstRect);
  } else if ((srcMemory != nullptr) && (dstMemory == nullptr)) {
    command = new amd::ReadMemoryCommand(*queue, CL_COMMAND_READ_BUFFER_RECT, waitList,
              *srcMemory->asBuffer(), srcStart, regionSize, dstPtr, srcRect, dstRect);
  } else if ((srcMemory != nullptr) &&  (dstMemory != nullptr)) {
    command = new amd::CopyMemoryCommand(*queue, CL_COMMAND_COPY_BUFFER_RECT, waitList,
              *srcMemory->asBuffer(),*dstMemory->asBuffer(), srcStart, dstStart, regionSize,
              srcRect, dstRect);
  }

  if (command == nullptr) {
    return hipErrorOutOfMemory;
  }

  command->enqueue();
  if (!isAsync) {
    command->awaitCompletion();
  }
  command->release();

  return hipSuccess;
}

hipError_t ihipMemcpy3D_V2(const struct hipMemcpy3DParms* p, hipStream_t stream, bool isAsync = false) {
  void* srcPtr = nullptr;
  size_t srcElementSizeInBytes = sizeof(unsigned char);
  size_t srcRowPitchInBytes = 0;
  size_t srcSlicePitchInBytes = 0;
  if ((p->srcArray != nullptr) && (p->srcPtr.ptr == nullptr)) {
    srcPtr = p->srcArray->data;
    getByteSizeFromChannelFormatKind(p->srcArray->desc.f, &srcElementSizeInBytes);
    srcElementSizeInBytes *= p->srcArray->NumChannels;
    srcRowPitchInBytes = srcElementSizeInBytes * p->srcArray->width;
    srcSlicePitchInBytes = srcRowPitchInBytes * p->srcArray->height;
  } else if ((p->srcArray == nullptr) && (p->srcPtr.ptr != nullptr)) {
    srcPtr = p->srcPtr.ptr;
    srcRowPitchInBytes = p->srcPtr.pitch;
    srcSlicePitchInBytes = srcRowPitchInBytes * p->srcPtr.ysize;
  } else {
    ShouldNotReachHere();
  }

  void* dstPtr = nullptr;
  size_t dstElementSizeInBytes = sizeof(unsigned char);
  size_t dstRowPitchInBytes = 0;
  size_t dstSlicePitchInBytes = 0;
  if ((p->dstArray != nullptr) && (p->dstPtr.ptr == nullptr)) {
    dstPtr = p->dstArray->data;
    getByteSizeFromChannelFormatKind(p->dstArray->desc.f, &dstElementSizeInBytes);
    dstElementSizeInBytes *= p->dstArray->NumChannels;
    dstRowPitchInBytes = dstElementSizeInBytes * p->dstArray->width;
    dstSlicePitchInBytes = dstRowPitchInBytes * p->dstArray->height;
  } else if ((p->dstArray == nullptr) && (p->dstPtr.ptr != nullptr)) {
    dstPtr = p->dstPtr.ptr;
    dstRowPitchInBytes = p->srcPtr.pitch;
    dstSlicePitchInBytes = dstRowPitchInBytes * p->dstPtr.ysize;
  } else {
    ShouldNotReachHere();
  }

  // If the source and destination are both arrays, they must have the same element size.
  if (((p->srcArray != nullptr) && (p->dstArray != nullptr)) &&
      (srcElementSizeInBytes != dstElementSizeInBytes)) {
    return hipErrorInvalidValue;
  }

  // If a HIP array is participating in the copy, the extent is defined in terms of that array's elements.
  // If no HIP array is participating in the copy, the extent is defined in elements of unsigned char.
  size_t region[3];
  if (p->srcArray != nullptr) {
    region[0] = srcRowPitchInBytes;
  } else if (p->dstArray != nullptr) {
    region[0] = dstRowPitchInBytes;
  } else {
    region[0] = sizeof(unsigned char) * p->extent.width;
  }
  region[1] = p->extent.height;
  region[2] = p->extent.depth;

  // The offset into the object is defined in units of the object's elements.
  size_t srcOrigin[3];
  srcOrigin[0] = srcElementSizeInBytes * p->srcPos.x;
  srcOrigin[1] = p->srcPos.y;
  srcOrigin[2] = p->srcPos.z;

  amd::BufferRect srcRect;
  if (!srcRect.create(srcOrigin, region, srcRowPitchInBytes, srcSlicePitchInBytes)) {
    return hipErrorInvalidValue;
  }

  size_t srcMemoryOffset = 0;
  amd::Memory* srcMemory = getMemoryObject(srcPtr, srcMemoryOffset);
  amd::Coord3D srcStart(srcRect.start_ + srcMemoryOffset, 0, 0);

  size_t dstOrigin[3];
  dstOrigin[0] = dstElementSizeInBytes * p->dstPos.x;
  dstOrigin[1] = p->dstPos.y;
  dstOrigin[2] = p->dstPos.z;

  amd::BufferRect dstRect;
  if (!dstRect.create(dstOrigin, region, dstRowPitchInBytes, dstSlicePitchInBytes)) {
    return hipErrorInvalidValue;
  }

  size_t dstMemoryOffset = 0;
  amd::Memory* dstMemory = getMemoryObject(dstPtr, dstMemoryOffset);
  amd::Coord3D dstStart(dstRect.start_ + dstMemoryOffset, 0, 0);

  amd::Command* command = nullptr;
  amd::Command::EventWaitList waitList;
  amd::HostQueue* queue = hip::getQueue(stream);
  amd::Coord3D regionSize(region[0], region[1], region[2]);

  if (((srcMemory == nullptr) && (dstMemory == nullptr)) ||
      (p->kind == hipMemcpyHostToHost)) {
    memcpy(dstPtr, srcPtr, region[0] * region[1] * region[2]);
    return hipSuccess;
  } else if ((srcMemory == nullptr) && (dstMemory != nullptr)) {
    command = new amd::WriteMemoryCommand(*queue, CL_COMMAND_WRITE_BUFFER_RECT, waitList,
              *dstMemory->asBuffer(), srcStart, regionSize, srcPtr, srcRect, dstRect);
  } else if ((srcMemory != nullptr) && (dstMemory == nullptr)) {
    command = new amd::ReadMemoryCommand(*queue, CL_COMMAND_READ_BUFFER_RECT, waitList,
              *srcMemory->asBuffer(), srcStart, regionSize, dstPtr, srcRect, dstRect);
  } else if ((srcMemory != nullptr) &&  (dstMemory != nullptr)) {
    command = new amd::CopyMemoryCommand(*queue, CL_COMMAND_COPY_BUFFER_RECT, waitList,
              *srcMemory->asBuffer(),*dstMemory->asBuffer(), srcStart, dstStart, regionSize,
              srcRect, dstRect);
  }

  if (command == nullptr) {
    return hipErrorOutOfMemory;
  }

  command->enqueue();
  if (!isAsync) {
    command->awaitCompletion();
  }
  command->release();

  return hipSuccess;
}

hipError_t ihipMemcpy3D(const struct hipMemcpy3DParms* p, hipStream_t stream, bool isAsync = false) {
  // Having src and dst be an array is ambigous, since we can't tell if the user intended to call hipMemcpy3D_V1() or hipMemcpy3D_V2().
  // For now hope that we never encounter this case.
  assert((p->srcArray == nullptr) || (p->dstArray == nullptr));

  // When calling hipMemcpy3D_V1(), the user must specify
  // one of srcHost, srcDevice or srcArray and
  // one of dstHost, dstDevice or dstArray.
  if (((p->srcHost != nullptr) || (p->srcDevice != nullptr) || (p->srcArray != nullptr)) &&
      ((p->dstHost != nullptr) || (p->dstDevice != nullptr) || (p->dstArray != nullptr))) {
    return ihipMemcpy3D_V1(p, stream, isAsync);
  }

  // When calling hipMemcpy3D_V2(), the user must specify
  // one of srcArray or srcPtr and
  // one of dstArray or dstPtr.
  if (((p->srcArray != nullptr) || (p->srcPtr.ptr != nullptr)) &&
      ((p->dstArray != nullptr) || (p->dstPtr.ptr != nullptr))) {
    return ihipMemcpy3D_V2(p, stream, isAsync);
  }

  // If we got here, then the user specified an invalid combination of src/dst parameters.
  return hipErrorInvalidValue;
}

hipError_t hipMemcpy3D(const struct hipMemcpy3DParms* p) {
  HIP_INIT_API(hipMemcpy3D, p);

  HIP_RETURN(ihipMemcpy3D(p, nullptr));
}

hipError_t hipMemcpy3DAsync(const struct hipMemcpy3DParms* p, hipStream_t stream) {
  HIP_INIT_API(hipMemcpy3DAsync, p, stream);

  HIP_RETURN(ihipMemcpy3D(p, stream, true));
}

hipError_t ihipMemset(void* dst, int value, size_t valueSize, size_t sizeBytes,
                      hipStream_t stream, bool isAsync = false) {
  if (sizeBytes == 0) {
    // Skip if nothing needs filling.
    return hipSuccess;
  }

  if (dst == nullptr) {
    return hipErrorInvalidValue;
  }

  size_t offset = 0;
  amd::HostQueue* queue = hip::getQueue(stream);
  amd::Memory* memory = getMemoryObject(dst, offset);

  if (memory != nullptr) {
    // Device memory
    amd::Command::EventWaitList waitList;
    amd::Coord3D fillOffset(offset, 0, 0);
    amd::Coord3D fillSize(sizeBytes, 1, 1);
    amd::FillMemoryCommand* command =
        new amd::FillMemoryCommand(*queue, CL_COMMAND_FILL_BUFFER, waitList, *memory->asBuffer(),
                                   &value, valueSize, fillOffset, fillSize);

    if (command == nullptr) {
      return hipErrorOutOfMemory;
    }

    command->enqueue();
    if (!isAsync) {
      command->awaitCompletion();
    }
    command->release();
  } else {
    // Host alloced memory
    memset(dst, value, sizeBytes);
  }

  return hipSuccess;
}

hipError_t hipMemset(void* dst, int value, size_t sizeBytes) {
  HIP_INIT_API(hipMemset, dst, value, sizeBytes);

  HIP_RETURN(ihipMemset(dst, value, sizeof(int8_t), sizeBytes, nullptr));
}

hipError_t hipMemsetAsync(void* dst, int value, size_t sizeBytes, hipStream_t stream) {
  HIP_INIT_API(hipMemsetAsync, dst, value, sizeBytes, stream);

  HIP_RETURN(ihipMemset(dst, value, sizeof(int8_t), sizeBytes, stream, true));
}

hipError_t hipMemsetD8(hipDeviceptr_t dst, unsigned char value, size_t count) {
  HIP_INIT_API(hipMemsetD8, dst, value, count);

  HIP_RETURN(ihipMemset(dst, value, sizeof(int8_t), count * sizeof(int8_t), nullptr));
}

hipError_t hipMemsetD8Async(hipDeviceptr_t dst, unsigned char value, size_t count,
                            hipStream_t stream) {
  HIP_INIT_API(hipMemsetD8Async, dst, value, count, stream);

  HIP_RETURN(ihipMemset(dst, value, sizeof(int8_t), count * sizeof(int8_t), stream, true));
}

hipError_t hipMemsetD16(hipDeviceptr_t dst, unsigned short value, size_t count) {
  HIP_INIT_API(hipMemsetD16, dst, value, count);

  HIP_RETURN(ihipMemset(dst, value, sizeof(int16_t), count * sizeof(int16_t), nullptr));
}

hipError_t hipMemsetD16Async(hipDeviceptr_t dst, unsigned short value, size_t count,
                             hipStream_t stream) {
  HIP_INIT_API(hipMemsetD16Async, dst, value, count, stream);

  HIP_RETURN(ihipMemset(dst, value, sizeof(int16_t), count * sizeof(int16_t), stream, true));
}

hipError_t hipMemsetD32(hipDeviceptr_t dst, int value, size_t count) {
  HIP_INIT_API(hipMemsetD32, dst, value, count);

  HIP_RETURN(ihipMemset(dst, value, sizeof(int32_t), count * sizeof(int32_t), nullptr));
}

hipError_t hipMemsetD32Async(hipDeviceptr_t dst, int value, size_t count,
                             hipStream_t stream) {
  HIP_INIT_API(hipMemsetD32Async, dst, value, count, stream);

  HIP_RETURN(ihipMemset(dst, value, sizeof(int32_t), count * sizeof(int32_t), stream, true));
}

hipError_t hipMemset2D(void* dst, size_t pitch, int value, size_t width, size_t height) {
  HIP_INIT_API(hipMemset2D, dst, pitch, value, width, height);

  HIP_RETURN(ihipMemset(dst, value, sizeof(int8_t), pitch * height, nullptr));
}

hipError_t hipMemset2DAsync(void* dst, size_t pitch, int value,
                            size_t width, size_t height, hipStream_t stream) {
  HIP_INIT_API(hipMemset2DAsync, dst, pitch, value, width, height, stream);

  HIP_RETURN(ihipMemset(dst, value, sizeof(int8_t), pitch * height, stream, true));
}

hipError_t hipMemset3D(hipPitchedPtr pitchedDevPtr, int value, hipExtent extent) {
  HIP_INIT_API(hipMemset3D, pitchedDevPtr, value, &extent);

  void *dst = pitchedDevPtr.ptr;
  size_t sizeBytes = pitchedDevPtr.pitch * extent.height * extent.depth;

  HIP_RETURN(ihipMemset(dst, value, sizeof(int8_t), sizeBytes, nullptr));
}

hipError_t hipMemset3DAsync(hipPitchedPtr pitchedDevPtr, int value, hipExtent extent, hipStream_t stream) {
  HIP_INIT_API(hipMemset3DAsync, pitchedDevPtr, value, &extent, stream);

  void *dst = pitchedDevPtr.ptr;
  size_t sizeBytes = pitchedDevPtr.pitch * extent.height * extent.depth;

  HIP_RETURN(ihipMemset(dst, value, sizeof(int8_t), sizeBytes, stream, true));
}

hipError_t hipMemAllocPitch(hipDeviceptr_t* dptr, size_t* pitch, size_t widthInBytes,
                            size_t height, unsigned int elementSizeBytes) {
  HIP_INIT_API(hipMemAllocPitch, dptr, pitch, widthInBytes, height, elementSizeBytes);

  HIP_RETURN(hipMallocPitch(dptr, pitch, widthInBytes, height));
}

hipError_t hipMemAllocHost(void** ptr, size_t size) {
  HIP_INIT_API(hipMemAllocHost, ptr, size);

  HIP_RETURN(hipHostMalloc(ptr, size, 0));
}

hipError_t hipIpcGetMemHandle(hipIpcMemHandle_t* handle, void* dev_ptr) {
  HIP_INIT_API(hipIpcGetMemHandle, handle, dev_ptr);

  size_t offset = 0;
  amd::Memory* amd_mem_obj = nullptr;
  device::Memory* dev_mem_obj = nullptr;
  ihipIpcMemHandle_t* ihandle = nullptr;

  if ((handle == nullptr) || (dev_ptr == nullptr)) {
    HIP_RETURN(hipErrorInvalidValue);
  }

  /* Get AMD::Memory object corresponding to this pointer */
  amd_mem_obj = getMemoryObject(dev_ptr, offset);
  if (amd_mem_obj == nullptr) {
    HIP_RETURN(hipErrorInvalidDevicePointer);
  }

  /* Get Device::Memory object pointer */
  dev_mem_obj = amd_mem_obj->getDeviceMemory(*hip::getCurrentContext()->devices()[0],false);
  if (dev_mem_obj == nullptr) {
    HIP_RETURN(hipErrorInvalidDevicePointer);
  }

  /* Create an handle for IPC. Store the memory size inside the handle */
  ihandle = reinterpret_cast<ihipIpcMemHandle_t *>(handle);
  dev_mem_obj->IpcCreate(offset, &(ihandle->psize), &(ihandle->ipc_handle));

  HIP_RETURN(hipSuccess);
}

hipError_t hipIpcOpenMemHandle(void** dev_ptr, hipIpcMemHandle_t handle, unsigned int flags) {
  HIP_INIT_API(hipIpcOpenMemHandle, dev_ptr, &handle, flags);

  amd::Memory* amd_mem_obj = nullptr;
  amd::Device* device = nullptr;
  ihipIpcMemHandle_t* ihandle = nullptr;

  if (dev_ptr == nullptr) {
    HIP_RETURN(hipErrorInvalidValue);
  }

  /* Call the IPC Attach from Device class */
  device = hip::getCurrentContext()->devices()[0];
  ihandle = reinterpret_cast<ihipIpcMemHandle_t *>(&handle);

  amd_mem_obj = device->IpcAttach(&(ihandle->ipc_handle), ihandle->psize, flags, dev_ptr);
  if (amd_mem_obj == nullptr) {
    HIP_RETURN(hipErrorInvalidDevicePointer);
  }

  /* Add the memory to the MemObjMap */
  amd::MemObjMap::AddMemObj(*dev_ptr, amd_mem_obj);

  HIP_RETURN(hipSuccess);
}

hipError_t hipIpcCloseMemHandle(void* dev_ptr) {
  HIP_INIT_API(hipIpcCloseMemHandle, dev_ptr);

  size_t offset = 0;
  amd::Device* device = nullptr;
  amd::Memory* amd_mem_obj = nullptr;

  hip::syncStreams();
  hip::getNullStream()->finish();

  if (dev_ptr == nullptr) {
    HIP_RETURN(hipErrorInvalidValue);
  }

  /* Get the amd::Memory object */
  amd_mem_obj = getMemoryObject(dev_ptr, offset);
  if (amd_mem_obj == nullptr) {
    HIP_RETURN(hipErrorInvalidDevicePointer);
  }

  /* Call IPC Detach from Device class */
  device = hip::getCurrentContext()->devices()[0];
  if (device == nullptr) {
    HIP_RETURN(hipErrorNoDevice);
  }

  /* Remove the memory from MemObjMap */
  amd::MemObjMap::RemoveMemObj(amd_mem_obj);

  /* detach the memory */
  device->IpcDetach(*amd_mem_obj);

  HIP_RETURN(hipSuccess);
}

hipChannelFormatDesc hipCreateChannelDesc(int x, int y, int z, int w, hipChannelFormatKind f) {
    hipChannelFormatDesc cd;
    cd.x = x;
    cd.y = y;
    cd.z = z;
    cd.w = w;
    cd.f = f;
    return cd;
}

hipError_t hipHostGetDevicePointer(void** devicePointer, void* hostPointer, unsigned flags) {
  HIP_INIT_API(hipHostGetDevicePointer, devicePointer, hostPointer, flags);

  size_t offset = 0;

  amd::Memory* memObj = getMemoryObject(hostPointer, offset);
  if (!memObj) {
    HIP_RETURN(hipErrorInvalidValue);
  }
  *devicePointer = reinterpret_cast<void*>(memObj->getDeviceMemory(*hip::getCurrentContext()->devices()[0])->virtualAddress() + offset);

  HIP_RETURN(hipSuccess);
}

hipError_t hipPointerGetAttributes(hipPointerAttribute_t* attributes, const void* ptr) {
  HIP_INIT_API(hipPointerGetAttributes, attributes, ptr);

  size_t offset = 0;
  amd::Memory* memObj = getMemoryObject(ptr, offset);
  int device = 0;

  if (memObj != nullptr) {
    attributes->memoryType = (CL_MEM_SVM_FINE_GRAIN_BUFFER & memObj->getMemFlags())? hipMemoryTypeHost : hipMemoryTypeDevice;
    attributes->hostPointer = memObj->getSvmPtr();
    attributes->devicePointer = memObj->getSvmPtr();
    attributes->isManaged = 0;
    attributes->allocationFlags = memObj->getMemFlags() >> 16;

    amd::Context &memObjCtx = memObj->getContext();
    if (*hip::host_context == memObjCtx) {
        attributes->device = ihipGetDevice();
        HIP_RETURN(hipSuccess);
    }
    for (auto& ctx : g_devices) {
      if (*ctx == memObjCtx) {
        attributes->device = device;
        HIP_RETURN(hipSuccess);
      }
      ++device;
    }
    HIP_RETURN(hipErrorInvalidDevice);
  }

  HIP_RETURN(hipErrorInvalidValue);
}