/* Meta's space manager C API (libhzos_spaces.meta.so), arm64: reconstructed from that library,
 * the fpHAL library behind it and Meta's JNI library that calls it (libspacemanager_jni.so).
 * The function names are the library's; type and field names are Prism's.
 * Zero-initialize requests and data, then set the version and metadata type explicitly.
 * Destroy all spaces before their manager.
 */
#ifndef PRISM_HZU_SPACES_H
#define PRISM_HZU_SPACES_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
#define HZU_ASSERT static_assert
extern "C" {
#else
#define HZU_ASSERT _Static_assert
#endif

typedef int32_t HzuResult;
typedef int64_t HzuTime; /* System.nanoTime domain, nanoseconds; see spec. */
typedef struct HzuSpaceManagerOpaque *HzuSpaceManager;
typedef struct HzuSpaceOpaque *HzuSpace;
typedef HzuSpace HzuVirtualSpace;
typedef HzuSpace HzuWindowSpace;
typedef int32_t HzuReferenceSpaceType;
enum { HZU_REFERENCE_RAW = 0, HZU_REFERENCE_VIEW = 1 };
enum {
    HZU_SUCCESS = 0, HZU_NO_PARENT = 2,
    HZU_ERROR_GENERIC = -1, HZU_ERROR_PARAMETER = -2,
    HZU_ERROR_SERVICE = -3, HZU_ERROR_VERSION_OR_TYPE = -4,
    HZU_ERROR_QUATERNION = -5, HZU_ERROR_PERMISSION = -7,
    HZU_ERROR_ENUMERATION_REQUIRED = -8, HZU_ERROR_CAPACITY = -9,
    HZU_ERROR_TIMESTAMP = -10
};
enum { HZU_VERSION_LEGACY = 0x55, HZU_VERSION_VIRTUAL_UUID = 0xc9,
       HZU_VERSION_DATA = 0xcb };
enum { HZU_META_VIRTUAL = 1, HZU_META_WINDOW = 2 };
/* Names corroborated by horizonos/perception/spaces/SpaceDataFlags.smali. */
enum { HZU_ORIENTATION_VALID = 1, HZU_POSITION_VALID = 2,
       HZU_ORIENTATION_TRACKED = 4, HZU_POSITION_TRACKED = 8,
       HZU_LINEAR_VELOCITY_VALID = 0x10, HZU_ANGULAR_VELOCITY_VALID = 0x20,
       HZU_POSE_VALID_MASK = 3, HZU_LEGACY_BOUNDS_VALID = 0x40,
       HZU_META_BOUNDS_VALID = 1, HZU_META_ALPHA_VALID = 2 };

/* The bytes in memory are canonical UUID bytes (big-endian Java msb, then lsb).
 * words[] is native little-endian storage, NOT Java UUID long values. */
typedef union HzuSpaceUuid { uint8_t bytes[16]; uint64_t words[2]; } HzuSpaceUuid;
typedef HzuSpaceUuid HzuUpdateToken;
typedef struct HzuVector3f { float x, y, z; } HzuVector3f;
typedef struct HzuQuaternionf { float x, y, z, w; } HzuQuaternionf;
typedef struct HzuPose { HzuQuaternionf orientation; HzuVector3f position; } HzuPose;
typedef struct HzuExtent3f { float width, height, depth; } HzuExtent3f;

typedef struct HzuReferenceSpaceCreateInfo {
    uint32_t version; /* 0x55 */
    HzuReferenceSpaceType type;
} HzuReferenceSpaceCreateInfo;
typedef struct HzuReferenceSpaces {
    uint32_t version; /* 0x55 */
    uint32_t capacity;
    uint32_t count; /* out, also on insufficient capacity */
    uint32_t reserved;
    HzuReferenceSpaceType *types; /* NULL: count query */
} HzuReferenceSpaces;
/* With version 0x55 only the first 16 bytes are read; uuid is ignored.
 * With 0xc9 all 32 bytes are read. Window creation requires 0xcb. */
typedef struct HzuSpaceCreateInfo {
    uint32_t version;
    uint32_t reserved;
    HzuSpace parent; /* C accepts NULL; Java creation requires a parent */
    HzuSpaceUuid uuid;
} HzuSpaceCreateInfo;
typedef struct HzuSpaceFromUuidInfo {
    uint32_t version; /* virtual: 0x55, window: 0xcb */
    uint32_t reserved;
    HzuSpaceUuid uuid;
} HzuSpaceFromUuidInfo;
typedef struct HzuSpaceLocateInfo {
    uint32_t version; /* 0x55 for BOTH locate functions */
    uint32_t reserved;
    HzuSpace space; /* target */
    HzuSpace baseSpace;
    HzuTime time;
} HzuSpaceLocateInfo;

/* Shared 128-byte external storage. SpaceData uses version 0x55, BaseSpaceData
 * uses 0xcb. legacyBounds is an old field retained in the newer layout.
 * Field names corroborated by the older Java SpaceData parcelable; storage
 * offsets are proved by native copies and interpolation/extrapolation.
 */
typedef struct HzuBaseSpaceData {
    uint32_t version;
    uint32_t reserved0;
    HzuTime time;
    HzuSpaceUuid odometryUuid; /* not the token from getUpdateToken */
    uint32_t flags;
    HzuExtent3f legacyBounds;
    HzuPose pose;
    HzuVector3f linearVelocity;
    HzuVector3f angularVelocity;
    HzuVector3f linearAcceleration;
    HzuVector3f angularAcceleration;
    uint32_t reservedTail;
} HzuBaseSpaceData;
typedef HzuBaseSpaceData HzuSpaceData;
typedef struct HzuVirtualSpaceData {
    uint32_t version; /* 0xcb */
    uint32_t type; /* 1 */
    uint32_t flags;
    HzuExtent3f bounds;
} HzuVirtualSpaceData;
typedef struct HzuWindowSpaceData {
    uint32_t version; /* 0xcb */
    uint32_t type; /* 2 */
    uint32_t flags;
    HzuExtent3f bounds;
    float alpha;
} HzuWindowSpaceData;
/* locateSpace2 may overwrite type. Allocate the largest alternative even
 * when the target is virtual. Layout is the flattened tagged union. */
typedef HzuWindowSpaceData HzuSpaceMetaData;

extern HzuResult HzuSpaceManager_create(HzuSpaceManager *outManager);
extern void HzuSpaceManager_destroy(HzuSpaceManager manager);
extern HzuResult HzuSpaceManager_enumerateReferenceSpaces(HzuSpaceManager manager, HzuReferenceSpaces *inOut);
extern HzuResult HzuSpaceManager_createReferenceSpace(HzuSpaceManager manager, const HzuReferenceSpaceCreateInfo *info, HzuSpace *outSpace);
extern HzuResult HzuSpaceManager_createVirtualSpace(HzuSpaceManager manager, const HzuSpaceCreateInfo *info, HzuVirtualSpace *outSpace);
extern HzuResult HzuSpaceManager_createVirtualSpaceFromUuid(HzuSpaceManager manager, const HzuSpaceFromUuidInfo *info, HzuVirtualSpace *outSpace);
extern HzuResult HzuSpaceManager_createWindowSpace(HzuSpaceManager manager, const HzuSpaceCreateInfo *info, HzuWindowSpace *outSpace);
extern HzuResult HzuSpaceManager_createWindowSpaceFromUuid(HzuSpaceManager manager, const HzuSpaceFromUuidInfo *info, HzuWindowSpace *outSpace);
extern HzuResult HzuSpaceManager_locateSpace(HzuSpaceManager manager, const HzuSpaceLocateInfo *info, HzuSpaceData *outData);
extern HzuResult HzuSpaceManager_locateSpace2(HzuSpaceManager manager, const HzuSpaceLocateInfo *info, HzuBaseSpaceData *outData, HzuSpaceMetaData *inOutMeta /* nullable */);
extern void HzuSpace_destroy(HzuSpace space);
extern HzuResult HzuSpace_getUuid(HzuSpace space, HzuSpaceUuid *outUuid);
extern HzuResult HzuSpace_getParentUuid(HzuSpace space, HzuSpaceUuid *outUuid);
extern HzuResult HzuSpace_update(HzuSpace space, const HzuSpaceData *data);
extern HzuResult HzuVirtualSpace_update(HzuVirtualSpace space, const HzuBaseSpaceData *baseData, const HzuVirtualSpaceData *data);
extern HzuResult HzuVirtualSpace_getUpdateToken(HzuVirtualSpace space, HzuUpdateToken *outToken);
extern HzuResult HzuWindowSpace_update(HzuWindowSpace space, const HzuBaseSpaceData *baseData, const HzuWindowSpaceData *data);
extern HzuResult HzuWindowSpace_getUpdateToken(HzuWindowSpace space, HzuUpdateToken *outToken);

HZU_ASSERT(sizeof(void *) == 8, "LP64 pointer ABI required");
HZU_ASSERT(sizeof(float) == 4, "binary32 required");
#define HZU_SIZE(T,N) HZU_ASSERT(sizeof(T)==(N), #T " size")
#define HZU_OFFSET(T,F,N) HZU_ASSERT(offsetof(T,F)==(N), #T "." #F " offset")
HZU_SIZE(HzuSpaceUuid,16); HZU_SIZE(HzuUpdateToken,16);
HZU_SIZE(HzuVector3f,12); HZU_SIZE(HzuQuaternionf,16); HZU_SIZE(HzuPose,28);
HZU_OFFSET(HzuQuaternionf,w,12); HZU_OFFSET(HzuPose,position,16);
HZU_SIZE(HzuExtent3f,12); HZU_OFFSET(HzuExtent3f,depth,8);
HZU_SIZE(HzuReferenceSpaceCreateInfo,8); HZU_OFFSET(HzuReferenceSpaceCreateInfo,type,4);
HZU_SIZE(HzuReferenceSpaces,24); HZU_OFFSET(HzuReferenceSpaces,capacity,4);
HZU_OFFSET(HzuReferenceSpaces,count,8); HZU_OFFSET(HzuReferenceSpaces,types,16);
HZU_SIZE(HzuSpaceCreateInfo,32); HZU_OFFSET(HzuSpaceCreateInfo,parent,8);
HZU_OFFSET(HzuSpaceCreateInfo,uuid,16);
HZU_SIZE(HzuSpaceFromUuidInfo,24); HZU_OFFSET(HzuSpaceFromUuidInfo,uuid,8);
HZU_SIZE(HzuSpaceLocateInfo,32); HZU_OFFSET(HzuSpaceLocateInfo,space,8);
HZU_OFFSET(HzuSpaceLocateInfo,baseSpace,16); HZU_OFFSET(HzuSpaceLocateInfo,time,24);
HZU_SIZE(HzuBaseSpaceData,128); HZU_SIZE(HzuSpaceData,128);
HZU_OFFSET(HzuBaseSpaceData,time,8); HZU_OFFSET(HzuBaseSpaceData,odometryUuid,16);
HZU_OFFSET(HzuBaseSpaceData,flags,32); HZU_OFFSET(HzuBaseSpaceData,legacyBounds,36);
HZU_OFFSET(HzuBaseSpaceData,pose,48); HZU_OFFSET(HzuBaseSpaceData,linearVelocity,76);
HZU_OFFSET(HzuBaseSpaceData,angularVelocity,88); HZU_OFFSET(HzuBaseSpaceData,linearAcceleration,100);
HZU_OFFSET(HzuBaseSpaceData,angularAcceleration,112); HZU_OFFSET(HzuBaseSpaceData,reservedTail,124);
HZU_SIZE(HzuVirtualSpaceData,24); HZU_OFFSET(HzuVirtualSpaceData,bounds,12);
HZU_SIZE(HzuWindowSpaceData,28); HZU_SIZE(HzuSpaceMetaData,28);
HZU_OFFSET(HzuWindowSpaceData,type,4); HZU_OFFSET(HzuWindowSpaceData,flags,8);
HZU_OFFSET(HzuWindowSpaceData,bounds,12); HZU_OFFSET(HzuWindowSpaceData,alpha,24);
#undef HZU_SIZE
#undef HZU_OFFSET
#ifdef __cplusplus
}
#endif
#undef HZU_ASSERT
#endif
