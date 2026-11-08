/* Names of NTSTATUS codes, for messages. */
#include "status.h"

const char *RtlStatusName(NTSTATUS s)
{
    switch ((uint32_t)s) {
    case 0x00000000: return "STATUS_SUCCESS";
    case 0x00000080: return "STATUS_ABANDONED_WAIT_0";
    case 0x00000102: return "STATUS_TIMEOUT";
    case 0x00000103: return "STATUS_PENDING";
    case 0x40000000: return "STATUS_OBJECT_NAME_EXISTS";
    case 0xC0000001: return "STATUS_UNSUCCESSFUL";
    case 0xC0000002: return "STATUS_NOT_IMPLEMENTED";
    case 0xC0000008: return "STATUS_INVALID_HANDLE";
    case 0xC000000D: return "STATUS_INVALID_PARAMETER";
    case 0xC0000017: return "STATUS_NO_MEMORY";
    case 0xC0000022: return "STATUS_ACCESS_DENIED";
    case 0xC0000023: return "STATUS_BUFFER_TOO_SMALL";
    case 0xC0000024: return "STATUS_OBJECT_TYPE_MISMATCH";
    case 0xC0000033: return "STATUS_OBJECT_NAME_INVALID";
    case 0xC0000034: return "STATUS_OBJECT_NAME_NOT_FOUND";
    case 0xC0000035: return "STATUS_OBJECT_NAME_COLLISION";
    case 0xC000003A: return "STATUS_OBJECT_PATH_NOT_FOUND";
    case 0xC0000046: return "STATUS_MUTANT_NOT_OWNED";
    case 0xC0000047: return "STATUS_SEMAPHORE_LIMIT_EXCEEDED";
    case 0xC000009A: return "STATUS_INSUFFICIENT_RESOURCES";
    case 0xC00000BB: return "STATUS_NOT_SUPPORTED";
    case 0xC0000120: return "STATUS_CANCELLED";
    case 0xC000027F: return "STATUS_REPARSE_LOOP";
    default:         return "STATUS_?";
    }
}
