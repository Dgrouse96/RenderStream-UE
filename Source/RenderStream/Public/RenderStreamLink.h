#pragma once

#include <stdint.h>

struct ID3D11Resource;
struct ID3D12Resource;
struct ID3D12Fence;

class RenderStreamLink
{
public:
    enum RSPixelFormat : uint32_t
    {
        RS_FMT_INVALID,

        RS_FMT_BGRA8,
        RS_FMT_BGRX8,

        RS_FMT_RGBA32F,
    };

    enum RS_ERROR
    {
        RS_ERROR_SUCCESS = 0,

        // Core is not initialised
        RS_NOT_INITIALISED,

        // Core is already initialised
        RS_ERROR_ALREADYINITIALISED,

        // Given handle is invalid
        RS_ERROR_INVALIDHANDLE,

        // Maximum number of frame senders have been created
        RS_MAXSENDERSREACHED,

        RS_ERROR_BADSTREAMTYPE,

        RS_ERROR_NOTFOUND,

        RS_ERROR_INCORRECTSCHEMA,

        RS_ERROR_INVALID_PARAMETERS,

        RS_ERROR_BUFFER_OVERFLOW,

        RS_ERROR_TIMEOUT,

        RS_ERROR_STREAMS_CHANGED,

        RS_ERROR_INCOMPATIBLE_VERSION,

        RS_ERROR_UNSPECIFIED
    };

    // Bitmask flags
    enum FRAMEDATA_FLAGS
    {
        FRAMEDATA_NO_FLAGS = 0,
        FRAMEDATA_RESET = 1
    };

    typedef uint64_t StreamHandle;
    typedef uint64_t CameraHandle;
    typedef void (*logger_t)(const char*);

#pragma pack(push, 4)
    typedef struct
    {
        float virtualZoomScale;
        unsigned char virtualReprojectionRequired;
        float xRealCamera, yRealCamera, zRealCamera;
        float rxRealCamera, ryRealCamera, rzRealCamera;
    } D3TrackingData;  // Tracking data required by d3 but not used to render content

    typedef struct
    {
        StreamHandle id;
        CameraHandle cameraHandle;
        float x, y, z;
        float rx, ry, rz;
        float focalLength;
        float sensorX, sensorY;
        float cx, cy;
        float nearZ, farZ;
        D3TrackingData d3Tracking;
    } CameraData;

    typedef struct
    {
        double tTracked;
        double localTime;
        double localTimeDelta;
        unsigned int frameRateNumerator;
        unsigned int frameRateDenominator;
        uint32_t flags;
        uint32_t scene;
    } FrameData;

    typedef struct
    {
        double tTracked;
        CameraData camera;
    } CameraResponseData;

    typedef struct
    {
        uint8_t* data;
        uint32_t stride;
    } HostMemoryData;

    typedef struct
    {
        ID3D11Resource* resource;
    } Dx11Data;

    typedef struct
    {
        ID3D12Resource* resource;
        ID3D12Fence* fence;
        int32_t fenceValue;
    } Dx12Data;

    typedef union
    {
        HostMemoryData cpu;
        Dx11Data dx11;
        Dx12Data dx12;
    } SenderFrameTypeData;

    typedef struct
    {
        uint32_t xOffset;
        uint32_t yOffset;
        uint32_t width;
        uint32_t height;
    } FrameRegion;

    // Normalised (0-1) clipping planes for the edges of the camera frustum, to be used to perform off-axis perspective projection, or
    // to offset and scale 2D orthographic matrices.
    typedef struct
    {
        float left;
        float right;
        float top;
        float bottom;
    } ProjectionClipping;

    typedef struct
    {
        StreamHandle handle;
        const char* channel;
        const char* name;
        uint32_t width;
        uint32_t height;
        RSPixelFormat format;
        ProjectionClipping clipping;
    } StreamDescription;

    typedef struct
    {
        uint32_t nStreams;
        StreamDescription* streams;
    } StreamDescriptions;

    typedef struct
    {
        const char* group;
        const char* displayName;
        const char* key;
        float min;
        float max;
        float step;
        float defaultValue;
        uint32_t nOptions;
        const char** options;

        int32_t dmxOffset;
        uint32_t dmxType;
    } RemoteParameter;

    typedef struct
    {
        const char* name;
        uint32_t nParameters;
        RemoteParameter* parameters;
        uint64_t hash;
    } RemoteParameters;

    typedef struct
    {
        uint32_t nScenes;
        RemoteParameters* scenes;
    } Scenes;

    typedef struct
    {
        uint32_t nChannels;
        const char** channels;
    } Channels;

    typedef struct
    {
        Channels channels;
        Scenes scenes;
    } Schema;

    typedef struct
    {
        const char* name;
        float value;
    } ProfilingEntry;

#pragma pack(pop)

#define RENDER_STREAM_VERSION_MAJOR 1
#define RENDER_STREAM_VERSION_MINOR 23

    enum SenderFrameType
    {
        RS_FRAMETYPE_HOST_MEMORY,
        RS_FRAMETYPE_DX11_TEXTURE,
        RS_FRAMETYPE_DX12_TEXTURE
    };

    RENDERSTREAM_API static RenderStreamLink& instance();

private:
    RenderStreamLink();
    ~RenderStreamLink();

private:
    typedef void (*logger_t)(const char*);

    typedef void rs_registerLoggingFuncFn(logger_t);
    typedef void rs_registerErrorLoggingFuncFn(logger_t);
    typedef void rs_registerVerboseLoggingFuncFn(logger_t);

    typedef void rs_unregisterLoggingFuncFn();
    typedef void rs_unregisterErrorLoggingFuncFn();
    typedef void rs_unregisterVerboseLoggingFuncFn();

    typedef RS_ERROR rs_initialiseFn(int expectedVersionMajor, int expectedVersionMinor);
    typedef RS_ERROR rs_shutdownFn();
    // non-isolated functions, these require init prior to use
    typedef RS_ERROR rs_saveSchemaFn(const char* assetPath, Schema* schema); // Save schema for project file/custom executable at (assetPath)
    typedef RS_ERROR rs_loadSchemaFn(const char* assetPath, /*Out*/Schema* schema, /*InOut*/uint32_t* nBytes); // Load schema for project file/custom executable at (assetPath) into a buffer of size (nBytes) starting at (schema)
    // workload functions, these require the process to be running inside d3's asset launcher environment
    typedef RS_ERROR rs_setSchemaFn(/*InOut*/Schema* schema); // Set schema and fill in per-scene hash for use with rs_getFrameParameters
    typedef RS_ERROR rs_getStreamsFn(/*Out*/StreamDescriptions* streams, /*InOut*/uint32_t* nBytes); // Populate streams into a buffer of size (nBytes) starting at (streams)

    typedef RS_ERROR rs_setFollowerFn(int isFollower); // Used to mark this node as relying on alternative mechanisms to distribute FrameData. Users must provide correct CameraResponseData to sendFrame, and call rs_beginFollowerFrame at the start of the frame, where awaitFrame would normally be called.
    typedef RS_ERROR rs_beginFollowerFrameFn(double tTracked); // Pass the engine-distributed tTracked value in, if you have called rs_setFollower(1) otherwise do not call this function.
    typedef RS_ERROR rs_awaitFrameDataFn(int timeoutMs, /*Out*/FrameData * data);

    typedef RS_ERROR rs_sendFrameFn(StreamHandle streamHandle, SenderFrameType frameType, SenderFrameTypeData data, const CameraResponseData* sendData);
    typedef RS_ERROR rs_getFrameParametersFn(uint64_t schemaHash, /*Out*/void* outParameterData, size_t outParameterDataSize); 
    typedef RS_ERROR rs_getFrameCameraFn(StreamHandle streamHandle, /*Out*/CameraData* outCameraData);
    typedef RS_ERROR rs_logToD3Fn(const char * str);
    typedef RS_ERROR rs_sendProfilingDataFn(ProfilingEntry* entries, int count);
    typedef RS_ERROR rs_setNewStatusMessageFn(const char* msg);

public:
    RENDERSTREAM_API bool isAvailable();

    bool loadExplicit();
    bool unloadExplicit();

    struct ScopedSchema
    {
        ScopedSchema()
        {
            clear();
        }
        ~ScopedSchema()
        {
            reset();
        }
        void reset()
        {
            for (size_t i = 0; i < schema.channels.nChannels; ++i)
                free(const_cast<char*>(schema.channels.channels[i]));
            free(schema.channels.channels);
            for (size_t i = 0; i < schema.scenes.nScenes; ++i)
            {
                RemoteParameters& scene = schema.scenes.scenes[i];
                free(const_cast<char*>(scene.name));
                for (size_t j = 0; j < scene.nParameters; ++j)
                {
                    RemoteParameter& parameter = scene.parameters[j];
                    free(const_cast<char*>(parameter.group));
                    free(const_cast<char*>(parameter.displayName));
                    free(const_cast<char*>(parameter.key));
                    for (size_t k = 0; k < parameter.nOptions; ++k)
                    {
                        free(const_cast<char*>(parameter.options[k]));
                    }
                    free(parameter.options);
                }
                free(scene.parameters);
            }
            free(schema.scenes.scenes);
            clear();
        }

        ScopedSchema(const ScopedSchema&) = delete;
        ScopedSchema(ScopedSchema&& other)
        {
            schema = std::move(other.schema);
            other.reset();
        }
        ScopedSchema& operator=(const ScopedSchema&) = delete;
        ScopedSchema& operator=(ScopedSchema&& other)
        {
            schema = std::move(other.schema);
            other.reset();
            return *this;
        }

        Schema schema;

    private:
        void clear()
        {
            schema.channels.nChannels = 0;
            schema.channels.channels = nullptr;
            schema.scenes.nScenes = 0;
            schema.scenes.scenes = nullptr;
        }
    };

public: // d3renderstream.h API, but loaded dynamically.
    rs_registerLoggingFuncFn* rs_registerLoggingFunc = nullptr;
    rs_registerErrorLoggingFuncFn* rs_registerErrorLoggingFunc = nullptr;
    rs_registerVerboseLoggingFuncFn* rs_registerVerboseLoggingFunc = nullptr;

    rs_unregisterLoggingFuncFn* rs_unregisterLoggingFunc = nullptr;
    rs_unregisterErrorLoggingFuncFn* rs_unregisterErrorLoggingFunc = nullptr;
    rs_unregisterVerboseLoggingFuncFn* rs_unregisterVerboseLoggingFunc = nullptr;

    rs_initialiseFn* rs_initialise = nullptr;
    rs_setSchemaFn* rs_setSchema = nullptr;
    rs_saveSchemaFn* rs_saveSchema = nullptr;
    rs_loadSchemaFn* rs_loadSchema = nullptr;
    rs_shutdownFn* rs_shutdown = nullptr;
    rs_getStreamsFn* rs_getStreams = nullptr;
    rs_sendFrameFn* rs_sendFrame = nullptr;
    rs_setFollowerFn* rs_setFollower = nullptr;
    rs_beginFollowerFrameFn* rs_beginFollowerFrame = nullptr;
    rs_awaitFrameDataFn* rs_awaitFrameData = nullptr;
    rs_getFrameParametersFn* rs_getFrameParameters = nullptr;
    rs_getFrameCameraFn* rs_getFrameCamera = nullptr;
    rs_logToD3Fn* rs_logToD3 = nullptr;
    rs_sendProfilingDataFn* rs_sendProfilingData = nullptr;
    rs_setNewStatusMessageFn* rs_setNewStatusMessage = nullptr;

private:
    bool m_loaded = false;
    void* m_dll = nullptr;
};

//template<typename Fn>
//bool RenderStreamLink::LoadFunc(Fn*& func, const FString& name)
