//
//
//  Description:
//      Implementation of Dijkstra's Single-Source Shortest Path (SSSP) algorithm on the GPU.
//      The basis of this implementation is the paper:
//
//          "Accelerating large graph algorithms on the GPU using CUDA" by
//          Parwan Harish and P.J. Narayanan
//
//
//  Author:
//      Dan Ginsburg
//
//  Children's Hospital Boston
//  GPL v2
//
#include <float.h>
#include <oclUtils.h>
#include "oclDijkstraKernel.h"

///
//  Types
//

// This structure is used in the multi-GPU implementation of the algorithm.
// This structure defines the workload for each GPU.  The code chunks up
// the work on a per-GPU basis.
typedef struct
{
    // GPU number to run algorithm on
    int device;

    // Pointer to graph data
    GraphData *graph;

    // Source vertex indices to process
    int *sourceVertices;

    // End vertex indices to process
    int *endVertices;

    // Results of processing
    float *outResultCosts;

    // Number of results
    int numResults;

} GPUPlan;


///////////////////////////////////////////////////////////////////////////////
//
//  Private Functions
//
//

///
/// Load and build an OpenCL program from source file
/// \return Handle to the program
cl_program loadAndBuildProgram( cl_context gpuContext, const char *fileName )
{
    size_t programLength;
    cl_int errNum;
    cl_program program;

    // Load the OpenCL source code from the .cl file
    const char* sourcePath = shrFindFilePath( fileName, "oclDijkstra");
    char *source = oclLoadProgSource(sourcePath, "", &programLength);
    shrCheckError(source != NULL, shrTRUE);
    shrLog(LOGBOTH, 0.0, "oclLoadProgSource\n");

    // Create the program for all GPUs in the context
    program = clCreateProgramWithSource(gpuContext, 1, (const char **)&source, &programLength, &errNum);
    shrCheckError(errNum, CL_SUCCESS);
    shrLog(LOGBOTH, 0.0, "clCreateProgramWithSource\n");

    // build the program for all devices on the context
    errNum = clBuildProgram(program, 0, NULL, NULL, NULL, NULL);
    if (errNum != CL_SUCCESS)
    {
        // write out standard error, Build Log and PTX, then cleanup and exit
        shrLog(LOGBOTH | ERRORMSG, (double)errNum, STDERROR);
        oclLogBuildInfo(program, oclGetFirstDev(gpuContext));
        oclLogPtx(program, oclGetFirstDev(gpuContext), "oclDijkstra.ptx");
        shrCheckError(errNum, CL_SUCCESS);
    }
    shrLog(LOGBOTH, 0.0, "clBuildProgram\n");

    return program;
}

///
/// Check whether the mask array is empty.  This tells the algorithm whether
/// it needs to continue running or not.
///
bool maskArrayEmpty(unsigned char *maskArray, int count)
{
    for(int i = 0; i < count; i++ )
    {
        if (maskArray[i] == 1)
        {
            return false;
        }
    }

    return true;
}

///
///  Allocate memory for input CUDA buffers and copy the data into device memory
///
void allocateOCLBuffers(cl_context gpuContext, cl_command_queue commandQueue, GraphData *graph,
                        cl_mem *vertexArrayDevice, cl_mem *edgeArrayDevice, cl_mem *weightArrayDevice,
                        cl_mem *maskArrayDevice, cl_mem *costArrayDevice, cl_mem *updatingCostArrayDevice)
{
    cl_int errNum;
    cl_mem hostVertexArrayBuffer;
    cl_mem hostEdgeArrayBuffer;
    cl_mem hostWeightArrayBuffer;

    // First, need to create OpenCL Host buffers that can be copied to device buffers
    hostVertexArrayBuffer = clCreateBuffer(gpuContext, CL_MEM_COPY_HOST_PTR | CL_MEM_ALLOC_HOST_PTR,
                                           sizeof(int) * graph->vertexCount, graph->vertexArray, &errNum);
    shrCheckError(errNum, CL_SUCCESS);

    hostEdgeArrayBuffer = clCreateBuffer(gpuContext, CL_MEM_COPY_HOST_PTR | CL_MEM_ALLOC_HOST_PTR,
                                           sizeof(int) * graph->edgeCount, graph->edgeArray, &errNum);
    shrCheckError(errNum, CL_SUCCESS);

    hostWeightArrayBuffer = clCreateBuffer(gpuContext, CL_MEM_COPY_HOST_PTR | CL_MEM_ALLOC_HOST_PTR,
                                           sizeof(float) * graph->edgeCount, graph->weightArray, &errNum);
    shrCheckError(errNum, CL_SUCCESS);

    // Now create all of the GPU buffers
    *vertexArrayDevice = clCreateBuffer(gpuContext, CL_MEM_READ_ONLY, sizeof(int) * graph->vertexCount, NULL, &errNum);
    shrCheckError(errNum, CL_SUCCESS);
    *edgeArrayDevice = clCreateBuffer(gpuContext, CL_MEM_READ_ONLY, sizeof(int) * graph->edgeCount, NULL, &errNum);
    shrCheckError(errNum, CL_SUCCESS);
    *weightArrayDevice = clCreateBuffer(gpuContext, CL_MEM_READ_ONLY, sizeof(float) * graph->edgeCount, NULL, &errNum);
    shrCheckError(errNum, CL_SUCCESS);
    *maskArrayDevice = clCreateBuffer(gpuContext, CL_MEM_READ_WRITE, sizeof(unsigned char) * graph->vertexCount, NULL, &errNum);
    shrCheckError(errNum, CL_SUCCESS);
    *costArrayDevice = clCreateBuffer(gpuContext, CL_MEM_READ_WRITE, sizeof(float) * graph->vertexCount, NULL, &errNum);
    shrCheckError(errNum, CL_SUCCESS);
    *updatingCostArrayDevice = clCreateBuffer(gpuContext, CL_MEM_READ_WRITE, sizeof(float) * graph->vertexCount, NULL, &errNum);
    shrCheckError(errNum, CL_SUCCESS);

    // Now queue up the data to be copied to the device
    errNum = clEnqueueCopyBuffer(commandQueue, hostVertexArrayBuffer, *vertexArrayDevice, 0, 0,
                                 sizeof(int) * graph->vertexCount, 0, NULL, NULL);
    shrCheckError(errNum, CL_SUCCESS);
    errNum = clEnqueueCopyBuffer(commandQueue, hostEdgeArrayBuffer, *edgeArrayDevice, 0, 0,
                                 sizeof(int) * graph->edgeCount, 0, NULL, NULL);
    shrCheckError(errNum, CL_SUCCESS);
    errNum = clEnqueueCopyBuffer(commandQueue, hostWeightArrayBuffer, *weightArrayDevice, 0, 0,
                                 sizeof(float) * graph->edgeCount, 0, NULL, NULL);
    shrCheckError(errNum, CL_SUCCESS);

}

///
/// Initialize OpenCL buffers for single run of Dijkstra
///
void initializeOCLBuffers(cl_command_queue commandQueue, cl_kernel initializeKernel, GraphData *graph)
{
    cl_int errNum;
    // Set # of work items in work group and total in 1 dimensional range
    size_t localWorkSize[1] = { 1 };
    size_t globalWorkSize[1];
    cl_event eventDone;

    globalWorkSize[0] = graph->vertexCount;

    errNum = clEnqueueNDRangeKernel(commandQueue, initializeKernel, 1, 0, globalWorkSize, localWorkSize,
                                    0, NULL, &eventDone);
    shrCheckError(errNum, CL_SUCCESS);

    //@TEMP - probably don't need to sync here - remove this once I have everythiung
    //        working
    // Synchronize with the GPUs and do accumulated error check
    clWaitForEvents(1, &eventDone);
    //@TEMP

}

///////////////////////////////////////////////////////////////////////////////
//
//  Public Functions
//
//

///
/// Run Dijkstra's shortest path on the GraphData provided to this function.  This
/// function will compute the shortest path distance from sourceVertices[n] ->
/// endVertices[n] and store the cost in outResultCosts[n].  The number of results
/// it will compute is given by numResults.
///
/// This function will run the algorithm on a single GPU.
///
/// \param graph Structure containing the vertex, edge, and weight arra
///              for the input graph
/// \param startVertices Indices into the vertex array from which to
///                      start the search
/// \param endVertices Indices into the vertex array from which to end
///                    the search.
/// \param outResultsCosts A pre-allocated array where the results for
///                        each shortest path search will be written
/// \param numResults Should be the size of all three passed inarrays
///
void runDijkstra( cl_context gpuContext, GraphData* graph, int *sourceVertices, int *endVertices,
                   float *outResultCosts, int numResults)
{
    cl_device_id deviceId;

    // Grab the highest performance GPU
    deviceId = oclGetMaxFlopsDev( gpuContext );
    oclPrintDevInfo(LOGBOTH, deviceId);

    // Create command queue
    cl_int errNum;
    cl_command_queue commandQueue;
    commandQueue = clCreateCommandQueue( gpuContext, deviceId, 0, &errNum );
    shrCheckError(errNum, CL_SUCCESS);
    shrLog(LOGBOTH, 0.0, "clCreateCommandQueue\n\n");

    // Program handle
    cl_program program = loadAndBuildProgram( gpuContext, "dijkstra.cl" );
    if (program <= 0 )
    {
        return;
    }

    cl_mem vertexArrayDevice;
    cl_mem edgeArrayDevice;
    cl_mem weightArrayDevice;
    cl_mem maskArrayDevice;
    cl_mem costArrayDevice;
    cl_mem updatingCostArrayDevice;

    // Allocate buffers in Device memory
    allocateOCLBuffers( gpuContext, commandQueue, graph, &vertexArrayDevice, &edgeArrayDevice, &weightArrayDevice,
                        &maskArrayDevice, &costArrayDevice, &updatingCostArrayDevice );


    // Create the Kernels
    cl_kernel initializeBuffersKernel;
    initializeBuffersKernel = clCreateKernel(program, "initializeBuffers", &errNum);
    shrCheckError(errNum, CL_SUCCESS);

    // Set the args values and check for errors
    errNum |= clSetKernelArg(initializeBuffersKernel, 0, sizeof(cl_mem), &maskArrayDevice);
    errNum |= clSetKernelArg(initializeBuffersKernel, 1, sizeof(cl_mem), &costArrayDevice);
    errNum |= clSetKernelArg(initializeBuffersKernel, 2, sizeof(cl_mem), &updatingCostArrayDevice);
    shrCheckError(errNum, CL_SUCCESS);

    // Kernel 1
    cl_kernel ssspKernel1;
    ssspKernel1 = clCreateKernel(program, "OCL_SSSP_KERNEL1", &errNum);
    shrCheckError(errNum, CL_SUCCESS);
    errNum |= clSetKernelArg(ssspKernel1, 0, sizeof(cl_mem), &vertexArrayDevice);
    errNum |= clSetKernelArg(ssspKernel1, 1, sizeof(cl_mem), &edgeArrayDevice);
    errNum |= clSetKernelArg(ssspKernel1, 2, sizeof(cl_mem), &weightArrayDevice);
    errNum |= clSetKernelArg(ssspKernel1, 3, sizeof(cl_mem), &maskArrayDevice);
    errNum |= clSetKernelArg(ssspKernel1, 4, sizeof(cl_mem), &costArrayDevice);
    errNum |= clSetKernelArg(ssspKernel1, 5, sizeof(cl_mem), &updatingCostArrayDevice);
    errNum |= clSetKernelArg(ssspKernel1, 6, sizeof(int), &graph->vertexCount);
    errNum |= clSetKernelArg(ssspKernel1, 7, sizeof(int), &graph->edgeCount);
    shrCheckError(errNum, CL_SUCCESS);

    // Kernel 2
    cl_kernel ssspKernel2;
    ssspKernel2 = clCreateKernel(program, "OCL_SSSP_KERNEL2", &errNum);
    shrCheckError(errNum, CL_SUCCESS);
    errNum |= clSetKernelArg(ssspKernel2, 0, sizeof(cl_mem), &vertexArrayDevice);
    errNum |= clSetKernelArg(ssspKernel2, 1, sizeof(cl_mem), &edgeArrayDevice);
    errNum |= clSetKernelArg(ssspKernel2, 2, sizeof(cl_mem), &weightArrayDevice);
    errNum |= clSetKernelArg(ssspKernel2, 3, sizeof(cl_mem), &maskArrayDevice);
    errNum |= clSetKernelArg(ssspKernel2, 4, sizeof(cl_mem), &costArrayDevice);
    errNum |= clSetKernelArg(ssspKernel2, 5, sizeof(cl_mem), &updatingCostArrayDevice);
    shrCheckError(errNum, CL_SUCCESS);


    unsigned char *maskArrayHost = (unsigned char*) malloc(sizeof(unsigned char) * graph->vertexCount);
    cl_mem maskArrayHostBuffer;
    maskArrayHostBuffer = clCreateBuffer(gpuContext, CL_MEM_COPY_HOST_PTR | CL_MEM_ALLOC_HOST_PTR,
                                         sizeof(unsigned char) * graph->vertexCount, maskArrayHost, &errNum);
    shrCheckError(errNum, CL_SUCCESS);


    for ( int i = 0 ; i < numResults; i++ )
    {

        errNum |= clSetKernelArg(initializeBuffersKernel, 3, sizeof(int), &sourceVertices[i]);
        errNum |= clSetKernelArg(ssspKernel2, 6, sizeof(int), &endVertices[i]);
        shrCheckError(errNum, CL_SUCCESS);

        // Initialize mask array to false, C and U to infiniti
        initializeOCLBuffers( commandQueue, initializeBuffersKernel, graph );

        // Read mask array from device -> host
        cl_event readDone;
        errNum = clEnqueueReadBuffer( commandQueue, maskArrayDevice, CL_FALSE, 0, sizeof(unsigned char) * graph->vertexCount,
                                      maskArrayHostBuffer, 0, NULL, &readDone);
        shrCheckError(errNum, CL_SUCCESS);
        clWaitForEvents(1, &readDone);

        while(!maskArrayEmpty(maskArrayHost, graph->vertexCount))
        {
            // Set # of work items in work group and total in 1 dimensional range
            size_t localWorkSize[1] = { 1 };
            size_t globalWorkSize[1];

            globalWorkSize[0] = graph->vertexCount;

            // execute the kernel
            errNum = clEnqueueNDRangeKernel(commandQueue, ssspKernel1, 1, 0, globalWorkSize, localWorkSize,
                                            0, NULL, NULL);
            shrCheckError(errNum, CL_SUCCESS);


            errNum = clEnqueueNDRangeKernel(commandQueue, ssspKernel2, 1, 0, globalWorkSize, localWorkSize,
                                            0, NULL, NULL);
            shrCheckError(errNum, CL_SUCCESS);

            errNum = clEnqueueReadBuffer( commandQueue, maskArrayDevice, CL_FALSE, 0, sizeof(unsigned char) * graph->vertexCount,
                                          maskArrayHostBuffer, 0, NULL, &readDone);
            shrCheckError(errNum, CL_SUCCESS);
            clWaitForEvents(1, &readDone);
        }

        shrLog(LOGBOTH, 0.0, "Iteration: %d\n", i);

//        float result;

        // Copy the result back
//        cutilSafeCall( cudaMemcpy( &result, &costArrayDevice[endVertices[i]], sizeof(float), cudaMemcpyDeviceToHost) );
//        outResultCosts[i] = result;
    }


    free (maskArrayHost);
/*
    // Free all the buffers
    cutilSafeCall(cudaFree(vertexArrayDevice));
    cutilSafeCall(cudaFree(edgeArrayDevice));
    cutilSafeCall(cudaFree(weightArrayDevice));
    cutilSafeCall(cudaFree(maskArrayDevice));
    cutilSafeCall(cudaFree(costArrayDevice));
    cutilSafeCall(cudaFree(updatingCostArrayDevice));
    cutilSafeCall(cudaFree(infinityArrayDevice));
    */
}



///
/// Run Dijkstra's shortest path on the GraphData provided to this function.  This
/// function will compute the shortest path distance from sourceVertices[n] ->
/// endVertices[n] and store the cost in outResultCosts[n].  The number of results
/// it will compute is given by numResults.
///
/// This function will run the algorithm on as many GPUs as is available.  It will
/// create N threads, one for each GPU, and chunk the workload up to perform
/// (numResults / N) searches per GPU.
///
/// \param graph Structure containing the vertex, edge, and weight arra
///              for the input graph
/// \param startVertices Indices into the vertex array from which to
///                      start the search
/// \param endVertices Indices into the vertex array from which to end
///                    the search.
/// \param outResultsCosts A pre-allocated array where the results for
///                        each shortest path search will be written
/// \param numResults Should be the size of all three passed inarrays
///
///

void runDijkstraMultiGPU( cl_context gpuContext, GraphData* graph, int *sourceVertices, int *endVertices,
                          float *outResultCosts, int numResults )
{
    /*
    int numGPUs;

    cutilSafeCall( cudaGetDeviceCount(&numGPUs) );
    printf("CUDA-capable device count: %i\n", numGPUs);

    if (numGPUs == 0)
    {
        // ERORR: no GPUs present!
        return;
    }

    GPUPlan *gpuPlans = (GPUPlan*) malloc(sizeof(GPUPlan) * numGPUs);
    CUTThread *threadIDs = (CUTThread*) malloc(sizeof(CUTThread) * numGPUs);

    // Divide the workload out per GPU
    int resultsPerGPU = numResults / numGPUs;

    int offset = 0;

    for (int i = 0; i < numGPUs; i++)
    {
        gpuPlans[i].device = i;
        gpuPlans[i].graph = graph;
        gpuPlans[i].sourceVertices = &sourceVertices[offset];
        gpuPlans[i].endVertices = &endVertices[offset];
        gpuPlans[i].outResultCosts = &outResultCosts[offset];
        gpuPlans[i].numResults = resultsPerGPU;

        offset += resultsPerGPU;
    }

    // Add any remaining work to the last GPU
    if (offset < numResults)
    {
        gpuPlans[numGPUs - 1].numResults += (numResults - offset);
    }

    // Launch all the threads
    for (int i = 0; i < numGPUs; i++)
    {
        threadIDs[i] = cutStartThread((CUT_THREADROUTINE)dijkstraThread, (void*)(gpuPlans + i));
    }

    // Wait for the results from all threads
    cutWaitForThreads(threadIDs, numGPUs);

    free (gpuPlans);
    free (threadIDs);
    */
}
