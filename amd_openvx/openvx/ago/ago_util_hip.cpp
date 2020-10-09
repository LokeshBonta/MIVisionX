/*
Copyright (c) 2019 Advanced Micro Devices, Inc. All rights reserved.

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
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/


#include "ago_internal.h"
#include "ago_haf_gpu.h"
#if  ENABLE_HIP

// Create HIP Context
int agoGpuHipCreateContext(AgoContext * context, int deviceID)
{
        if (deviceID >= 0) {
            hipError_t err;
            // use the given HIP device
            context->hip_context_imported = true;
            hipGetDeviceProperties(&context->hip_dev_prop, 0);
            err = hipSetDevice(deviceID);
            if (err != hipSuccess) {
                agoAddLogEntry(NULL, VX_FAILURE, "ERROR: hipSetDevice(%d) => %d (failed)\n", deviceID, err);
                return -1;
            }
            context->hip_device_id = deviceID;
        }
        else {            
            hipError_t err = hipInit(0);
            if (err != hipSuccess) {
                agoAddLogEntry(NULL, VX_FAILURE, "ERROR: hipInit(0) => %d (failed)\n", err);
                return -1;
            }
            hipGetDeviceCount(&context->hip_num_devices);
            //set the device forcontext if specified.
            if (deviceID < context->hip_num_devices) {
                hipSetDevice(context->hip_device_id);
                if (err != hipSuccess) {
                    agoAddLogEntry(NULL, VX_FAILURE, "ERROR: hipSetDevice(%d) => %d (failed)\n", deviceID, err);
                    return -1;
                }
                context->hip_device_id = deviceID;
            }
            err = hipStreamCreate(&context->hip_stream);
            if (err != hipSuccess) {
                agoAddLogEntry(NULL, VX_FAILURE, "ERROR: hipStreamCreate(%p) => %d (failed)\n", context->hip_stream, err);
                return -1;
            }
        }
        return 0;
}

int agoGpuHipReleaseContext(AgoContext * context)
{
    if (context->hip_stream) {
        hipStreamDestroy(context->hip_stream);
        context->hip_stream = NULL;
        // reset the device
        hipDeviceReset();
    }
    if (!context->hip_context_imported) {
        //hipCtxDestroy(context->hip_context);
        //context->hip_context = NULL;
        hipDeviceReset();
    }
    return 0;
}

int agoGpuHipReleaseGraph(AgoGraph * graph)
{
    if (graph->hip_stream0) {
        hipError_t status = hipStreamDestroy(graph->hip_stream0);
        if (status != hipSuccess) {
            agoAddLogEntry(&graph->ref, VX_FAILURE, "ERROR: agoGpuHipReleaseGraph: hipStreamDestroy(%p) failed (%d)\n", graph->hip_stream0, status);
            return -1;
        }
        graph->hip_stream0 = NULL;
    }
    return 0;
}

int agoGpuHipReleaseData(AgoData * data)
{
    if (data->hip_memory_allocated) {
        hipFree((void *)data->hip_memory_allocated);
        data->hip_memory_allocated = NULL;
        data->ref.context->hip_mem_release_count++;
    }
#if defined(CL_VERSION_2_0)
    if (data->opencl_svm_buffer_allocated) {
		if (data->ref.context->opencl_config_flags & CONFIG_OPENCL_SVM_AS_FGS) {
			agoReleaseMemory(data->opencl_svm_buffer_allocated);
		}
		else {
			clSVMFree(data->ref.context->opencl_context, data->opencl_svm_buffer_allocated);
		}
		data->opencl_svm_buffer_allocated = NULL;
	}
	data->opencl_svm_buffer = NULL;
#endif
    data->hip_memory = NULL;
    data->opencl_buffer_offset = 0;
    return 0;
}


int agoGpuHipReleaseSuperNode(AgoSuperNode * supernode)
{
    if (supernode->hip_stream0) {
        hipError_t status = hipStreamDestroy(supernode->hip_stream0);
        if (status != hipSuccess) {
            //agoAddLogEntry(&graph->ref, VX_FAILURE, "ERROR: agoGpuHipReleaseGraph: hipStreamDestroy(%p) failed (%d)\n", graph->hip_stream0, status);
            return -1;
        }
        supernode->hip_stream0 = NULL;
    }
    return 0;
}

// create HIP device memory
static void agoGpuHipCreateBuffer(AgoContext * context, void * host_ptr, size_t size, hipError_t errcode_ret)
{
        errcode_ret = hipMalloc((void **) &host_ptr, size);
        if (host_ptr && (errcode_ret == hipSuccess) ) {
            context->hip_mem_alloc_count++;
            context->hip_mem_alloc_size += size;
        }
        return;
}

int agoGpuHipAllocBuffer(AgoData * data)
{
    // make sure buffer is valid
    if (agoDataSanityCheckAndUpdate(data)) {
            return -1;
    }
    // allocate buffer
    AgoContext * context = data->ref.context;
    if (data->ref.type == VX_TYPE_IMAGE) {
        AgoData * dataMaster = data->u.img.roiMasterImage ? data->u.img.roiMasterImage : data; // to handle image ROI
        if (!dataMaster->hip_memory && !dataMaster->u.img.enableUserBufferOpenCL && !(dataMaster->import_type == VX_MEMORY_TYPE_HIP)) {
            hipError_t err = hipSuccess;
            {
                // allocate hip_memory
                dataMaster->hip_memory = 0;
                agoGpuHipCreateBuffer(context, dataMaster->hip_memory, dataMaster->size + dataMaster->opencl_buffer_offset, err);
                dataMaster->hip_memory_allocated = dataMaster->hip_memory;
            }
            if (!dataMaster->hip_memory || err) {
                agoAddLogEntry(&context->ref, VX_FAILURE, "ERROR: agoGpuHipAllocBuffer(MEM_READ_WRITE,%d,0,*) FAILED\n", (int)dataMaster->size + dataMaster->opencl_buffer_offset);
                return -1;
            }
            if (dataMaster->u.img.isUniform) {
                // make sure that CPU buffer is allocated
                if (!dataMaster->buffer) {
                    if (agoAllocData(dataMaster)) {
                        return -1;
                    }
                }
                // copy the uniform image into HIP memory because there won't be any commits happening to this buffer
                hipError_t err = hipMemcpyHtoD((void *)(dataMaster->hip_memory + dataMaster->opencl_buffer_offset), dataMaster->buffer, dataMaster->size);
                if (err != hipSuccess) {
                    agoAddLogEntry(&context->ref, VX_FAILURE, "ERROR: agoGpuHipAllocBuffer: hipMemcpyHtoD() => %d\n", err);
                    return -1;
                }
                dataMaster->buffer_sync_flags |= AGO_BUFFER_SYNC_FLAG_DIRTY_SYNCHED;
            }
        }
        if (data != dataMaster) {
            // special handling for image ROI
            data->hip_memory = dataMaster->hip_memory;
        }
    }
    else if (data->ref.type == VX_TYPE_ARRAY || data->ref.type == AGO_TYPE_CANNY_STACK) {
        hipError_t err = hipSuccess;
        if (!data->hip_memory) {
            data->opencl_buffer_offset = DATA_OPENCL_ARRAY_OFFSET; // first few bytes reserved for numitems/stacktop
            err = hipSuccess;
            {
                // normal opencl_buffer allocation
                data->hip_memory = 0;
                agoGpuHipCreateBuffer(context, data->hip_memory, data->size + data->opencl_buffer_offset, err);
                data->hip_memory_allocated = data->hip_memory;
                if (data->hip_memory) {
                    // initialize array header which containts numitems
                    err = hipMemset(data->hip_memory, 0, data->size + data->opencl_buffer_offset);
                }
            }
            if (err) {
                agoAddLogEntry(&context->ref, VX_FAILURE, "ERROR: agoGpuHipAllocBuffer(MEM_READ_WRITE,%d,0,*) FAILED\n", (int)data->size + data->opencl_buffer_offset);
                return -1;
            }
        }
    }
    else if (data->ref.type == VX_TYPE_SCALAR || data->ref.type == VX_TYPE_THRESHOLD || data->ref.type == VX_TYPE_CONVOLUTION) {
            // nothing to do
    }
    else if (data->ref.type == VX_TYPE_LUT) {
        hipError_t err = hipSuccess;
        if (!data->hip_memory) {
            if (data->u.lut.type == VX_TYPE_UINT8) {
                // todo:: allocate HIP 2D array
                data->opencl_buffer_offset = 0;
            }
            else {
                // normal opencl_buffer allocation
                data->hip_memory = 0;
                agoGpuHipCreateBuffer(context, data->hip_memory, data->size + data->opencl_buffer_offset, err);
                data->hip_memory_allocated = data->hip_memory;
                if (err) {
                    agoAddLogEntry(&context->ref, VX_FAILURE, "ERROR: agoGpuHipAllocBuffer(MEM_READ_WRITE,%d,0,*) FAILED\n", (int)data->size + data->opencl_buffer_offset);
                    return -1;
                }
            }
        }
    }
    else if (data->ref.type == VX_TYPE_REMAP) {
        hipError_t err = hipSuccess;
        if (!data->hip_memory) {
            data->hip_memory = 0;
            agoGpuHipCreateBuffer(context, data->hip_memory, data->size + data->opencl_buffer_offset, err);
            data->hip_memory_allocated = data->hip_memory;
            if (err) {
                agoAddLogEntry(&context->ref, VX_FAILURE, "ERROR: agoGpuHipAllocBuffer(MEM_READ_WRITE,%d,0,*) FAILED\n", (int)data->size + data->opencl_buffer_offset);
                return -1;
            }
            data->opencl_buffer_offset = 0;
        }
    }
    else if (data->ref.type == VX_TYPE_MATRIX) {
        if (!data->hip_memory) {
            hipError_t err = hipSuccess;
            data->hip_memory = 0;
            agoGpuHipCreateBuffer(context, data->hip_memory, data->size + data->opencl_buffer_offset, err);
            data->hip_memory_allocated = data->hip_memory;
            if (err) {
                agoAddLogEntry(&context->ref, VX_FAILURE, "ERROR: agoGpuHipAllocBuffer(MEM_READ_WRITE,%d,0,*) FAILED\n", (int)data->size + data->opencl_buffer_offset);
                return -1;
            }
            data->opencl_buffer_offset = 0;
        }
    }
    else if (data->ref.type == VX_TYPE_TENSOR) {
        AgoData * dataMaster = data->u.tensor.roiMaster ? data->u.tensor.roiMaster : data; // to handle tensor ROI
        if (!dataMaster->hip_memory) {
            hipError_t err = hipSuccess;
            dataMaster->hip_memory = 0;
            agoGpuHipCreateBuffer(context, dataMaster->hip_memory, dataMaster->size + dataMaster->opencl_buffer_offset, err);
            dataMaster->hip_memory_allocated = dataMaster->hip_memory;
            if (err) {
                agoAddLogEntry(&context->ref, VX_FAILURE, "ERROR: agoGpuHipAllocBuffer(MEM_READ_WRITE,%d,0,*) FAILED\n", (int)dataMaster->size + dataMaster->opencl_buffer_offset);
                return -1;
            }
            dataMaster->opencl_buffer_offset = 0;
        }
        if (data != dataMaster) {
            // special handling for tensor ROI
            data->hip_memory = dataMaster->hip_memory;
            data->opencl_buffer_offset = (vx_uint32)data->u.tensor.offset;
        }
    }
    else if (data->numChildren > 0) {
        for (vx_uint32 child = 0; child < data->numChildren; child++) {
            if (agoGpuHipAllocBuffer(data->children[child]) < 0) {
                    return -1;
            }
        }
    }
    else {
            agoAddLogEntry(&data->ref, VX_FAILURE, "ERROR: agoGpuOclAllocBuffer: doesn't support object type %s of %s\n", agoEnum2Name(data->ref.type), data->name.length() ? "?" : data->name.c_str());
            return -1;
    }
    // allocate CPU buffer
    if (agoAllocData(data)) {
            return -1;
    }
    return 0;
}

static int agoGpuHipDataInputSync(AgoGraph * graph, AgoData * data, vx_uint32 dataFlags, vx_uint32 group, bool need_access, bool need_read_access)
{
    if (data->ref.type == VX_TYPE_IMAGE) {
        if (need_access) { // only use image objects that need read access
            if (!data->hip_memory && data->isVirtual && data->ownerOfUserBufferOpenCL &&
                data->ownerOfUserBufferOpenCL->akernel->opencl_buffer_update_callback_f)
            { // need to update hip_memory from user kernel
                vx_status status = data->ownerOfUserBufferOpenCL->akernel->opencl_buffer_update_callback_f(data->ownerOfUserBufferOpenCL,
                    (vx_reference *)data->ownerOfUserBufferOpenCL->paramList, data->ownerOfUserBufferOpenCL->paramCount);
                if (status || !data->hip_memory) {
                    agoAddLogEntry(&data->ownerOfUserBufferOpenCL->ref, status, "ERROR: opencl_buffer_update_callback_f: failed(%d:%p)\n", status, data->hip_memory);
                    return -1;
                }
            }
            if (data->isDelayed) {
                data->hip_need_as_argument = 1;
            }
            else if ((data->u.img.enableUserBufferOpenCL || data->import_type == VX_MEMORY_TYPE_HIP) && data->hip_memory) {
                data->hip_need_as_argument = 1;
            }
            if (need_read_access) {
                auto dataToSync = data->u.img.isROI ? data->u.img.roiMasterImage : data;
                if (!(dataToSync->buffer_sync_flags & AGO_BUFFER_SYNC_FLAG_DIRTY_SYNCHED)) {
                    if (dataToSync->buffer_sync_flags & (AGO_BUFFER_SYNC_FLAG_DIRTY_BY_NODE | AGO_BUFFER_SYNC_FLAG_DIRTY_BY_COMMIT)) {
                        int64_t stime = agoGetClockCounter();
                        // HIP write HostToDevice
                        if (dataToSync->hip_memory) {
                            hipError_t err = hipMemcpy(dataToSync->hip_memory + dataToSync->opencl_buffer_offset, dataToSync->buffer, dataToSync->size, hipMemcpyHostToDevice);
                            if (err) {
                                agoAddLogEntry(&graph->ref, VX_FAILURE, "ERROR: hipMemcpyHtoD() => %d\n", err);
                                return -1;
                            }
                        }
                        dataToSync->buffer_sync_flags |= AGO_BUFFER_SYNC_FLAG_DIRTY_SYNCHED;
                        int64_t etime = agoGetClockCounter();
                        graph->opencl_perf.buffer_write += etime - stime;
                    }
                }
            }
        }
    }
    else if (data->ref.type == VX_TYPE_ARRAY) {
        if (data->isDelayed) {
            data->hip_need_as_argument = 1;
        }
        if (need_read_access) {
            if (!(data->buffer_sync_flags & AGO_BUFFER_SYNC_FLAG_DIRTY_SYNCHED)) {
                if (data->buffer_sync_flags & (AGO_BUFFER_SYNC_FLAG_DIRTY_BY_NODE | AGO_BUFFER_SYNC_FLAG_DIRTY_BY_COMMIT)) {
                    int64_t stime = agoGetClockCounter();
                    vx_size size = data->u.arr.numitems * data->u.arr.itemsize;
                    if (size > 0 && data->hip_memory) {
                        hipError_t err = hipMemcpyHtoD(data->hip_memory + data->opencl_buffer_offset, data->buffer, data->size);
                        if (err) {
                            agoAddLogEntry(&data->ref, VX_FAILURE, "ERROR: hipMemcpyHtoD() => %d (array)\n", err);
                            return -1;
                        }
                    }
                    data->buffer_sync_flags |= AGO_BUFFER_SYNC_FLAG_DIRTY_SYNCHED;
                    int64_t etime = agoGetClockCounter();
                    graph->opencl_perf.buffer_write += etime - stime;
#if ENABLE_DEBUG_DUMP_CL_BUFFERS
                    clDumpBuffer("input_%04d.bin", opencl_cmdq, data);
#endif
                }
            }
        }
    }
    else if (data->ref.type == AGO_TYPE_CANNY_STACK) {
        if (data->isDelayed) {
            data->hip_need_as_argument = 1;
        }
        if (need_read_access) {
            agoAddLogEntry(&data->ref, VX_FAILURE, "ERROR: agoGpuOclDataSyncInputs: doesn't support object type %s for read-access in group#%d for kernel arg setting\n", agoEnum2Name(data->ref.type), group);
            return -1;
        }
    }
    else if (data->ref.type == VX_TYPE_THRESHOLD) {
        // need to do this at the kernel launch time
 #if 0
        uint2 value;
        value.x = data->u.thr.threshold_lower;
        if (data->u.thr.thresh_type == VX_THRESHOLD_TYPE_RANGE) {
            size = sizeof(uint2);
            value.y = data->u.thr.threshold_upper;
        }
#endif
    }
    else if ((data->ref.type == VX_TYPE_SCALAR) || (data->ref.type == VX_TYPE_CONVOLUTION)) {
        // nothing to do.. the node will
    }
    else if (data->ref.type == VX_TYPE_MATRIX) {
        //data pass by value has to be decided inside the kernel implementation
        if (data->isDelayed) {
            data->hip_need_as_argument = 1;
        }
        if (need_read_access) {
            if (data->hip_memory && !(data->buffer_sync_flags & AGO_BUFFER_SYNC_FLAG_DIRTY_SYNCHED)) {
                if (data->buffer_sync_flags & (AGO_BUFFER_SYNC_FLAG_DIRTY_BY_NODE | AGO_BUFFER_SYNC_FLAG_DIRTY_BY_COMMIT)) {
                    int64_t stime = agoGetClockCounter();
                    if (data->size > 0 && data->hip_memory) {
                        hipError_t err = hipMemcpyHtoD(data->hip_memory + data->opencl_buffer_offset, data->buffer, data->size);
                        if (err) {
                            agoAddLogEntry(&data->ref, VX_FAILURE, "ERROR: hipMemcpyHtoD() => %d (array)\n", err);
                            return -1;
                        }
                    }
                    data->buffer_sync_flags |= AGO_BUFFER_SYNC_FLAG_DIRTY_SYNCHED;
                    int64_t etime = agoGetClockCounter();
                    graph->opencl_perf.buffer_write += etime - stime;
                }
            }
        }
    }
    else if (data->ref.type == VX_TYPE_LUT) {
        if (need_access) { // only use lut objects that need read access
            if (data->isDelayed) {
                data->hip_need_as_argument = 1;
            }
            if (need_read_access) {
                if (!(data->buffer_sync_flags & AGO_BUFFER_SYNC_FLAG_DIRTY_SYNCHED)) {
                    if (data->buffer_sync_flags & (AGO_BUFFER_SYNC_FLAG_DIRTY_BY_NODE | AGO_BUFFER_SYNC_FLAG_DIRTY_BY_COMMIT)) {
                        int64_t stime = agoGetClockCounter();
                        if (data->u.lut.type == VX_TYPE_UINT8 && data->hip_memory) {
                            hipError_t err = hipMemcpy2D(data->hip_memory, 1, data->buffer, 256, 256, 1, hipMemcpyHostToDevice);
                            if (err) {
                                agoAddLogEntry(&data->ref, VX_FAILURE, "ERROR: hipMemcpy2D(lut) => %d\n", err);
                                return -1;
                            }
                        }
                        else if (data->u.lut.type == VX_TYPE_INT16 && data->hip_memory && data->size >0) {
                            hipError_t err = hipMemcpyHtoD(data->hip_memory + data->opencl_buffer_offset, data->buffer, data->size);
                            if (err) {
                                agoAddLogEntry(&data->ref, VX_FAILURE, "ERROR: agoGpuOclDataInputSync: hipMemcpyHtoD() => %d (for LUT)\n", err);
                                return -1;
                            }
                        }
                        data->buffer_sync_flags |= AGO_BUFFER_SYNC_FLAG_DIRTY_SYNCHED;
                        int64_t etime = agoGetClockCounter();
                        graph->opencl_perf.buffer_write += etime - stime;
                    }
                }
            }
        }
    }
    else if (data->ref.type == VX_TYPE_REMAP) {
        if (need_access) { // only use image objects that need read access
            if (data->isDelayed) {
                data->hip_need_as_argument = 1;
            }
            if (need_read_access) {
                if (data->hip_memory && !(data->buffer_sync_flags & AGO_BUFFER_SYNC_FLAG_DIRTY_SYNCHED)) {
                    if (data->buffer_sync_flags & (AGO_BUFFER_SYNC_FLAG_DIRTY_BY_NODE | AGO_BUFFER_SYNC_FLAG_DIRTY_BY_COMMIT)) {
                        int64_t stime = agoGetClockCounter();
                        hipError_t err = hipMemcpyHtoD(data->hip_memory + data->opencl_buffer_offset, data->buffer, data->size);
                        if (err) {
                            agoAddLogEntry(&data->ref, VX_FAILURE, "ERROR: agoGpuOclDataInputSync: hipMemcpyHtoD() => %d (for Remap)\n", err);
                            return -1;
                        }
                        data->buffer_sync_flags |= AGO_BUFFER_SYNC_FLAG_DIRTY_SYNCHED;
                        int64_t etime = agoGetClockCounter();
                        graph->opencl_perf.buffer_write += etime - stime;
                    }
                }
            }
        }
    }
    else if (data->ref.type == VX_TYPE_TENSOR) {
        if (data->isDelayed) {
            // needs to set opencl_buffer everytime when the buffer is part of a delay object
            data->hip_need_as_argument = 1;
        }
        if (need_read_access) {
            auto dataToSync = data->u.tensor.roiMaster ? data->u.tensor.roiMaster : data;
            if (!(dataToSync->buffer_sync_flags & AGO_BUFFER_SYNC_FLAG_DIRTY_SYNCHED)) {
                if (dataToSync->buffer_sync_flags & (AGO_BUFFER_SYNC_FLAG_DIRTY_BY_NODE | AGO_BUFFER_SYNC_FLAG_DIRTY_BY_COMMIT)) {
                    int64_t stime = agoGetClockCounter();
                    if (dataToSync->hip_memory) {
                        hipError_t err = hipMemcpyHtoD(dataToSync->hip_memory + dataToSync->opencl_buffer_offset, dataToSync->buffer, dataToSync->size);
                        if (err) {
                            agoAddLogEntry(&graph->ref, VX_FAILURE, "ERROR: hipMemcpyHtoD() => %d (tensor)\n", err);
                            return -1;
                        }
                    }
                    dataToSync->buffer_sync_flags |= AGO_BUFFER_SYNC_FLAG_DIRTY_SYNCHED;
                    int64_t etime = agoGetClockCounter();
                    graph->opencl_perf.buffer_write += etime - stime;
#if ENABLE_DEBUG_DUMP_CL_BUFFERS
                    char fileName[128]; sprintf(fileName, "input_%%04d_tensor.raw");
                    clDumpBuffer(fileName, opencl_cmdq, dataToSync);
#endif
                }
            }
        }
    }
    else {
        agoAddLogEntry(&data->ref, VX_FAILURE, "ERROR: agoGpuOclDataSyncInputs: doesn't support object type %s in group#%d for kernel arg setting\n", agoEnum2Name(data->ref.type), group);
        return -1;
    }
    return 0;
}


int agoGpuHipSuperNodeMerge(AgoGraph * graph, AgoSuperNode * supernode, AgoNode * node)
{
    // sanity check
    if (!node->akernel->func && !node->akernel->kernel_f) {
        agoAddLogEntry(&node->akernel->ref, VX_FAILURE, "ERROR: agoGpuHipSuperNodeMerge: doesn't support kernel %s\n", node->akernel->name);
        return -1;
    }
    // merge node into supernode
    supernode->nodeList.push_back(node);
    for (vx_uint32 i = 0; i < node->paramCount; i++) {
        AgoData * data = node->paramList[i];
        if (data) {
            size_t index = std::find(supernode->dataList.begin(), supernode->dataList.end(), data) - supernode->dataList.begin();
            if (index == supernode->dataList.size()) {
                // add data with zero entries into the lists
                AgoSuperNodeDataInfo info = { 0 };
                info.needed_as_a_kernel_argument = true;
                supernode->dataInfo.push_back(info);
                supernode->dataList.push_back(data);
                supernode->dataListForAgeDelay.push_back(data);
            }
            // update count for data direction
            supernode->dataInfo[index].argument_usage[node->parameters[i].direction]++;
        }
    }
    return 0;
}

int agoGpuHipSuperNodeUpdate(AgoGraph * graph, AgoSuperNode * supernode)
{
    // make sure that all output images have same dimensions
    // check to make sure that max input hierarchy level is less than min output hierarchy level
    vx_uint32 width = 0, height = 0;
    vx_uint32 max_input_hierarchical_level = 0, min_output_hierarchical_level = INT_MAX;
    for (size_t index = 0; index < supernode->dataList.size(); index++) {
        AgoData * data = supernode->dataList[index];
        if (data->ref.type == VX_TYPE_IMAGE && supernode->dataInfo[index].argument_usage[VX_INPUT] == 0) {
            if (!width || !height) {
                width = data->u.img.width;
                height = data->u.img.height;
            }
            else if (width != data->u.img.width || height != data->u.img.height) {
                agoAddLogEntry(&data->ref, VX_FAILURE, "ERROR: agoGpuHipSuperNodeUpdate: doesn't support different image dimensions inside same group#%d\n", supernode->group);
                return -1;
            }
        }
        if (data->isVirtual && data->ref.type != VX_TYPE_SCALAR &&
            data->inputUsageCount == supernode->dataInfo[index].argument_usage[VX_INPUT] &&
            data->outputUsageCount == supernode->dataInfo[index].argument_usage[VX_OUTPUT] &&
            data->inoutUsageCount == supernode->dataInfo[index].argument_usage[VX_BIDIRECTIONAL])
        {
            // no need of this parameter as an argument into the kernel
            // mark that this will be an internal variable for the kernel
            supernode->dataInfo[index].needed_as_a_kernel_argument = false;
            // TBD: mark this the buffer doesn't need allocation
        }
        if (data->hierarchical_level > min_output_hierarchical_level) min_output_hierarchical_level = data->hierarchical_level;
        if (data->hierarchical_level < max_input_hierarchical_level) max_input_hierarchical_level = data->hierarchical_level;
    }
    if (max_input_hierarchical_level > min_output_hierarchical_level) {
        agoAddLogEntry(&graph->ref, VX_FAILURE, "ERROR: agoGpuHipSuperNodeUpdate: doesn't support mix of hierarchical levels inside same group#%d\n", supernode->group);
        return -1;
    }
    supernode->width = width;
    supernode->height = height;

    // mark hierarchical level (start,end) of all supernodes
    for (AgoSuperNode * supernode = graph->supernodeList; supernode; supernode = supernode->next) {
        supernode->hierarchical_level_start = INT_MAX;
        supernode->hierarchical_level_end = 0;
        for (AgoNode * node : supernode->nodeList) {
            supernode->hierarchical_level_start = min(supernode->hierarchical_level_start, node->hierarchical_level);
            supernode->hierarchical_level_end = max(supernode->hierarchical_level_end, node->hierarchical_level);
        }
    }

    return 0;
}


int agoGpuHipSingleNodeFinalize(AgoGraph * graph, AgoNode * node)
{
    const char * hip_code = node->hip_code.c_str();

    // dump Hip kernel if environment variable AGO_DUMP_GPU is specified with dump file path prefix
    // the output file name will be "$(AGO_DUMP_GPU)-0.<counter>.cu"
    if (hip_code) {
        char textBuffer[1024];
        if (agoGetEnvironmentVariable("AGO_DUMP_GPU", textBuffer, sizeof(textBuffer))) {
                char fileName[1024]; static int counter = 0;
                sprintf(fileName, "%s-0.%04d.cu", textBuffer, counter++);
                FILE * fp = fopen(fileName, "w");
                if (!fp) agoAddLogEntry(NULL, VX_FAILURE, "ERROR: unable to create: %s\n", fileName);
                else {
                        fprintf(fp, "%s", hip_code);
                        fclose(fp);
                        agoAddLogEntry(NULL, VX_SUCCESS, "OK: created %s\n", fileName);
                }
        }
    }


// HIP:: make sure HIP program is created in the node... as well as setting parameters;

    return 0;
}

int agoGpuHipSuperNodeFinalize(AgoGraph * graph, AgoSuperNode * node)
{
// Super node is not supported in hip yet
// HIP:: make sure HIP program is created in the node... as well as setting parameters;

    return -1;
}


static int agoGpuHipDataOutputMarkDirty(AgoGraph * graph, AgoData * data, bool need_access, bool need_write_access)
{
    if (data->ref.type == VX_TYPE_IMAGE) {
        if (need_access) { // only use image objects that need write access
            if (need_write_access) {
                auto dataToSync = data->u.img.isROI ? data->u.img.roiMasterImage : data;
                dataToSync->buffer_sync_flags &= ~AGO_BUFFER_SYNC_FLAG_DIRTY_MASK;
                dataToSync->buffer_sync_flags |= AGO_BUFFER_SYNC_FLAG_DIRTY_BY_NODE_CL;
            }
        }
    }
    else if (data->ref.type == VX_TYPE_ARRAY || data->ref.type == VX_TYPE_MATRIX) {
        if (need_access) { // only use image objects that need write access
            if (need_write_access) {
                data->buffer_sync_flags &= ~AGO_BUFFER_SYNC_FLAG_DIRTY_MASK;
                data->buffer_sync_flags |= AGO_BUFFER_SYNC_FLAG_DIRTY_BY_NODE_CL;
            }
        }
    }
    else if (data->ref.type == VX_TYPE_TENSOR) {
        if (need_access) { // only use tensor objects that need write access
            if (need_write_access) {
                auto dataToSync = data->u.tensor.roiMaster ? data->u.tensor.roiMaster : data;
                dataToSync->buffer_sync_flags &= ~AGO_BUFFER_SYNC_FLAG_DIRTY_MASK;
                dataToSync->buffer_sync_flags |= AGO_BUFFER_SYNC_FLAG_DIRTY_BY_NODE_CL;
            }
        }
    }
    return 0;
}

static int agoGpuHipDataOutputAtomicSync(AgoGraph * graph, AgoData * data)
{

    if (data->ref.type == VX_TYPE_ARRAY) {
        // update number of items
        int64_t stime = agoGetClockCounter();
        vx_uint32 * pNumItems = nullptr;

        if (data->hip_memory) {
            hipError_t err = hipMemcpyDtoH((void *)data->buffer, (data->hip_memory + data->opencl_buffer_offset), data->size);
            if (err) {
                agoAddLogEntry(&data->ref, VX_FAILURE, "ERROR: hipMemcpyDtoH() for numitems => %d\n", err);
                return -1;
            }
            pNumItems = (vx_uint32 *) data->buffer;
        }
        int64_t etime = agoGetClockCounter();
        graph->opencl_perf.buffer_read += etime - stime;
        // read and reset the counter
        data->u.arr.numitems = *pNumItems;
    }
    else if (data->ref.type == AGO_TYPE_CANNY_STACK) {
        // update number of items and reset it for next use
        int64_t stime = agoGetClockCounter();
        vx_uint8 * stack = nullptr;
        if (data->hip_memory) {
            hipError_t err = hipMemcpyDtoH((void *)data->buffer, (data->hip_memory + data->opencl_buffer_offset), data->size);
            if (err) {
                agoAddLogEntry(&data->ref, VX_FAILURE, "ERROR: hipMemcpyDtoH() for stacktop => %d\n", err);
                return -1;
            }
            stack = (vx_uint8 *)data->buffer;
        }
        int64_t etime = agoGetClockCounter();
        graph->opencl_perf.buffer_read += etime - stime;
        data->u.cannystack.stackTop = *(vx_uint32 *)stack;
    }
    return 0;
}


int agoGpuHipSingleNodeLaunch(AgoGraph * graph, AgoNode * node)
{
    // compute global work (if requested) and set numitems of output array (if requested further)
    // Not needed for hip since the node is responsible for launching kernel at runtime

    // make sure that all input buffers are synched and other arguments are updated
    for (size_t index = 0; index < node->paramCount; index++) {
        if (node->paramList[index] ) {
            bool need_read_access = node->parameters[index].direction != VX_OUTPUT ? true : false;
            vx_uint32 dataFlags = NODE_HIP_TYPE_NEED_IMGSIZE;
            if (agoGpuHipDataInputSync(graph, node->paramList[index], dataFlags, 0, true, need_read_access) < 0) {
                return -1;
            }
        }
    }
#if 0
    // update global work if needed
    if (node->akernel->opencl_global_work_update_callback_f) {
        vx_status status = node->akernel->opencl_global_work_update_callback_f(node, (vx_reference *)node->paramList, node->paramCount, node->opencl_work_dim, node->opencl_global_work, node->opencl_local_work);
        if (status) {
            agoAddLogEntry(&node->ref, VX_FAILURE, "ERROR: agoGpuHipSingleNodeLaunch: invalid opencl_global_work_update_callback_f failed (%d) for kernel %s\n", status, node->akernel->name);
            return -1;
        }
        for(vx_size dim = node->opencl_work_dim; dim < 3; dim++) {
            node->opencl_global_work[dim] = 1;
            node->opencl_local_work[dim] = 1;
        }
        node->opencl_work_dim = 3;
    }
#endif
    // call execute kernel
    float eventMs = 1.0f;
    AgoKernel * kernel = node->akernel;
    vx_status status = VX_SUCCESS;
    hipEventRecord(node->hip_event_start, graph->hip_stream0);
    if (kernel->func) {
        status = kernel->func(node, ago_kernel_cmd_hip_execute);
        if (status == AGO_ERROR_KERNEL_NOT_IMPLEMENTED)
            status = VX_ERROR_NOT_IMPLEMENTED;
    }
    else if (kernel->kernel_f) {
        status = kernel->kernel_f(node, (vx_reference *)node->paramList, node->paramCount);
    }
    if (status) {
        agoAddLogEntry((vx_reference)graph, VX_FAILURE, "ERROR: kernel %s exec failed (%d:%s)\n", kernel->name, status, agoEnum2Name(status));
        return -1;
    }
    if(graph->enable_node_level_opencl_flush) {
        hipError_t err = hipStreamSynchronize(graph->hip_stream0);
        if (err) {
            agoAddLogEntry(&node->ref, VX_FAILURE, "ERROR: hipStreamSynchronize(singlenode) failed(%d) for %s\n", err, node->akernel->name);
            return -1;
        }
    }
    hipEventRecord(node->hip_event_stop, graph->hip_stream0);
    hipEventElapsedTime(&eventMs, node->hip_event_start, node->hip_event_stop);
    graph->opencl_perf.kernel_enqueue += eventMs;

    // mark that node outputs are dirty
    for (size_t index = 0; index < node->paramCount; index++) {
        if (node->paramList[index]) {
            bool need_write_access = node->parameters[index].direction != VX_INPUT ? true : false;
            if (agoGpuHipDataOutputMarkDirty(graph, node->paramList[index], true, need_write_access) < 0) {
                    return -1;
            }
        }
    }
    return 0;
}

int agoGpuHipSuperNodeLaunch(AgoGraph * graph, AgoSuperNode * supernode)
{
    // make sure that all input buffers are synched and other arguments are updated
    for (size_t index = 0; index < supernode->dataList.size(); index++) {
        bool need_access = supernode->dataInfo[index].needed_as_a_kernel_argument;
        bool need_read_access = supernode->dataInfo[index].argument_usage[VX_INPUT] || supernode->dataInfo[index].argument_usage[VX_BIDIRECTIONAL];
        if (agoGpuHipDataInputSync(graph, supernode->dataList[index], supernode->dataInfo[index].data_type_flags, supernode->group, need_access, need_read_access) < 0) {
            return -1;
        }
    }
    // call execute kernel
#if 0
    AgoKernel * kernel = supernode->akernel;
    status = VX_SUCCESS;
    hipEventRecord(supernode->hip_event_start, graph->hip_stream0);
    if (kernel->func) {
        status = kernel->func(supernode, ago_kernel_cmd_execute);
        if (status == AGO_ERROR_KERNEL_NOT_IMPLEMENTED)
            status = VX_ERROR_NOT_IMPLEMENTED;
    }
    else if (kernel->kernel_f) {
        status = kernel->kernel_f(supernode, (vx_reference *)supernode->paramList, node->paramCount);
    }
    if (status) {
        agoAddLogEntry((vx_reference)graph, VX_FAILURE, "ERROR: kernel %s exec failed (%d:%s)\n", kernel->name, status, agoEnum2Name(status));
        return status;
    }
    hipEventRecord(supernode->hip_event_stop, graph->hip_stream0);
    if(graph->enable_node_level_opencl_flush) {
        hipError_t err = hipStreamSynchronize(graph->hip_stream0);
        if (err) {
            agoAddLogEntry(&node->ref, VX_FAILURE, "ERROR: hipStreamSynchronize(supernode) failed(%d) for %s\n", err, supernode->akernel->name);
            return -1;
        }
    }
#endif
    // mark that supernode outputs are dirty
    for (size_t index = 0; index < supernode->dataList.size(); index++) {
        if (!(supernode->dataInfo[index].data_type_flags & DATA_HIP_FLAG_DISCARD_PARAM)) {
            bool need_access = supernode->dataInfo[index].needed_as_a_kernel_argument;
            bool need_write_access = supernode->dataInfo[index].argument_usage[VX_OUTPUT] || supernode->dataInfo[index].argument_usage[VX_BIDIRECTIONAL];
            if (agoGpuHipDataOutputMarkDirty(graph, supernode->dataList[index], need_access, need_write_access) < 0) {
                return -1;
            }
        }
    }
    return 0;

}

int agoGpuHipSingleNodeWait(AgoGraph * graph, AgoNode * node)
{
    // wait for completion
    float eventMs = 1.0f;
    hipEventSynchronize(node->hip_event_stop);
    hipEventElapsedTime(&eventMs, node->hip_event_start, node->hip_event_stop);
    graph->opencl_perf.kernel_wait += eventMs;
    // sync the outputs
    for (size_t index = 0; index < node->paramCount; index++) {
        if (node->paramList[index]) {
            bool need_write_access = node->parameters[index].direction != VX_INPUT ? true : false;
            if (need_write_access /*&& node->opencl_param_atomic_mask & (1 << index)*/) {
                if (agoGpuHipDataOutputAtomicSync(graph, node->paramList[index]) < 0) {
                        return -1;
                }
            }
        }
    }

    // The num items in an array should not exceed the capacity unless kernels need it for reporting number of items detected (ex. FAST corners)
    for (size_t index = 0; index < node->paramCount; index++) {
            if (node->paramList[index]) {
                    bool need_write_access = node->parameters[index].direction != VX_INPUT ? true : false;
                    if (need_write_access /*&& node->opencl_param_atomic_mask & (1 << index)*/) {
                            if (node->paramList[index]->ref.type == VX_TYPE_ARRAY) {
                                    node->paramList[index]->u.arr.numitems = min(node->paramList[index]->u.arr.numitems, node->paramList[index]->u.arr.capacity);
                            }
                    }
            }
    }
    return 0;
}

#endif
