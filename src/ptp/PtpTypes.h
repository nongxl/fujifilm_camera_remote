#ifndef PTP_TYPES_H
#define PTP_TYPES_H

#include <stdint.h>
#include <stddef.h>

// PTP/IP Packet Types
enum PtpPacketType : uint32_t {
    PTP_PKT_INIT_CMD_REQ    = 1,
    PTP_PKT_INIT_CMD_ACK    = 2,
    PTP_PKT_INIT_EVENT_REQ  = 3,
    PTP_PKT_INIT_EVENT_ACK  = 4,
    PTP_PKT_INIT_FAIL       = 5,
    PTP_PKT_OPER_REQ        = 6,
    PTP_PKT_OPER_RESP       = 7,
    PTP_PKT_EVENT           = 8,
    PTP_PKT_START_DATA      = 9,
    PTP_PKT_DATA            = 10,
    PTP_PKT_CANCEL_REQ      = 11,
    PTP_PKT_END_DATA        = 12,
    PTP_PKT_PROBE_REQ       = 13,
    PTP_PKT_PROBE_RESP      = 14
};

// PTP Standard Operation Codes
enum PtpOpCode : uint16_t {
    PTP_OC_Undefined            = 0x1000,
    PTP_OC_GetDeviceInfo        = 0x1001,
    PTP_OC_OpenSession          = 0x1002,
    PTP_OC_CloseSession         = 0x1003,
    PTP_OC_GetStorageIDs        = 0x1004,
    PTP_OC_GetStorageInfo       = 0x1005,
    PTP_OC_GetNumObjects        = 0x1006,
    PTP_OC_GetObjectHandles     = 0x1007,
    PTP_OC_GetObjectInfo        = 0x1008,
    PTP_OC_GetObject            = 0x1009,
    PTP_OC_GetThumb             = 0x100A,
    PTP_OC_DeleteObject         = 0x100B,
    PTP_OC_SendObjectInfo       = 0x100C,
    PTP_OC_SendObject           = 0x100D,
    PTP_OC_InitiateCapture      = 0x100E,
    PTP_OC_GetDevicePropDesc    = 0x1014,
    PTP_OC_GetDevicePropValue   = 0x1015,
    PTP_OC_SetDevicePropValue   = 0x1016,
    PTP_OC_ResetDevicePropValue = 0x1017,
    PTP_OC_TerminateOpenCapture = 0x1018,
    PTP_OC_InitiateOpenCapture  = 0x101C,
    
    // Fuji Vendor OpCodes
    FUJI_OC_GetRegistrationStatus   = 0x9022,
    FUJI_OC_InitiateCapture         = 0x9028,
    FUJI_OC_StartLiveView           = 0x902B,
    FUJI_OC_GetLiveViewFrame        = 0x902C,
    FUJI_OC_StopLiveView            = 0x902D,
    FUJI_OC_GetDeviceProps          = 0x902E,
    FUJI_OC_SetDevicePropValueEx    = 0x902F
};

// PTP Response Codes
enum PtpRespCode : uint16_t {
    PTP_RC_Undefined                = 0x2000,
    PTP_RC_OK                       = 0x2001,
    PTP_RC_GeneralError             = 0x2002,
    PTP_RC_SessionNotOpen           = 0x2003,
    PTP_RC_InvalidTransactionID     = 0x2004,
    PTP_RC_OperationNotSupported    = 0x2005,
    PTP_RC_ParameterNotSupported    = 0x2006,
    PTP_RC_IncompleteTransfer       = 0x2007,
    PTP_RC_InvalidStorageId         = 0x2008,
    PTP_RC_InvalidObjectHandle      = 0x2009,
    PTP_RC_DevicePropNotSupported   = 0x200A,
    PTP_RC_InvalidObjectFormatCode  = 0x200B,
    PTP_RC_StoreFull                = 0x200C,
    PTP_RC_ObjectWriteProtected     = 0x200D,
    PTP_RC_StoreReadOnly            = 0x200E,
    PTP_RC_AccessDenied             = 0x200F,
    PTP_RC_NoThumbnailPresent       = 0x2010,
    PTP_RC_SelfTestFailed           = 0x2011,
    PTP_RC_PartialDeletion          = 0x2012,
    PTP_RC_StoreNotAvailable        = 0x2013,
    PTP_RC_SpecificationByFormatUnsupported = 0x2014,
    PTP_RC_NoValidObjectInfo        = 0x2015,
    PTP_RC_InvalidCodeFormat        = 0x2016,
    PTP_RC_UnknownVendorCode        = 0x2017,
    PTP_RC_CaptureAlreadyTerminated = 0x2018,
    PTP_RC_DeviceBusy               = 0x2019,
    PTP_RC_InvalidParentObject      = 0x201A,
    PTP_RC_InvalidDevicePropFormat  = 0x201B,
    PTP_RC_InvalidDevicePropValue   = 0x201C,
    PTP_RC_InvalidParameter         = 0x201D,
    PTP_RC_SessionAlreadyOpened     = 0x201E,
    PTP_RC_TransactionCanceled      = 0x201F,
    PTP_RC_SpecificationOfDestinationUnsupported = 0x2020
};

// Data Phase flags for Operation Request
enum PtpDataPhase : uint32_t {
    PTP_DATA_PHASE_NONE = 0,
    PTP_DATA_PHASE_OUT  = 1, // Client to device
    PTP_DATA_PHASE_IN   = 2  // Device to client
};

#pragma pack(push, 1)
struct PtpIpHeader {
    uint32_t length;
    uint32_t type;
};
#pragma pack(pop)

#endif // PTP_TYPES_H
