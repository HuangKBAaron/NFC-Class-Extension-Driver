/*++

Copyright (c) Microsoft Corporation.  All Rights Reserved

Module Name:

    NfcCxSC.cpp

Abstract:

    SC Interface implementation

Environment:

    User-mode Driver Framework

--*/

#include "NfcCxPch.h"

#include "NfcCxSC.tmh"

// TODO: Temporarily define this here until we get ScDeviceEnum.h published
// {D6B5B883-18BD-4B4D-B2EC-9E38AFFEDA82}, 2, DEVPROP_TYPE_BYTE
DEFINE_DEVPROPKEY(DEVPKEY_Device_ReaderKind,
    0xD6B5B883, 0x18BD, 0x4B4D, 0xB2, 0xEC, 0x9E, 0x38, 0xAF, 0xFE, 0xDA, 0x82, 0x02);

typedef struct _NFCCX_SC_DISPATCH_ENTRY {
    ULONG IoControlCode;
    BOOLEAN fPowerManaged;
    BOOLEAN fSequentialDispatch;
    size_t MinimumInputBufferLength;
    size_t MinimumOutputBufferLength;
    PFN_NFCCX_SC_DISPATCH_HANDLER DispatchHandler;
} NFCCX_SC_DISPATCH_ENTRY, *PNFCCX_SC_DISPATCH_ENTRY;

typedef struct _ATTRIBUTE_DISPATCH_ENTRY {
    DWORD dwAttributeId;
    PBYTE pbResultBuffer;
    size_t cbResultBuffer;
    BOOL fRequireLock;
    PFN_NFCCX_SC_ATTRIBUTE_DISPATCH_HANDLER pfnDispatchHandler;
} ATTRIBUTE_DISPATCH_ENTRY, *PATTRIBUTE_DISPATCH_ENTRY;

static const CHAR SCReaderVendorName[] = "Microsoft";
static const CHAR SCReaderVendorIfd[] = "IFD";
static const DWORD SCReaderVendorIfdVersion = ((IFD_MAJOR_VER & 0x3) << 6) | ((IFD_MINOR_VER & 0x3) << 4) | (IFD_BUILD_NUM & 0xF);
static const DWORD SCReaderChannelId = SCARD_READER_TYPE_NFC << 16;
static const DWORD SCReaderProtocolTypes = SCARD_PROTOCOL_T1;
static const DWORD SCReaderDeviceUnit = 0;
static const DWORD SCReaderDefaultClk = 13560;
static const DWORD SCReaderMaxClk = 13560;
static const DWORD SCReaderDefaultDataRate = 1;
static const DWORD SCReaderMaxDataRate = 1;
static const DWORD SCReaderMaxIfsd = 254;
static const DWORD SCReaderCharacteristics = SCARD_READER_CONTACTLESS;
static const DWORD SCReaderCurrentProtocolType = SCARD_PROTOCOL_T1;
static const DWORD SCReaderCurrentClk = 13560;
static const DWORD SCReaderCurrentD = 1;
static const DWORD SCReaderCurrentIfsc = 32;
static const DWORD SCReaderCurrentIfsd = 254;
static const DWORD SCReaderCurrentBwt = 4;

NFCCX_SC_DISPATCH_ENTRY 
g_ScDispatch [] = {
    { IOCTL_SMARTCARD_GET_ATTRIBUTE,        TRUE,   FALSE,  sizeof(DWORD),                     sizeof(BYTE),                     NfcCxSCInterfaceDispatchGetAttribute },
    { IOCTL_SMARTCARD_SET_ATTRIBUTE,        FALSE,  FALSE,  sizeof(DWORD),                     0,                                NfcCxSCInterfaceDispatchSetAttribute },
    { IOCTL_SMARTCARD_GET_STATE,            FALSE,  FALSE,  0,                                 sizeof(DWORD),                    NfcCxSCInterfaceDispatchGetState },
    { IOCTL_SMARTCARD_POWER,                FALSE,  FALSE,  sizeof(DWORD),                     0,                                NfcCxSCInterfaceDispatchSetPower },
    { IOCTL_SMARTCARD_SET_PROTOCOL,         FALSE,  FALSE,  sizeof(DWORD),                     sizeof(DWORD),                    NfcCxSCInterfaceDispatchSetProtocol },
    { IOCTL_SMARTCARD_IS_ABSENT,            FALSE,  FALSE,  0,                                 0,                                NfcCxSCInterfaceDispatchIsAbsent },
    { IOCTL_SMARTCARD_IS_PRESENT,           FALSE,  FALSE,  0,                                 0,                                NfcCxSCInterfaceDispatchIsPresent },
    // IOCTL_SMARTCARD_TRANSMIT is expecting input to have at least one byte and IO_REQUEST and output to be SW1 + SW2 + IO_REQUEST
    { IOCTL_SMARTCARD_TRANSMIT,             TRUE,   TRUE,   sizeof(SCARD_IO_REQUEST)+1,        sizeof(SCARD_IO_REQUEST)+2,       NfcCxSCInterfaceDispatchTransmit },
    { IOCTL_SMARTCARD_EJECT,                FALSE,  FALSE,  0,                                 0,                                NfcCxSCInterfaceDispatchNotSupported },
    { IOCTL_SMARTCARD_GET_LAST_ERROR,       FALSE,  FALSE,  0,                                 sizeof(DWORD),                    NfcCxSCInterfaceDispatchGetLastError },
    { IOCTL_SMARTCARD_SWALLOW,              FALSE,  FALSE,  0,                                 0,                                NfcCxSCInterfaceDispatchNotSupported },
    { IOCTL_SMARTCARD_CONFISCATE,           FALSE,  FALSE,  0,                                 0,                                NfcCxSCInterfaceDispatchNotSupported },
    { IOCTL_SMARTCARD_GET_PERF_CNTR,        FALSE,  FALSE,  0,                                 0,                                NfcCxSCInterfaceDispatchNotSupported },
};

ATTRIBUTE_DISPATCH_ENTRY
g_ScAttributeDispatch [] = {
    { SCARD_ATTR_VENDOR_NAME,               (PBYTE)SCReaderVendorName,           sizeof(SCReaderVendorName),             TRUE,  &NfcCxSCInterfaceDispatchAttributeLocked },
    { SCARD_ATTR_VENDOR_IFD_TYPE,           (PBYTE)SCReaderVendorIfd,            sizeof(SCReaderVendorIfd),              TRUE,  &NfcCxSCInterfaceDispatchAttributeLocked },
    { SCARD_ATTR_VENDOR_IFD_VERSION,        (PBYTE)&SCReaderVendorIfdVersion,    sizeof(SCReaderVendorIfdVersion),       TRUE,  &NfcCxSCInterfaceDispatchAttributeLocked },
    { SCARD_ATTR_CHANNEL_ID,                (PBYTE)&SCReaderChannelId,           sizeof(SCReaderChannelId),              TRUE,  &NfcCxSCInterfaceDispatchAttributeLocked },
    { SCARD_ATTR_PROTOCOL_TYPES,            (PBYTE)&SCReaderProtocolTypes,       sizeof(SCReaderProtocolTypes),          TRUE,  &NfcCxSCInterfaceDispatchAttributeLocked },
    { SCARD_ATTR_DEVICE_UNIT,               (PBYTE)&SCReaderDeviceUnit,          sizeof(SCReaderDeviceUnit),             TRUE,  &NfcCxSCInterfaceDispatchAttributeLocked },
    { SCARD_ATTR_DEFAULT_CLK,               (PBYTE)&SCReaderDefaultClk,          sizeof(SCReaderDefaultClk),             TRUE,  &NfcCxSCInterfaceDispatchAttributeLocked },
    { SCARD_ATTR_MAX_CLK,                   (PBYTE)&SCReaderMaxClk,              sizeof(SCReaderMaxClk),                 TRUE,  &NfcCxSCInterfaceDispatchAttributeLocked },
    { SCARD_ATTR_DEFAULT_DATA_RATE,         (PBYTE)&SCReaderDefaultDataRate,     sizeof(SCReaderDefaultDataRate),        TRUE,  &NfcCxSCInterfaceDispatchAttributeLocked },
    { SCARD_ATTR_MAX_DATA_RATE,             (PBYTE)&SCReaderMaxDataRate,         sizeof(SCReaderMaxDataRate),            TRUE,  &NfcCxSCInterfaceDispatchAttributeLocked },
    { SCARD_ATTR_MAX_IFSD,                  (PBYTE)&SCReaderMaxIfsd,             sizeof(SCReaderMaxIfsd),                TRUE,  &NfcCxSCInterfaceDispatchAttributeLocked },
    { SCARD_ATTR_CHARACTERISTICS,           (PBYTE)&SCReaderCharacteristics,     sizeof(SCReaderCharacteristics),        TRUE,  &NfcCxSCInterfaceDispatchAttributeLocked },
    { SCARD_ATTR_CURRENT_CLK,               (PBYTE)&SCReaderCurrentClk,          sizeof(SCReaderCurrentClk),             TRUE,  &NfcCxSCInterfaceDispatchAttributeLocked },
    { SCARD_ATTR_CURRENT_D,                 (PBYTE)&SCReaderCurrentD,            sizeof(SCReaderCurrentD),               TRUE,  &NfcCxSCInterfaceDispatchAttributeLocked },
    { SCARD_ATTR_CURRENT_IFSC,              (PBYTE)&SCReaderCurrentIfsc,         sizeof(SCReaderCurrentIfsc),            TRUE,  &NfcCxSCInterfaceDispatchAttributeLocked },
    { SCARD_ATTR_CURRENT_IFSD,              (PBYTE)&SCReaderCurrentIfsd,         sizeof(SCReaderCurrentIfsd),            TRUE,  &NfcCxSCInterfaceDispatchAttributeLocked },
    { SCARD_ATTR_CURRENT_BWT,               (PBYTE)&SCReaderCurrentBwt,          sizeof(SCReaderCurrentBwt),             TRUE,  &NfcCxSCInterfaceDispatchAttributeLocked },
    { SCARD_ATTR_CURRENT_PROTOCOL_TYPE,     (PBYTE)&SCReaderCurrentProtocolType, sizeof(SCReaderCurrentProtocolType),    TRUE,  &NfcCxSCInterfaceDispatchAttributeCurrentProtocolTypeLocked },
    { SCARD_ATTR_ICC_PRESENCE,              NULL,                                0,                                      TRUE,  &NfcCxSCInterfaceDispatchAttributePresentLocked },
    { SCARD_ATTR_ATR_STRING,                NULL,                                0,                                      FALSE, &NfcCxSCInterfaceDispatchAttributeAtr },
    { SCARD_ATTR_ICC_TYPE_PER_ATR,          NULL,                                0,                                      TRUE,  &NfcCxSCInterfaceDispatchAttributeIccTypeLocked },
};

NTSTATUS
NfcCxSCInterfaceCreate(
    _In_ PNFCCX_FDO_CONTEXT DeviceContext,
    _Out_ PNFCCX_SC_INTERFACE * PPSCInterface
    )
/*++

Routine Description:

    This routine creates and initalizes the SmartCard Reader Interface.

Arguments:

    DeviceContext - A pointer to the FdoContext
    PPSCInterface - A pointer to a memory location to receive the allocated SmartCard Reader interface

Return Value:

    NTSTATUS

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    PNFCCX_SC_INTERFACE scInterface = NULL;
    WDF_OBJECT_ATTRIBUTES objectAttrib;
    WDF_IO_QUEUE_CONFIG queueConfig;

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);
    
    scInterface = (PNFCCX_SC_INTERFACE)malloc(sizeof((*scInterface)));
    if (NULL == scInterface) {
        TRACE_LINE(LEVEL_ERROR, "Failed to allocate the SC interface");
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }

    RtlZeroMemory(scInterface, sizeof(*scInterface));
    scInterface->FdoContext = DeviceContext;

    //
    // Setup the locks and manual IO queue
    //
    WDF_OBJECT_ATTRIBUTES_INIT(&objectAttrib);
    objectAttrib.ParentObject = scInterface->FdoContext->Device;

    status = WdfWaitLockCreate(&objectAttrib,
                                &scInterface->SmartCardLock);
    if (!NT_SUCCESS(status)) {
        TRACE_LINE(LEVEL_ERROR, "Failed to create the SmartCard WaitLock, %!STATUS!", status);
        goto Done;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&objectAttrib);
    objectAttrib.ParentObject = scInterface->FdoContext->Device;

    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig,
                             WdfIoQueueDispatchParallel);

    queueConfig.PowerManaged = WdfFalse;
    queueConfig.EvtIoDeviceControl = NfcCxSCInterfaceSequentialIoDispatch;
    queueConfig.Settings.Parallel.NumberOfPresentedRequests = 1;

    status = WdfIoQueueCreate(scInterface->FdoContext->Device,
                              &queueConfig,
                              &objectAttrib,
                              &scInterface->SerialIoQueue);
    if (!NT_SUCCESS(status)) {
        TRACE_LINE(LEVEL_ERROR, "Failed WdfIoQueueCreate, %!STATUS!", status);
        goto Done;
    }

    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    queueConfig.PowerManaged = WdfFalse;

    status = WdfIoQueueCreate(scInterface->FdoContext->Device,
                              &queueConfig,
                              &objectAttrib,
                              &scInterface->PresentQueue);
    if (!NT_SUCCESS(status)) {
        TRACE_LINE(LEVEL_ERROR, "Failed to create the SmartCard Present IO Queue, %!STATUS!", status);
        goto Done;
    }

    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    queueConfig.PowerManaged = WdfFalse;

    status = WdfIoQueueCreate(scInterface->FdoContext->Device,
                              &queueConfig,
                              &objectAttrib,
                              &scInterface->AbsentQueue);
    if (!NT_SUCCESS(status)) {
        TRACE_LINE(LEVEL_ERROR, "Failed to create the SmartCard Absent IO Queue, %!STATUS!", status);
        goto Done;
    }

Done:

    if (!NT_SUCCESS(status)) {
        if (NULL != scInterface) {
            NfcCxSCInterfaceDestroy(scInterface);
            scInterface = NULL;
        }
    }

    *PPSCInterface = scInterface;

    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);
    return status;
}

VOID
NfcCxSCInterfaceDestroy(
    _In_ PNFCCX_SC_INTERFACE ScInterface
    )
/*++

Routine Description:

    This routine cleans up the SmartCard Reader Interface

Arguments:

    Interface - A pointer to the SCInterface to cleanup.

Return Value:

    None

--*/
{
    DECLARE_CONST_UNICODE_STRING(nfcScReaderReference, SMARTCARD_READER_NAMESPACE);

    //
    // Since the lock and queue objects are parented to the device,
    // there are no needs to manually delete them here
    //
    if (ScInterface->InterfaceCreated) {
        //
        // Disable the SmartCard Reader interface
        //
        WdfDeviceSetDeviceInterfaceState(ScInterface->FdoContext->Device,
                                         &GUID_DEVINTERFACE_SMARTCARD_READER,
                                         &nfcScReaderReference,
                                         FALSE);

        TRACE_LINE(LEVEL_VERBOSE, "SmartCard Reader interface disabled");
    }

    if (NULL != ScInterface->SmartCardLock) {
        WdfWaitLockAcquire(ScInterface->SmartCardLock, NULL);

        if (NULL != ScInterface->StorageCard) {
            ScInterface->StorageCard->Release();
            ScInterface->StorageCard = NULL;
        }

        if (NULL != ScInterface->StorageCardKey) {
            delete ScInterface->StorageCardKey;
            ScInterface->StorageCardKey = NULL;
        }

        WdfWaitLockRelease(ScInterface->SmartCardLock);
    }

    free(ScInterface);
}

NTSTATUS
NfcCxSCInterfaceStart(
    _In_ PNFCCX_SC_INTERFACE ScInterface
    )
/*++

Routine Description:

    Start the SmartCard Reader Interface

Arguments:

    ScInterface - The SmartCard Reader Interface

Return Value:

    NTSTATUS

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    PNFCCX_FDO_CONTEXT fdoContext = ScInterface->FdoContext;
    WDF_DEVICE_INTERFACE_PROPERTY_DATA nfcScReaderData;
    BYTE readerKind = 0;
    DECLARE_CONST_UNICODE_STRING(nfcScReaderReference, SMARTCARD_READER_NAMESPACE);

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    if (fdoContext->NfpRadioState) {
        //
        // Publish the NFC smart card reader interface
        //
        if (!ScInterface->InterfaceCreated) {
            status = WdfDeviceCreateDeviceInterface(fdoContext->Device,
                                                    &GUID_DEVINTERFACE_SMARTCARD_READER,
                                                    &nfcScReaderReference);
            if (!NT_SUCCESS(status)) {
                TRACE_LINE(LEVEL_ERROR, "Failed to create the NFC smart card reader device interface, %!STATUS!", status);
                goto Done;
            }

            WDF_DEVICE_INTERFACE_PROPERTY_DATA_INIT(&nfcScReaderData,
                                                    &GUID_DEVINTERFACE_SMARTCARD_READER,
                                                    &DEVPKEY_Device_ReaderKind);
            nfcScReaderData.ReferenceString = &nfcScReaderReference;
            readerKind = ABI::Windows::Devices::SmartCards::SmartCardReaderKind_Nfc;
            status = WdfDeviceAssignInterfaceProperty(fdoContext->Device,
                                                      &nfcScReaderData,
                                                      DEVPROP_TYPE_BYTE,
                                                      sizeof(readerKind),
                                                      &readerKind);
            if (!NT_SUCCESS(status)) {
                TRACE_LINE(LEVEL_ERROR, "Failed to assign property for the NFC smart card reader device interface, %!STATUS!", status);
                goto Done;
            }

            ScInterface->InterfaceCreated = TRUE;
        }

        WdfDeviceSetDeviceInterfaceState(fdoContext->Device,
                                         &GUID_DEVINTERFACE_SMARTCARD_READER,
                                         &nfcScReaderReference,
                                         TRUE);
    }

Done:
    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);

    return status;
}

NTSTATUS
NfcCxSCInterfaceStop(
    _In_ PNFCCX_SC_INTERFACE ScInterface
    )
/*++

Routine Description:

    Stop the SmartCard Reader Interface

Arguments:

    SCInterface - The SmartCard Reader Interface

Return Value:

    NTSTATUS

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    DECLARE_CONST_UNICODE_STRING(nfcScReaderReference, SMARTCARD_READER_NAMESPACE);

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    if (ScInterface->InterfaceCreated) {
        WdfDeviceSetDeviceInterfaceState(ScInterface->FdoContext->Device,
                                         &GUID_DEVINTERFACE_SMARTCARD_READER,
                                         &nfcScReaderReference,
                                         FALSE);
    }

    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);

    return status;
}

BOOLEAN 
NfcCxSCInterfaceIsIoctlSupported(
    _In_ PNFCCX_FDO_CONTEXT FdoContext,
    _In_ ULONG IoControlCode
    )
/*++

Routine Description:

    This routine returns true if the provided IOCTL is supported by the
    module.
    
Arguments:

    FdoContext - The FDO Context
    IoControlCode - The IOCTL code to check

Return Value:

    TRUE - The IOCTL is supported by this module
    FALSE - The IOCTL is not supported by this module

--*/
{
    ULONG i;

    UNREFERENCED_PARAMETER(FdoContext);

    for (i=0; i < ARRAYSIZE(g_ScDispatch); i++) {
        if (g_ScDispatch[i].IoControlCode == IoControlCode) {
            return TRUE;
        }
    }

    return FALSE;
}

BOOLEAN
NfcCxSCIsPowerManagedRequest(
    _In_ ULONG IoControlCode
    )
/*++

Routine Description:

    This routine returns true if the provided IOCTL is power managed.
    
Arguments:

    FdoContext - The FDO Context
    IoControlCode - The IOCTL code to check

Return Value:

    TRUE - The IOCTL is power managed
    FALSE - The IOCTL is not power managed

--*/
{
    ULONG i;

    for (i=0; i < ARRAYSIZE(g_ScDispatch); i++) {
        if (g_ScDispatch[i].IoControlCode == IoControlCode) {
            return g_ScDispatch[i].fPowerManaged;
        }
    }

    return FALSE;
}

BOOLEAN
NfcCxSCIsSequentialDispatchRequest(
    _In_ ULONG IoControlCode
    )
/*++

Routine Description:

    This routine returns true if the provided IOCTL requires
    sequential dispatch.
    
Arguments:

    IoControlCode - The IOCTL code to check

Return Value:

    TRUE - The IOCTL requires sequential dispatch
    FALSE - The IOCTL doesn't requires sequential dispatch

--*/
{
    ULONG i;

    for (i=0; i < ARRAYSIZE(g_ScDispatch); i++) {
        if (g_ScDispatch[i].IoControlCode == IoControlCode) {
            return g_ScDispatch[i].fSequentialDispatch;
        }
    }

    return FALSE;
}

VOID
NfcCxSCInterfaceSequentialIoDispatch(
    _In_ WDFQUEUE      Queue,
    _In_ WDFREQUEST    Request,
    _In_ size_t        OutputBufferLength,
    _In_ size_t        InputBufferLength,
    _In_ ULONG         IoControlCode
    )
/*++

Routine Description:

    This is the sequential dispatch IoControl handler for the SC module.

Arguments:

    Queue -  Handle to the framework queue object that is associated with the I/O request.
    Request - Handle to a framework request object.
    OutputBufferLength - Length of the output buffer associated with the request.
    InputBufferLength - Length of the input buffer associated with the request.
    IoControlCode - IOCTL code.

Return Value:

  None.

--*/
{
    NTSTATUS  status = STATUS_SUCCESS;
    PNFCCX_FILE_CONTEXT fileContext;

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    UNREFERENCED_PARAMETER(Queue);

    fileContext = NfcCxFileGetContext(WdfRequestGetFileObject(Request));

    status = NfcCxSCInterfaceIoDispatch(fileContext,
                                        Request,
                                        OutputBufferLength,
                                        InputBufferLength,
                                        IoControlCode);

    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
    } else if (STATUS_SUCCESS == status) {
        WdfRequestComplete(Request, status);
    } else {
        //
        // At this point we know the NT_SUCCESS macro was satisfied, 
        // this means that one of the dispatch should have completed
        // the request already.
        //
        NT_ASSERT(STATUS_PENDING == status);
    }

    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);
}

NTSTATUS 
NfcCxSCInterfaceIoDispatch(
    _In_opt_ PNFCCX_FILE_CONTEXT FileContext,
    _In_ WDFREQUEST    Request,
    _In_ size_t        OutputBufferLength,
    _In_ size_t        InputBufferLength,
    _In_ ULONG         IoControlCode
    )
/*++

Routine Description:

    This is the first entry into the SCInterface.  It validates and dispatches SmartCard request
    as appropriate.

Arguments:

    FileContext - The File context.
    Request - Handle to a framework request object.
    OutputBufferLength - Length of the output buffer associated with the request.
    InputBufferLength - Length of the input buffer associated with the request.
    IoControlCode - IOCTL code.

Return Value:

    NTSTATUS.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    PNFCCX_FDO_CONTEXT fdoContext = NULL;
    WDFMEMORY outMem = NULL;
    WDFMEMORY inMem = NULL;
    PVOID     inBuffer = NULL;
    PVOID     outBuffer = NULL;
    size_t    sizeInBuffer = 0;
    size_t    sizeOutBuffer = 0;
    BOOLEAN   releasePowerReference = FALSE;

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    fdoContext = NfcCxFileObjectGetFdoContext(FileContext);

    //
    // Is the NFP radio enabled. SmartCard sharing the same radio control with NFP
    //
    if (FALSE == NfcCxPowerIsAllowedNfp(fdoContext)) {
        TRACE_LINE(LEVEL_ERROR, "NFP radio is off");
        status = STATUS_DEVICE_POWERED_OFF;
        goto Done;
    }

    //
    // Forward the request to sequential dispatch IO queue
    //
    if (NfcCxSCIsSequentialDispatchRequest(IoControlCode) &&
        fdoContext->SCInterface->SerialIoQueue != WdfRequestGetIoQueue(Request)) {
        status = WdfRequestForwardToIoQueue(Request,
                                            fdoContext->SCInterface->SerialIoQueue);
        if (!NT_SUCCESS(status)) {
            TRACE_LINE(LEVEL_ERROR, "Failed WdfRequestForwardToIoQueue, %!STATUS!", status);
            goto Done;
        }

        status = STATUS_PENDING;
        goto Done;
    }

    //
    // Take a power reference if the request is power managed
    //
    if (NfcCxSCIsPowerManagedRequest(IoControlCode)) {
        status = WdfDeviceStopIdle(fdoContext->Device,
                                   TRUE);
        if (!NT_SUCCESS(status)) {
            TRACE_LINE(LEVEL_ERROR, "Failed WdfDeviceStopIdle, %!STATUS!", status);
            goto Done;
        }

        releasePowerReference = TRUE;
    }

    //
    // Get the request memory and perform the operation here
    //
    if (0 != OutputBufferLength) {
        status = WdfRequestRetrieveOutputMemory(Request, &outMem);
        if (!NT_SUCCESS(status)) {
            TRACE_LINE(LEVEL_ERROR, "Failed to retrieve the output buffer, %!STATUS!", status);
            goto Done;
        }

        outBuffer = WdfMemoryGetBuffer(outMem, &sizeOutBuffer);
        NT_ASSERT(sizeOutBuffer == OutputBufferLength);
        NT_ASSERT(NULL != outBuffer);
    }

    if (0 != InputBufferLength) {
        status = WdfRequestRetrieveInputMemory(Request, &inMem);
        if (!NT_SUCCESS(status)) {
            TRACE_LINE(LEVEL_ERROR, "Failed to retrieve the input buffer, %!STATUS!", status);
            goto Done;
        }

        inBuffer = WdfMemoryGetBuffer(inMem, &sizeInBuffer);
        NT_ASSERT(sizeInBuffer == InputBufferLength);
        NT_ASSERT(NULL != inBuffer);
    }

    //
    // Validate the request
    //
    status = NfcCxSCInterfaceValidateRequest(IoControlCode,
                                             InputBufferLength,
                                             OutputBufferLength);
    if (!NT_SUCCESS(status)) {
        TRACE_LINE(LEVEL_ERROR, "Request validation failed, %!STATUS!", status);
        goto Done;
    }

    //
    // Dispatch the request
    //
    status = NfcCxSCInterfaceDispatchRequest(FileContext,
                                             Request,
                                             IoControlCode,
                                             inBuffer,
                                             InputBufferLength,
                                             outBuffer,
                                             OutputBufferLength);
    if (!NT_SUCCESS(status)) {
        TRACE_LINE(LEVEL_ERROR, "Request dispatch failed, %!STATUS!", status);
        goto Done;
    }

Done:
    if (releasePowerReference) {
        WdfDeviceResumeIdle(fdoContext->Device);
        releasePowerReference = FALSE;
    }

    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);
    return status;
}

NTSTATUS
NfcCxSCInterfaceValidateRequest(
    _In_ ULONG        IoControlCode,
    _In_ size_t       InputBufferLength,
    _In_ size_t       OutputBufferLength
    )
/*++

Routine Description:

    This routine validates the SmartCard request.

Arguments:

    IoControlCode - IOCTL code.
    InputBufferLength - Length of the input buffer associated with the request.
    OutputBufferLength - Length of the output buffer associated with the request.

Return Value:

  NTSTATUS.

--*/
{
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    USHORT i = 0;

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    for (i = 0; i < ARRAYSIZE(g_ScDispatch); i++) {
        if (g_ScDispatch[i].IoControlCode == IoControlCode) {
            
            if (g_ScDispatch[i].MinimumInputBufferLength > InputBufferLength) {
                TRACE_LINE(LEVEL_ERROR, "Invalid Input buffer.  Expected %I64x, got %I64x",
                    g_ScDispatch[i].MinimumInputBufferLength,
                    InputBufferLength);
                status = STATUS_INVALID_PARAMETER;
                goto Done;
            }

            if (g_ScDispatch[i].MinimumOutputBufferLength > OutputBufferLength) {
                TRACE_LINE(LEVEL_ERROR, "Invalid Output buffer.  Expected %I64x, got %I64x",
                    g_ScDispatch[i].MinimumOutputBufferLength,
                    OutputBufferLength);
                status = STATUS_INVALID_PARAMETER;
                goto Done;
            }

            status = STATUS_SUCCESS;
            break;
        }
    }

Done:

    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);
    return status;
}

NTSTATUS
NfcCxSCInterfaceDispatchRequest(
    _In_ PNFCCX_FILE_CONTEXT FileContext,
    _In_ WDFREQUEST Request,
    _In_ ULONG IoControlCode,
    _In_opt_bytecount_(InputBufferLength) PVOID InputBuffer,
    _In_ size_t InputBufferLength,
    _Out_opt_bytecap_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength
    )
/*++

Routine Description:

    This routine dispatches the SmartCard request to the appropriate handler.

Arguments:

    FileContext - The File context
    Request - Handle to a framework request object.
    IoControlCode - The Io Control Code of the Nfp Request.
    InputBuffer - The input buffer
    InputBufferLength - Length of the input buffer.
    OutputBufferLength - Length of the output buffer associated with the request.
    OuptutBuffer - The output buffer

Return Value:

  NTSTATUS.

--*/
{
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    USHORT i = 0;
    WDFDEVICE device = WdfFileObjectGetDevice(FileContext->FileObject);

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    for (i = 0; i < ARRAYSIZE(g_ScDispatch); i++) {
        if (g_ScDispatch[i].IoControlCode == IoControlCode) {

            status = g_ScDispatch[i].DispatchHandler(device,
                                                     Request,
                                                     InputBuffer,
                                                     InputBufferLength,
                                                     OutputBuffer,
                                                     OutputBufferLength);
            break;
        }
    }

    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);

    return status;
}

NTSTATUS
NfcCxSCInterfaceAddClient(
    _In_ PNFCCX_SC_INTERFACE ScInterface,
    _In_ PNFCCX_FILE_CONTEXT FileContext
    )
/*++

Routine Description:

    This routine holds the reference of the client context

Arguments:

    ScInterface - A pointer to the SCInterface
    FileContext - Client to add
    
Return Value:

    NTSTATUS

--*/
{
    NTSTATUS status = STATUS_SUCCESS;

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    WdfWaitLockAcquire(ScInterface->SmartCardLock, NULL);

    if (ScInterface->CurrentClient != NULL) {
        TRACE_LINE(LEVEL_ERROR, "There is existing file handle on the SmartCard");
        status = STATUS_ACCESS_DENIED;
    }
    else {
        ScInterface->CurrentClient = FileContext;
        TRACE_LINE(LEVEL_INFO, "SmartCard client = %p added", FileContext);
    }

    WdfWaitLockRelease(ScInterface->SmartCardLock);

    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);

    return status;
}

VOID
NfcCxSCInterfaceRemoveClient(
    _In_ PNFCCX_SC_INTERFACE ScInterface,
    _In_ PNFCCX_FILE_CONTEXT FileContext
    )
/*++

Routine Description:

    This routine release the reference  of the client

Arguments:

    ScInterface - A pointer to the SCInterface
    
Return Value:

    VOID

--*/
{
    BOOLEAN fResetCard = FALSE;

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    WdfWaitLockAcquire(ScInterface->SmartCardLock, NULL);

    if (ScInterface->CurrentClient != FileContext) {
        WdfWaitLockRelease(ScInterface->SmartCardLock);
        goto Done;
    }

    if (ScInterface->SessionEstablished &&
        !ScInterface->FdoContext->RFInterface->pLibNfcContext->bIsTagNdefFormatted) {
        //
        // We need to reset the smart card if the application has established a 
        // session with the smart card to clear any authentication or state information
        //
        fResetCard = TRUE;
    }

    ScInterface->CurrentClient = NULL;
    TRACE_LINE(LEVEL_INFO, "SmartCard client = %p removed", FileContext);

    WdfWaitLockRelease(ScInterface->SmartCardLock);

    if (fResetCard) {
        NfcCxSCInterfaceResetCard(ScInterface);
    }

Done:

    TRACE_FUNCTION_EXIT(LEVEL_VERBOSE);
}

//
// Handling methods below
//

VOID
NfcCxSCInterfaceHandleSmartCardConnectionEstablished(
    _In_ PNFCCX_SC_INTERFACE ScInterface,
    _In_ phNfc_sRemoteDevInformation_t* pRemoteDeviceInfo
    )
/*++

Routine Description:

    This routine distribute The SmartCardConnectionEstablished event

Arguments:

    ScInterface - A pointer to the SCInterface
    pRemoteDeviceInfo - The remote device info structure
    
Return Value:

    VOID

--*/
{
    NTSTATUS status = STATUS_SUCCESS;

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    switch (pRemoteDeviceInfo->RemDevType)
    {
        case phLibNfc_eISO14443_4A_PICC:
        case phLibNfc_eISO14443_4B_PICC:
        case phLibNfc_eJewel_PICC:
        case phLibNfc_eMifare_PICC:
        case phLibNfc_eFelica_PICC:
        case phLibNfc_eISO15693_PICC:
            break;

        default:
            TRACE_LINE(LEVEL_WARNING, "Unsupported SmartCard type %!phNfc_eRFDevType_t!", pRemoteDeviceInfo->RemDevType);
            goto Done;
    }

    TRACE_LINE(LEVEL_INFO, "SmartCardConnectionEstablished!!!");

    WdfWaitLockAcquire(ScInterface->SmartCardLock, NULL);

    ScInterface->SmartCardConnected = TRUE;
    RtlCopyMemory(&ScInterface->RemoteDeviceInfo, pRemoteDeviceInfo, sizeof(phNfc_sRemoteDevInformation_t));
    status = NfcCxSCInterfaceLoadStorageClassFromAtrLocked(ScInterface);

    WdfWaitLockRelease(ScInterface->SmartCardLock);

    if (NT_SUCCESS(status)) {
        NfcCxSCInterfaceDistributePresentAbsentEvent(ScInterface, ScInterface->PresentQueue);
    }

Done:
    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);
}

VOID
NfcCxSCInterfaceHandleSmartCardConnectionLost(
    _In_ PNFCCX_SC_INTERFACE ScInterface
    )
/*++

Routine Description:

    This routine distribute The SmartCardConnectionLost event

Arguments:

    ScInterface - A pointer to the SCInterface

Return Value:

    VOID

--*/
{
    NTSTATUS status = STATUS_SUCCESS;

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    WdfWaitLockAcquire(ScInterface->SmartCardLock, NULL);

    if (!ScInterface->SmartCardConnected) {
        WdfWaitLockRelease(ScInterface->SmartCardLock);
        TRACE_LINE(LEVEL_INFO, "SmartCard is not connected, no action taken in connection lost!");
        goto Done;
    }

    TRACE_LINE(LEVEL_INFO, "SmartCardConnectionLost!!!");

    ScInterface->SmartCardConnected = FALSE;
    ScInterface->SessionEstablished = FALSE;

    RtlZeroMemory(&ScInterface->RemoteDeviceInfo, sizeof(phNfc_sRemoteDevInformation_t));

    if (NULL != ScInterface->StorageCard) {
        ScInterface->StorageCard->Release();
        ScInterface->StorageCard = NULL;
    }

    if (NULL != ScInterface->StorageCardKey) {
        delete ScInterface->StorageCardKey;
        ScInterface->StorageCardKey = NULL;
    }

    WdfWaitLockRelease(ScInterface->SmartCardLock);

    NfcCxSCInterfaceDistributePresentAbsentEvent(ScInterface, ScInterface->AbsentQueue);

Done:
    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);
}

BOOL
NfcCxSCInterfaceCheckIfDriverDiscoveryEnabled(
    _In_ PNFCCX_SC_INTERFACE ScInterface
    )
/*++

Routine Description:

    This routine determines if there is a smartcard discovery client available

Arguments:

    ScInterface - A pointer to the SCInterface
    
Return Value:

    TRUE - A discovery client is active
    FALSE - A discovery client isn't active
    
--*/
{
    BOOL fEnabled = FALSE;

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    if (!ScInterface->FdoContext->NfpRadioState) {
        goto Done;
    }

    WdfWaitLockAcquire(ScInterface->SmartCardLock, NULL);
    fEnabled = (ScInterface->CurrentClient != NULL) ? TRUE : FALSE;
    WdfWaitLockRelease(ScInterface->SmartCardLock);

Done:

    TRACE_FUNCTION_EXIT(LEVEL_VERBOSE);

    return fEnabled;
}

//
// Dispatched methods below
//

NTSTATUS
NfcCxSCInterfaceDispatchGetAttribute(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_opt_bytecount_(InputBufferLength) PVOID InputBuffer,
    _In_ size_t InputBufferLength,
    _Out_opt_bytecap_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength
    )
/*++

Routine Description:

    This routine dispatches the SmartCard Get Attribute

Arguments:

    Device - Handle to a framework device object.
    Request - Handle to a framework request object.
    InputBuffer - The input buffer
    InputBufferLength - Length of the input buffer.
    OutputBufferLength - Length of the output buffer associated with the request.
    OuptutBuffer - The output buffer

Return Value:

  NTSTATUS.

--*/
{
    NTSTATUS status = STATUS_NOT_SUPPORTED;
    DWORD *pdwAttributeId = (DWORD*)InputBuffer;
    size_t cbOutputBuffer = OutputBufferLength;
    PNFCCX_SC_INTERFACE scInterface;

    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(InputBufferLength);

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    //
    // Buffer validated by the validation method
    //
    _Analysis_assume_(sizeof(BYTE) <= OutputBufferLength);


    scInterface = NfcCxFdoGetContext(Device)->SCInterface;

    for (USHORT TableEntry = 0; TableEntry < ARRAYSIZE(g_ScAttributeDispatch); TableEntry++) {
        if (g_ScAttributeDispatch[TableEntry].dwAttributeId == *pdwAttributeId) {

            if (g_ScAttributeDispatch[TableEntry].fRequireLock) {
                WdfWaitLockAcquire(scInterface->SmartCardLock, NULL);
            }

            status = (*g_ScAttributeDispatch[TableEntry].pfnDispatchHandler)(
                            scInterface,
                            g_ScAttributeDispatch[TableEntry].pbResultBuffer,
                            g_ScAttributeDispatch[TableEntry].cbResultBuffer,
                            (PBYTE)OutputBuffer,
                            &cbOutputBuffer);

            if (g_ScAttributeDispatch[TableEntry].fRequireLock) {
                WdfWaitLockRelease(scInterface->SmartCardLock);
            }

            break;
        }
    }

    if (NT_SUCCESS(status)) {
        TRACE_LINE(LEVEL_INFO,
            "Completing request %p, with %!STATUS!, 0x%I64x", Request, status, cbOutputBuffer);
        WdfRequestCompleteWithInformation(Request, status, cbOutputBuffer);
        //
        // Since we have completed the request here,
        // return STATUS_PENDING to avoid double completion of the request
        //
        status = STATUS_PENDING;
    }

    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);
    TRACE_LOG_NTSTATUS_ON_FAILURE(status);

    return status;
}

NTSTATUS
NfcCxSCInterfaceDispatchSetAttribute(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_opt_bytecount_(InputBufferLength) PVOID InputBuffer,
    _In_ size_t InputBufferLength,
    _Out_opt_bytecap_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength
    )
/*++

Routine Description:

    This routine dispatches the SmartCard Set Attribute

Arguments:

    Device - Handle to a framework device object.
    Request - Handle to a framework request object.
    InputBuffer - The input buffer
    InputBufferLength - Length of the input buffer.
    OutputBufferLength - Length of the output buffer associated with the request.
    OuptutBuffer - The output buffer

Return Value:

  NTSTATUS.

--*/
{
    NTSTATUS status = STATUS_NOT_SUPPORTED;
    DWORD *pdwAttributeId = (DWORD*)InputBuffer;

    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBuffer);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    //
    // Buffer validated by the validation method
    //
    _Analysis_assume_(sizeof(DWORD) <= InputBufferLength);

    if (*pdwAttributeId == SCARD_ATTR_DEVICE_IN_USE) {
        status = STATUS_SUCCESS;
    }

    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);
    TRACE_LOG_NTSTATUS_ON_FAILURE(status);
    
    return status;
}

NTSTATUS
NfcCxSCInterfaceDispatchGetState(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_opt_bytecount_(InputBufferLength) PVOID InputBuffer,
    _In_ size_t InputBufferLength,
    _Out_opt_bytecap_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength
    )
/*++

Routine Description:

    This routine dispatches the SmartCard Get State

Arguments:

    Device - Handle to a framework device object.
    Request - Handle to a framework request object.
    InputBuffer - The input buffer
    InputBufferLength - Length of the input buffer.
    OutputBufferLength - Length of the output buffer associated with the request.
    OuptutBuffer - The output buffer

Return Value:

  NTSTATUS.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    PNFCCX_SC_INTERFACE scInterface;
    DWORD *pdwState = (DWORD*)OutputBuffer;

    UNREFERENCED_PARAMETER(InputBuffer);
    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    //
    // Buffer validated by the validation method
    //
    _Analysis_assume_(sizeof(DWORD) <= OutputBufferLength);

    scInterface = NfcCxFdoGetContext(Device)->SCInterface;

    WdfWaitLockAcquire(scInterface->SmartCardLock, NULL);

    if (scInterface->SmartCardConnected) {
        *pdwState = SCARD_SPECIFIC;
    }
    else {
        *pdwState = SCARD_ABSENT;
    }

    WdfWaitLockRelease(scInterface->SmartCardLock);

    if (NT_SUCCESS(status)) {
        TRACE_LINE(LEVEL_INFO,
            "Completing request %p, with %!STATUS!, 0x%I64x", Request, status, sizeof(DWORD));
        WdfRequestCompleteWithInformation(Request, status, sizeof(DWORD));
        //
        // Since we have completed the request here, 
        // return STATUS_PENDING to avoid double completion of the request
        //
        status = STATUS_PENDING;
    }

    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);
    TRACE_LOG_NTSTATUS_ON_FAILURE(status);
    
    return status;
}

NTSTATUS
NfcCxSCInterfaceDispatchSetPower(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_opt_bytecount_(InputBufferLength) PVOID InputBuffer,
    _In_ size_t InputBufferLength,
    _Out_opt_bytecap_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength
    )
/*++

Routine Description:

    This routine dispatches the SmartCard Set Power

Arguments:

    Device - Handle to a framework device object.
    Request - Handle to a framework request object.
    InputBuffer - The input buffer
    InputBufferLength - Length of the input buffer.
    OutputBufferLength - Length of the output buffer associated with the request.
    OuptutBuffer - The output buffer

Return Value:

  NTSTATUS.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    PNFCCX_SC_INTERFACE scInterface;
    DWORD *pdwPower = (DWORD*)InputBuffer;

    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBuffer);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    //
    // Buffer validated by the validation method
    //
    _Analysis_assume_(sizeof(DWORD) <= InputBufferLength);

    scInterface = NfcCxFdoGetContext(Device)->SCInterface;

    WdfWaitLockAcquire(scInterface->SmartCardLock, NULL);

    if (!scInterface->SmartCardConnected) {
        status = STATUS_NO_MEDIA;
        WdfWaitLockRelease(scInterface->SmartCardLock);
        goto Done;
    }

    WdfWaitLockRelease(scInterface->SmartCardLock);

    switch (*pdwPower)
    {
    case SCARD_COLD_RESET:
    case SCARD_WARM_RESET:
        NfcCxSCInterfaceResetCard(scInterface);
        break;

    case SCARD_POWER_DOWN:
        status = STATUS_NOT_SUPPORTED;
        break;

    default:
        status = STATUS_INVALID_PARAMETER;
        break;
    }

Done:
    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);
    TRACE_LOG_NTSTATUS_ON_FAILURE(status);
    
    return status;
}

NTSTATUS
NfcCxSCInterfaceDispatchSetProtocol(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_opt_bytecount_(InputBufferLength) PVOID InputBuffer,
    _In_ size_t InputBufferLength,
    _Out_opt_bytecap_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength
    )
/*++

Routine Description:

    This routine dispatches the SmartCard Set Protocol

Arguments:

    Device - Handle to a framework device object.
    Request - Handle to a framework request object.
    InputBuffer - The input buffer
    InputBufferLength - Length of the input buffer.
    OutputBufferLength - Length of the output buffer associated with the request.
    OuptutBuffer - The output buffer

Return Value:

  NTSTATUS.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    PNFCCX_SC_INTERFACE scInterface;
    DWORD *pdwProtocol = (DWORD*)InputBuffer;
    DWORD *pdwSelectedProtocol = (DWORD*)OutputBuffer;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    //
    // Buffer validated by the validation method
    //
    _Analysis_assume_(sizeof(DWORD) <= InputBufferLength);
    _Analysis_assume_(sizeof(DWORD) <= OutputBufferLength);

    scInterface = NfcCxFdoGetContext(Device)->SCInterface;

    WdfWaitLockAcquire(scInterface->SmartCardLock, NULL);

    if (!scInterface->SmartCardConnected) {
        TRACE_LINE(LEVEL_ERROR, "SmartCard not connected");
        status = STATUS_NO_MEDIA;
        WdfWaitLockRelease(scInterface->SmartCardLock);
        goto Done;
    }

    WdfWaitLockRelease(scInterface->SmartCardLock);

    if (*pdwProtocol == SCARD_PROTOCOL_OPTIMAL) {
        *pdwSelectedProtocol = SCReaderCurrentProtocolType;
    }
    else if ((((*pdwProtocol) & SCARD_PROTOCOL_DEFAULT) != 0) ||
            (((*pdwProtocol) & SCARD_PROTOCOL_T1) != 0) ) {
        *pdwSelectedProtocol = SCReaderCurrentProtocolType;
    }
    else if ((((*pdwProtocol) & SCARD_PROTOCOL_RAW) != 0) ||
            (((*pdwProtocol) & SCARD_PROTOCOL_T0) != 0) ||
            (((*pdwProtocol) & SCARD_PROTOCOL_Tx) != 0) ) {
        status = STATUS_NOT_SUPPORTED;
        TRACE_LINE(LEVEL_ERROR, "Protocol not supported %d", *pdwProtocol);
    }
    else {
        status = STATUS_INVALID_DEVICE_REQUEST;
        TRACE_LINE(LEVEL_ERROR, "Invalid protocol %d", *pdwProtocol);
    }

Done:
    if (NT_SUCCESS(status)) {
        TRACE_LINE(LEVEL_INFO,
            "Completing request %p, with %!STATUS!, 0x%I64x", Request, status, sizeof(DWORD));
        WdfRequestCompleteWithInformation(Request, status, sizeof(DWORD));
        //
        // Since we have completed the request here, 
        // return STATUS_PENDING to avoid double completion of the request
        //
        status = STATUS_PENDING;
    }

    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);
    TRACE_LOG_NTSTATUS_ON_FAILURE(status);
    
    return status;
}

NTSTATUS
NfcCxSCInterfaceDispatchIsAbsent(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_opt_bytecount_(InputBufferLength) PVOID InputBuffer,
    _In_ size_t InputBufferLength,
    _Out_opt_bytecap_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength
    )
/*++

Routine Description:

    This routine dispatches the SmartCard Is Absent

Arguments:

    Device - Handle to a framework device object.
    Request - Handle to a framework request object.
    InputBuffer - The input buffer
    InputBufferLength - Length of the input buffer.
    OutputBufferLength - Length of the output buffer associated with the request.
    OuptutBuffer - The output buffer

Return Value:

  NTSTATUS.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    PNFCCX_SC_INTERFACE scInterface;

    UNREFERENCED_PARAMETER(InputBuffer);
    UNREFERENCED_PARAMETER(OutputBuffer);

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    if (0 != InputBufferLength) {
        TRACE_LINE(LEVEL_ERROR, "Input buffer should be NULL");
        status = STATUS_INVALID_PARAMETER;
        goto Done;
    }

    if (0 != OutputBufferLength) {
        TRACE_LINE(LEVEL_ERROR, "Output buffer should be NULL");
        status = STATUS_INVALID_PARAMETER;
        goto Done;
    }

    scInterface = NfcCxFdoGetContext(Device)->SCInterface;

    WdfWaitLockAcquire(scInterface->SmartCardLock, NULL);

    if (!scInterface->SmartCardConnected) {
        WdfWaitLockRelease(scInterface->SmartCardLock);
        WdfRequestComplete(Request, STATUS_SUCCESS);
        goto Done;
    }

    WdfWaitLockRelease(scInterface->SmartCardLock);
    
    status = NfcCxSCInterfaceVerifyAndAddIsAbsent(scInterface, Request);

Done:
    if (NT_SUCCESS(status)) {
        //
        // Since we have completed the request here, 
        // return STATUS_PENDING to avoid double completion of the request
        //
        status = STATUS_PENDING;
    }

    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);
    return status;
}

NTSTATUS
NfcCxSCInterfaceDispatchIsPresent(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_opt_bytecount_(InputBufferLength) PVOID InputBuffer,
    _In_ size_t InputBufferLength,
    _Out_opt_bytecap_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength
    )
/*++

Routine Description:

    This routine dispatches the SmartCard Is Present

Arguments:

    Device - Handle to a framework device object.
    Request - Handle to a framework request object.
    InputBuffer - The input buffer
    InputBufferLength - Length of the input buffer.
    OutputBufferLength - Length of the output buffer associated with the request.
    OuptutBuffer - The output buffer

Return Value:

  NTSTATUS.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    PNFCCX_SC_INTERFACE scInterface;

    UNREFERENCED_PARAMETER(InputBuffer);
    UNREFERENCED_PARAMETER(OutputBuffer);

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    if (0 != InputBufferLength) {
        TRACE_LINE(LEVEL_ERROR, "Input buffer should be NULL");
        status = STATUS_INVALID_PARAMETER;
        goto Done;
    }

    if (0 != OutputBufferLength) {
        TRACE_LINE(LEVEL_ERROR, "Output buffer should be NULL");
        status = STATUS_INVALID_PARAMETER;
        goto Done;
    }

    scInterface = NfcCxFdoGetContext(Device)->SCInterface;

    WdfWaitLockAcquire(scInterface->SmartCardLock, NULL);

    if (scInterface->SmartCardConnected) {
        WdfWaitLockRelease(scInterface->SmartCardLock);
        WdfRequestComplete(Request, STATUS_SUCCESS);
        goto Done;
    }

    WdfWaitLockRelease(scInterface->SmartCardLock);
    
    status = NfcCxSCInterfaceVerifyAndAddIsPresent(scInterface, Request);

Done:
    if (NT_SUCCESS(status)) {
        //
        // Since we have completed the request here, 
        // return STATUS_PENDING to avoid double completion of the request
        //
        status = STATUS_PENDING;
    }

    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);
    return status;
}

NTSTATUS
NfcCxSCInterfaceDispatchTransmit(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_opt_bytecount_(InputBufferLength) PVOID InputBuffer,
    _In_ size_t InputBufferLength,
    _Out_opt_bytecap_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength
    )
/*++

Routine Description:

    This routine dispatches the SmartCard Transmit

Arguments:

    Device - Handle to a framework device object.
    Request - Handle to a framework request object.
    InputBuffer - The input buffer.
    InputBufferLength - Length of the input buffer.
    OutputBufferLength - Length of the output buffer associated with the request.
    OuptutBuffer - The output buffer.

Return Value:

    NTSTATUS.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    PNFCCX_SC_INTERFACE scInterface = NULL;
    DWORD cbInputBuffer = 0;
    DWORD cbOutputBuffer = 0;
    DWORD cbOutputBufferUsed = 0;
    DWORD cbResponseBuffer = 0;
    SCARD_IO_REQUEST *inputRequest = (SCARD_IO_REQUEST*)InputBuffer;

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    //
    // Buffer size is validated at dispatch time.
    //
    _Analysis_assume_(DWORD_MAX >= InputBufferLength);
    _Analysis_assume_(DWORD_MAX >= OutputBufferLength);

    cbInputBuffer = (DWORD)InputBufferLength;
    cbOutputBuffer = (DWORD)OutputBufferLength;

    //
    // Since the input and buffer buffer length has already been validated
    // for input buffer length we need to have at least one byte to transfer (discarding the header)
    // for output buffer length we should atleast be able to store status codes SW1+SW2
    //
    _Analysis_assume_(sizeof(SCARD_IO_REQUEST)+1 <= InputBufferLength);
    _Analysis_assume_(sizeof(SCARD_IO_REQUEST)+2 <= OutputBufferLength);

    if (inputRequest->dwProtocol != SCReaderCurrentProtocolType) {
        status = STATUS_INVALID_DEVICE_STATE;
        goto Done;
    }

    scInterface = NfcCxFdoGetContext(Device)->SCInterface;

    WdfWaitLockAcquire(scInterface->SmartCardLock, NULL);

    if (!scInterface->SmartCardConnected) {
        status = STATUS_NO_MEDIA;
        WdfWaitLockRelease(scInterface->SmartCardLock);
        goto Done;
    }

    scInterface->SessionEstablished = TRUE;
    WdfWaitLockRelease(scInterface->SmartCardLock);

    // Check if it is a load key command
    if (NfcCxSCInterfaceValidateLoadKeyCommand((PBYTE)InputBuffer + sizeof(SCARD_IO_REQUEST),
                                               cbInputBuffer - sizeof(SCARD_IO_REQUEST))) {
        BYTE Sw1Sw2[DEFAULT_APDU_STATUS_SIZE] = {0};

        WdfWaitLockAcquire(scInterface->SmartCardLock, NULL);
        status = NfcCxSCInterfaceLoadKeyLocked(scInterface,
                                               (PBYTE)InputBuffer + sizeof(SCARD_IO_REQUEST),
                                               cbInputBuffer - sizeof(SCARD_IO_REQUEST),
                                               Sw1Sw2,
                                               sizeof(Sw1Sw2));
        if (!NT_SUCCESS(status)) {
            WdfWaitLockRelease(scInterface->SmartCardLock);
            TRACE_LINE(LEVEL_ERROR, "Failed to load key, %!STATUS!", status);
            goto Done;
        }

        WdfWaitLockRelease(scInterface->SmartCardLock);

        status = NfcCxSCInterfaceCopyResponseData(OutputBuffer,
                                                  cbOutputBuffer,
                                                  Sw1Sw2,
                                                  sizeof(Sw1Sw2),
                                                  &cbResponseBuffer);
        if (!NT_SUCCESS(status)) {
            TRACE_LINE(LEVEL_ERROR, "Failed to construct response buffer, %!STATUS!", status);
            goto Done;
        }

        cbOutputBufferUsed = cbResponseBuffer;
    }
    else {
        status = NfcCxSCInterfaceTransmitRequest(scInterface,
                                                 (PBYTE)InputBuffer + sizeof(SCARD_IO_REQUEST),
                                                 cbInputBuffer - sizeof(SCARD_IO_REQUEST),
                                                 (PBYTE)OutputBuffer + sizeof(SCARD_IO_REQUEST),
                                                 cbOutputBuffer - sizeof(SCARD_IO_REQUEST),
                                                 &cbOutputBufferUsed);
        if (!NT_SUCCESS(status)) {
            TRACE_LINE(LEVEL_ERROR, "Failed to transmit request, %!STATUS!", status);
            goto Done;
        }

        status = NfcCxSCInterfaceCopyResponseData(OutputBuffer, cbOutputBuffer, NULL, 0, &cbResponseBuffer);
        if (!NT_SUCCESS(status)) {
            TRACE_LINE(LEVEL_ERROR, "Failed to construct response buffer, %!STATUS!", status);
            goto Done;
        }

        cbOutputBufferUsed += cbResponseBuffer;
    }

Done:
    if (NT_SUCCESS(status)) {
        TRACE_LINE(LEVEL_INFO,
            "Completing request %p, with %!STATUS!, output buffer used = %lu", Request, status, cbOutputBufferUsed);
        WdfRequestCompleteWithInformation(Request, status, cbOutputBufferUsed);
        //
        // Since we have completed the request here, 
        // return STATUS_PENDING to avoid double completion of the request
        //
        status = STATUS_PENDING;
    }

    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);
    TRACE_LOG_NTSTATUS_ON_FAILURE(status);

    return status;
}

NTSTATUS
NfcCxSCInterfaceDispatchNotSupported(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_opt_bytecount_(InputBufferLength) PVOID InputBuffer,
    _In_ size_t InputBufferLength,
    _Out_opt_bytecap_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength
    )
/*++

Routine Description:

    This routine returns not supported status code

Arguments:

    Device - Handle to a framework device object.
    Request - Handle to a framework request object.
    InputBuffer - The input buffer
    InputBufferLength - Length of the input buffer.
    OutputBufferLength - Length of the output buffer associated with the request.
    OuptutBuffer - The output buffer

Return Value:

  NTSTATUS.

--*/
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(InputBuffer);
    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBuffer);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
NfcCxSCInterfaceDispatchGetLastError(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_opt_bytecount_(InputBufferLength) PVOID InputBuffer,
    _In_ size_t InputBufferLength,
    _Out_opt_bytecap_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength
    )
/*++

Routine Description:

    This routine returns success status code

Arguments:

    Device - Handle to a framework device object.
    Request - Handle to a framework request object.
    InputBuffer - The input buffer
    InputBufferLength - Length of the input buffer.
    OutputBufferLength - Length of the output buffer associated with the request.
    OuptutBuffer - The output buffer

Return Value:

  NTSTATUS.

--*/
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(InputBuffer);
    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    _Analysis_assume_(NULL != OutputBuffer);
    _Analysis_assume_(sizeof(DWORD) <= OutputBufferLength);

    *((DWORD*)OutputBuffer) = STATUS_SUCCESS;
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(DWORD));

    return STATUS_PENDING;
}

//
// Dispatched attribute methods below
//

FORCEINLINE NTSTATUS
NfcCxSCInterfaceCopyToBuffer(
    _In_reads_bytes_(cbAttributeValue) const VOID *pbAttributeValue,
    _In_ size_t cbAttributeValue,
    _Out_writes_bytes_(*pcbOutputBuffer) PBYTE pbOutputBuffer,
    _Inout_ size_t* pcbOutputBuffer
    )
{
    NTSTATUS status = STATUS_SUCCESS;

    if (*pcbOutputBuffer < cbAttributeValue) {
        status = STATUS_BUFFER_TOO_SMALL;
        goto Done;
    }

    RtlCopyMemory(pbOutputBuffer, pbAttributeValue, cbAttributeValue);
    *pcbOutputBuffer = cbAttributeValue;

Done:
    return status;
}

_Requires_lock_held_(ScInterface->SmartCardLock)
NTSTATUS
NfcCxSCInterfaceDispatchAttributeLocked(
    _In_ PNFCCX_SC_INTERFACE ScInterface,
    _In_opt_bytecount_(cbResultBuffer) PBYTE pbResultBuffer,
    _In_ size_t cbResultBuffer,
    _Out_bytecap_(*pcbOutputBuffer) PBYTE pbOutputBuffer,
    _Inout_ size_t* pcbOutputBuffer
    )
/*++

Routine Description:

    This routine dispatches to copy the attribute dword

Arguments:

    ScInterface - Pointer to the SmartCard Reader interface.
    pbResultBuffer - The buffer points to the result value.
    cbResultBuffer - Length of the result buffer.
    pbOutputBuffer - The output buffer.
    pcbOutputBuffer - Pointer to the length of the output buffer.

Return Value:

  NTSTATUS.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(ScInterface);

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    status = NfcCxSCInterfaceCopyToBuffer(pbResultBuffer, cbResultBuffer, pbOutputBuffer, pcbOutputBuffer);

    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);
    return status;
}

_Requires_lock_held_(ScInterface->SmartCardLock)
NTSTATUS
NfcCxSCInterfaceDispatchAttributePresentLocked(
    _In_ PNFCCX_SC_INTERFACE ScInterface,
    _In_opt_bytecount_(cbResultBuffer) PBYTE pbResultBuffer,
    _In_ size_t cbResultBuffer,
    _Out_bytecap_(*pcbOutputBuffer) PBYTE pbOutputBuffer,
    _Inout_ size_t* pcbOutputBuffer
    )
/*++

Routine Description:

    This routine dispatches to get the present state

Arguments:

    ScInterface - Pointer to the SmartCard Reader interface.
    pbResultBuffer - The buffer points to the result value.
    cbResultBuffer - Length of the result buffer.
    pbOutputBuffer - The output buffer.
    pcbOutputBuffer - Pointer to the length of the output buffer.

Return Value:

  NTSTATUS.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    BYTE IccPresence = (ScInterface->SmartCardConnected ? 0x1 : 0x0);

    UNREFERENCED_PARAMETER(pbResultBuffer);
    UNREFERENCED_PARAMETER(cbResultBuffer);

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    status = NfcCxSCInterfaceCopyToBuffer(&IccPresence, sizeof(IccPresence), pbOutputBuffer, pcbOutputBuffer);

    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);
    return status;
}

_Requires_lock_held_(ScInterface->SmartCardLock)
NTSTATUS
NfcCxSCInterfaceDispatchAttributeCurrentProtocolTypeLocked(
    _In_ PNFCCX_SC_INTERFACE ScInterface,
    _In_opt_bytecount_(cbResultBuffer) PBYTE pbResultBuffer,
    _In_ size_t cbResultBuffer,
    _Out_bytecap_(*pcbOutputBuffer) PBYTE pbOutputBuffer,
    _Inout_ size_t* pcbOutputBuffer
    )
/*++

Routine Description:

    This routine dispatches to get the current protocol type

Arguments:

    ScInterface - Pointer to the SmartCard Reader interface.
    pbResultBuffer - The buffer points to the result value.
    cbResultBuffer - Length of the result buffer.
    pbOutputBuffer - The output buffer.
    pcbOutputBuffer - Pointer to the length of the output buffer.

Return Value:

  NTSTATUS.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    if (!ScInterface->SmartCardConnected) {
        TRACE_LINE(LEVEL_ERROR, "SmartCard not connected");
        status = STATUS_INVALID_DEVICE_STATE;
        goto Done;
    }
    
    status = NfcCxSCInterfaceCopyToBuffer(pbResultBuffer, cbResultBuffer, pbOutputBuffer, pcbOutputBuffer);

Done:
    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);
    return status;
}

_Requires_lock_not_held_(ScInterface->SmartCardLock)
NTSTATUS
NfcCxSCInterfaceDispatchAttributeAtr(
    _In_ PNFCCX_SC_INTERFACE ScInterface,
    _In_opt_bytecount_(cbResultBuffer) PBYTE pbResultBuffer,
    _In_ size_t cbResultBuffer,
    _Out_bytecap_(*pcbOutputBuffer) PBYTE pbOutputBuffer,
    _Inout_ size_t* pcbOutputBuffer
    )
/*++

Routine Description:

    This routine dispatches to get the present state

Arguments:

    ScInterface - Pointer to the SmartCard Reader interface.
    pbResultBuffer - The buffer points to the result value.
    cbResultBuffer - Length of the result buffer.
    pbOutputBuffer - The output buffer.
    pcbOutputBuffer - Pointer to the length of the output buffer.

Return Value:

  NTSTATUS.

--*/
{
    static const DWORD PositionOfNN = 13;

    NTSTATUS status = STATUS_SUCCESS;
    BOOLEAN isStorageCard = FALSE;
    DWORD cbOutputBufferUsed = 0;

    UNREFERENCED_PARAMETER(pbResultBuffer);
    UNREFERENCED_PARAMETER(cbResultBuffer);

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    // Buffer size is validated at dispatch time
    _Analysis_assume_(DWORD_MAX >= *pcbOutputBuffer);

    WdfWaitLockAcquire(ScInterface->SmartCardLock, NULL);

    if (!ScInterface->SmartCardConnected) {
        WdfWaitLockRelease(ScInterface->SmartCardLock);
        TRACE_LINE(LEVEL_ERROR, "SmartCard not connected");
        status = STATUS_INVALID_DEVICE_STATE;
        goto Done;
    }

    isStorageCard = NfcCxSCInterfaceIsStorageCardConnected(ScInterface);

    if (isStorageCard) {
        NT_ASSERT(ScInterface->StorageCard != NULL);
        if (ScInterface->StorageCard->AtrCached()) {
            TRACE_LINE(LEVEL_INFO, "Using cached ATR");
            status = ScInterface->StorageCard->GetCachedAtr(pbOutputBuffer,
                                                            (DWORD)*pcbOutputBuffer,
                                                            &cbOutputBufferUsed);
            if (NT_SUCCESS(status)) {
                *pcbOutputBuffer = cbOutputBufferUsed;
            }

            WdfWaitLockRelease(ScInterface->SmartCardLock);
            goto Done;
        }
    }

    status = NfcCxSCInterfaceGetAtrLocked(ScInterface,
                                          pbOutputBuffer,
                                          (DWORD)*pcbOutputBuffer,
                                          &cbOutputBufferUsed);
    if (!NT_SUCCESS(status)) {
        WdfWaitLockRelease(ScInterface->SmartCardLock);
        TRACE_LINE(LEVEL_ERROR, "Failed to get the ATR string, %!STATUS!", status);
        goto Done;
    }

    if (isStorageCard) {
        ScInterface->StorageCard->CacheAtr(pbOutputBuffer, cbOutputBufferUsed);
    }

    WdfWaitLockRelease(ScInterface->SmartCardLock);

    *pcbOutputBuffer = cbOutputBufferUsed;

Done:
    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);
    return status;
}

_Requires_lock_held_(ScInterface->SmartCardLock)
NTSTATUS
NfcCxSCInterfaceDispatchAttributeIccTypeLocked(
    _In_ PNFCCX_SC_INTERFACE ScInterface,
    _In_opt_bytecount_(cbResultBuffer) PBYTE pbResultBuffer,
    _In_ size_t cbResultBuffer,
    _Out_bytecap_(*pcbOutputBuffer) PBYTE pbOutputBuffer,
    _Inout_ size_t* pcbOutputBuffer
    )
/*++

Routine Description:

    This routine dispatches to get the present state

Arguments:

    ScInterface - Pointer to the SmartCard Reader interface.
    pbResultBuffer - The buffer points to the result value.
    cbResultBuffer - Length of the result buffer.
    pbOutputBuffer - The output buffer.
    pcbOutputBuffer - Pointer to the length of the output buffer.

Return Value:

  NTSTATUS.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    BYTE IccType = 0;

    UNREFERENCED_PARAMETER(pbResultBuffer);
    UNREFERENCED_PARAMETER(cbResultBuffer);

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    if (!ScInterface->SmartCardConnected) {
        status = STATUS_INVALID_DEVICE_STATE;
        goto Done;
    }

    status = NfcCxSCInterfaceGetIccTypePerAtrLocked(ScInterface, &IccType);
    if (NT_SUCCESS(status)) {
        status = NfcCxSCInterfaceCopyToBuffer(&IccType, sizeof(IccType), pbOutputBuffer, pcbOutputBuffer);
    }

Done:
    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);
    return status;
}

//
// Other internal methods below
//

NTSTATUS
NfcCxSCInterfaceVerifyAndAddIsAbsent(
    _In_ PNFCCX_SC_INTERFACE ScInterface,
    _In_ WDFREQUEST Request
    )
/*++

Routine Description:

    This routine verifies the IsAbsent request has not been previously called,
    and it forwards the request into a manual IO queue.

Arguments:

    ScInterface - Pointer to the SmartCard Reader interface.
    Request - Handle to a framework request object.
    
Return Value:

  NTSTATUS.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    WDFREQUEST outRequest = NULL;

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    if (NT_SUCCESS(WdfIoQueueFindRequest(ScInterface->AbsentQueue,
                                         NULL,
                                         WdfRequestGetFileObject(Request),
                                         NULL,
                                         &outRequest))) {
        WdfObjectDereference(outRequest);
        status = STATUS_DEVICE_BUSY;
        goto Done;
    }

    status = WdfRequestForwardToIoQueue(Request, ScInterface->AbsentQueue);
    if (!NT_SUCCESS(status)) {
        TRACE_LINE(LEVEL_ERROR, "Failed to forward request to IO Queue, %!STATUS!", status);
        goto Done;
    }

Done:
    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);
    return status;
}

NTSTATUS
NfcCxSCInterfaceVerifyAndAddIsPresent(
    _In_ PNFCCX_SC_INTERFACE ScInterface,
    _In_ WDFREQUEST Request
    )
/*++

Routine Description:

    This routine verifies the IsPresent request has not been previously called,
    and it forwards the request into a manual IO queue.

Arguments:

    ScInterface - Pointer to the SmartCard Reader interface.
    Request - Handle to a framework request object.
    
Return Value:

  NTSTATUS.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    WDFREQUEST outRequest = NULL;

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    if (NT_SUCCESS(WdfIoQueueFindRequest(ScInterface->PresentQueue,
                                         NULL,
                                         WdfRequestGetFileObject(Request),
                                         NULL,
                                         &outRequest))) {
        WdfObjectDereference(outRequest);
        status = STATUS_DEVICE_BUSY;
        goto Done;
    }

    status = WdfRequestForwardToIoQueue(Request, ScInterface->PresentQueue);
    if (!NT_SUCCESS(status)) {
        TRACE_LINE(LEVEL_ERROR, "Failed to forward request to IO Queue, %!STATUS!", status);
        goto Done;
    }

Done:
    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);
    return status;
}

NTSTATUS
NfcCxSCInterfaceCopyResponseData(
    _Out_opt_bytecap_(OutputBufferLength) PVOID OutputBuffer,
    _In_ ULONG OutputBufferLength,
    _In_bytecount_(DataLength) PVOID Data,
    _In_ ULONG DataLength,
    _Out_ PULONG BufferUsed
    )
/*++

Routine Description:

   Copies the response data into the transmit output buffer.

Arguments:

    OutputBuffer - The Output buffer
    OutputBufferLength - The output buffer length
    Data - The response data buffer
    DataLength - The response data buffer length
    BufferUsed - A pointer to a ULONG to receive how many bytes of the output buffer was used.

Return Value:

    NTSTATUS

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG requiredBufferSize = 0;
    SCARD_IO_REQUEST outputRequest = {0};

    *BufferUsed = 0;

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    //
    // The output buffer has already been validated
    //
    _Analysis_assume_(sizeof(SCARD_IO_REQUEST)+2 <= OutputBufferLength);

    //
    // The returning buffer should contains the SCARD_IO_REQUEST structure
    // followed by the response payload
    //
    outputRequest.dwProtocol = SCReaderCurrentProtocolType;
    outputRequest.cbPciLength = sizeof(SCARD_IO_REQUEST);

    if (OutputBufferLength < sizeof(SCARD_IO_REQUEST)) {
        status = STATUS_BUFFER_TOO_SMALL;
        goto Done;
    }

    CopyMemory(OutputBuffer, &outputRequest, sizeof(SCARD_IO_REQUEST));
    *BufferUsed = sizeof(SCARD_IO_REQUEST);

    status = RtlULongAdd(DataLength, sizeof(SCARD_IO_REQUEST), &requiredBufferSize);
    if (!NT_SUCCESS(status)) {
        TRACE_LINE(LEVEL_ERROR, "Failed to calculate the required buffer size, %!STATUS!", status);
        goto Done;
    }

    if (OutputBufferLength < requiredBufferSize) {
        status = STATUS_BUFFER_TOO_SMALL;
        goto Done;
    }

    CopyMemory(((PUCHAR)OutputBuffer) + sizeof(SCARD_IO_REQUEST), Data, DataLength);
    *BufferUsed = requiredBufferSize;

Done:

    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);

    return status;
}

NTSTATUS
NfcCxSCInterfaceDistributePresentAbsentEvent(
    _In_ PNFCCX_SC_INTERFACE ScInterface,
    _In_ WDFQUEUE Queue
    )
/*++

Routine Description:

    This routine distribute the Present or Absent event

Arguments:

    ScInterface - A pointer to the SmartCard Reader interface
    Queue - framework queue object
    
Return Value:

    NTSTATUS

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    WDFREQUEST request = NULL;

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    UNREFERENCED_PARAMETER(ScInterface);

    while (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Queue, &request))) {
        TRACE_LINE(LEVEL_INFO, "Completing Request with Status, %!STATUS!", STATUS_SUCCESS);
        WdfRequestComplete(request, STATUS_SUCCESS);
    }

    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);

    return status;
}

_Requires_lock_not_held_(ScInterface->SmartCardLock)
NTSTATUS
NfcCxSCInterfaceTransmitRequest(
    _In_ PNFCCX_SC_INTERFACE ScInterface,
    _In_opt_bytecount_(InputBufferLength) PBYTE InputBuffer,
    _In_ DWORD InputBufferLength,
    _Out_opt_bytecap_(OutputBufferLength) PBYTE OutputBuffer,
    _In_ DWORD OutputBufferLength,
    _Out_ DWORD* BytesTransferred
    )
/*++

Routine Description:

    This routine dispatches the SmartCard Transmit

Arguments:

    ScInterface - Pointer to the SmartCard Reader interface.
    InputBuffer - The input buffer
    InputBufferLength - Length of the input buffer.
    OutputBufferLength - Length of the output buffer associated with the request.
    OuptutBuffer - The output buffer
    BytesTransferred - The number of bytes copied to the output buffer

Return Value:

    NTSTATUS.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    BOOLEAN isStorageCard = FALSE;
    PcscInsByte command = (PcscInsByte)0x00;
    BYTE intermediateBuffer[COMMAND_BUFFER_SIZE] = {0};
    DWORD cbIntermediateBuffer = 0;
    StorageCardManager* pStorageCard = NULL;

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    *BytesTransferred = 0;

    WdfWaitLockAcquire(ScInterface->SmartCardLock, NULL);

    if (!ScInterface->SmartCardConnected) {
        WdfWaitLockRelease(ScInterface->SmartCardLock);
        TRACE_LINE(LEVEL_ERROR, "SmartCard not connected");
        status = STATUS_INVALID_DEVICE_STATE;
        goto Done;
    }

    isStorageCard = NfcCxSCInterfaceIsStorageCardConnected(ScInterface);

    WdfWaitLockRelease(ScInterface->SmartCardLock);

    if (!isStorageCard) {
        TRACE_LINE(LEVEL_INFO, "Transmit: Card TypeA/TypeB");
        status = NfcCxSCInterfaceTransmitRawData(ScInterface,
                                                 InputBuffer,
                                                 InputBufferLength,
                                                 OutputBuffer,
                                                 OutputBufferLength,
                                                 BytesTransferred,
                                                 DEFAULT_14443_4_TRANSCEIVE_TIMEOUT);
    }
    else {
        ApduResult apduStatus = RESULT_SUCCESS;
        pStorageCard = NfcCxAcquireStorageCardManagerReference(ScInterface);

        if (pStorageCard == NULL) {
            status = STATUS_INVALID_DEVICE_STATE;
            TRACE_LINE(LEVEL_ERROR, "SmartCard connection lost");
            goto Done;
        }

        TRACE_LINE(LEVEL_INFO, "Transmit: Storage Card");

        //
        // Validate the parameters
        //
        apduStatus = pStorageCard->ValidateParameters(InputBuffer, NULL);

        if (RESULT_SUCCESS != apduStatus) {

            if (OutputBufferLength < DEFAULT_APDU_STATUS_SIZE) {
                status = STATUS_INVALID_PARAMETER;
                TRACE_LINE(LEVEL_ERROR, "OutputBufferLength too small");
                goto Done;
            }

            RtlCopyMemory((PBYTE)OutputBuffer, APDU_STATUS_ERROR_DATA_OBJECT_MISSING, DEFAULT_APDU_STATUS_SIZE);

            *BytesTransferred = DEFAULT_APDU_STATUS_SIZE;
            goto Done;
        }

        apduStatus = pStorageCard->GetCommandFromAPDU(InputBuffer, InputBufferLength, &command);

        if (RESULT_SUCCESS != apduStatus) {
            TRACE_LINE(LEVEL_ERROR, "GetCommandFromAPDU failed (%d)", apduStatus);
        }
        else {
            switch (command)
            {
                case PcscGetDataCmd:
                {
                    apduStatus = pStorageCard->GetDataCommand(InputBuffer,
                                                              InputBufferLength,
                                                              OutputBuffer,
                                                              OutputBufferLength,
                                                              BytesTransferred);
                    if (RESULT_SUCCESS != apduStatus) {
                        TRACE_LINE(LEVEL_ERROR, " GetDataCommand failed (%d)", apduStatus);
                        break;
                    }
                }
                break;

                case PcscReadCmd:
                {
                    apduStatus = pStorageCard->ReadBinary(InputBuffer,
                                                          InputBufferLength,
                                                          intermediateBuffer,
                                                          sizeof(intermediateBuffer),
                                                          &cbIntermediateBuffer);
                    if (RESULT_SUCCESS != apduStatus) {
                        TRACE_LINE(LEVEL_ERROR, " ReadBinary failed (%d)", apduStatus);
                        break;
                    }

                    apduStatus = pStorageCard->StorageCardTransceive(intermediateBuffer,
                                                                     cbIntermediateBuffer,
                                                                     OutputBuffer,
                                                                     OutputBufferLength,
                                                                     BytesTransferred);
                }
                break;

                case PcscWriteCmd:
                {
                    apduStatus = pStorageCard->UpdateBinary(InputBuffer,
                                                            InputBufferLength,
                                                            intermediateBuffer,
                                                            sizeof(intermediateBuffer),
                                                            &cbIntermediateBuffer);
                    if (RESULT_SUCCESS != apduStatus) {
                        TRACE_LINE(LEVEL_ERROR, " UpdateBinary failed (%d)", apduStatus);
                        break;
                    }

                    apduStatus = pStorageCard->StorageCardTransceive(intermediateBuffer,
                                                                     cbIntermediateBuffer,
                                                                     OutputBuffer,
                                                                     OutputBufferLength,
                                                                     BytesTransferred);
                }
                break;

                case PcscMifareGenAuthCmd:
                {
                    TRACE_LINE(LEVEL_INFO, "Mifare General Authentication\n");

                    WdfWaitLockAcquire(ScInterface->SmartCardLock, NULL);

                    if (ScInterface->StorageCardKey == NULL) {
                        WdfWaitLockRelease(ScInterface->SmartCardLock);
                        status = STATUS_INVALID_DEVICE_STATE;
                        goto Done;
                    }

                    if (InputBufferLength > sizeof(intermediateBuffer)) {
                        WdfWaitLockRelease(ScInterface->SmartCardLock);
                        status = STATUS_INVALID_PARAMETER;
                        goto Done;
                    }

                    apduStatus = pStorageCard->GetGeneralAuthenticateCommand(ScInterface->StorageCardKey,
                                                                             InputBuffer,
                                                                             InputBufferLength,
                                                                             intermediateBuffer,
                                                                             sizeof(intermediateBuffer),
                                                                             &cbIntermediateBuffer);
                    if (RESULT_SUCCESS != apduStatus) {
                        WdfWaitLockRelease(ScInterface->SmartCardLock);
                        TRACE_LINE(LEVEL_ERROR, " GetGeneralAuthenticateCommand failed (%d)", apduStatus);
                        break;
                    }

                    WdfWaitLockRelease(ScInterface->SmartCardLock);

                    apduStatus = pStorageCard->StorageCardTransceive(intermediateBuffer,
                                                                     cbIntermediateBuffer,
                                                                     OutputBuffer,
                                                                     OutputBufferLength,
                                                                     BytesTransferred);
                }
                break;

                case PcscManageSessionCmd:
                {
                    TRACE_LINE(LEVEL_INFO, "PCSC Manage Session Command\n");

                    apduStatus = pStorageCard->HandleManageSessionCommand(InputBuffer,
                                                                          InputBufferLength,
                                                                          OutputBuffer,
                                                                          OutputBufferLength,
                                                                          BytesTransferred);
                    if (RESULT_SUCCESS != apduStatus) {
                        TRACE_LINE(LEVEL_ERROR, " PcscManageSessionCmd failed (%d)", apduStatus);
                    }
                }
                break;

                case PcscTransExchangeCmd:
                {
                    TRACE_LINE(LEVEL_INFO, "PCSC Transparent Exchange Command\n");

                    apduStatus = pStorageCard->HandleTransSessionCommand(InputBuffer,
                                                                         InputBufferLength,
                                                                         OutputBuffer,
                                                                         OutputBufferLength,
                                                                         BytesTransferred);
                    if (RESULT_SUCCESS != apduStatus) {
                        TRACE_LINE(LEVEL_ERROR, " PcscTransExchangeCmd failed (%d)", apduStatus);
                    }
                }
                break;

                case PcscIncrementDecrementCmd:
                {
                    TRACE_LINE(LEVEL_INFO, "PCSC Increment Decrement Command\n");

                    apduStatus = pStorageCard->HandleIncDecCommand(InputBuffer,
                                                                   InputBufferLength,
                                                                   OutputBuffer,
                                                                   OutputBufferLength,
                                                                   BytesTransferred);
                    if (RESULT_SUCCESS != apduStatus) {
                        TRACE_LINE(LEVEL_ERROR, " PcscIncrementDecrementCmd failed (%d)", apduStatus);
                    }
                }
                break;

                default:
                {
                    apduStatus = RESULT_NOT_SUPPORTED;
                    TRACE_LINE(LEVEL_ERROR, " Wrong Command for the storage card 0x%x", command);
                }
            }
        }

        if (NT_SUCCESS(status)) {
            uint8_t Sw1Sw2Response[DEFAULT_APDU_STATUS_SIZE] = {0x00,0x00};

            pStorageCard->PrepareResponseCode(apduStatus, Sw1Sw2Response);
            pStorageCard->UpdateResponseCodeToResponseBuffer(apduStatus,
                                                             command,
                                                             *BytesTransferred,
                                                             Sw1Sw2Response,
                                                             OutputBuffer,
                                                             OutputBufferLength,
                                                             BytesTransferred);
        }
    }

Done:

    NfcCxReleaseStorageCardManagerReference(pStorageCard);
    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);
    return status;
}

_Requires_lock_not_held_(ScInterface->SmartCardLock)
NTSTATUS
NfcCxSCInterfaceTransmitRawData(
    _In_ PNFCCX_SC_INTERFACE ScInterface,
    _In_opt_bytecount_(InputBufferLength) PBYTE InputBuffer,
    _In_ DWORD InputBufferLength,
    _Out_writes_bytes_to_(OutputBufferLength, *BytesTransferred) PBYTE OutputBuffer,
    _In_ DWORD OutputBufferLength,
    _Out_ DWORD* BytesTransferred,
    _In_ USHORT Timeout
    )
/*++

Routine Description:

    This routine dispatches the SmartCard Transmit

Arguments:

    ScInterface - Pointer to the SmartCard Reader interface.
    InputBuffer - The input buffer
    InputBufferLength - Length of the input buffer.
    OuptutBuffer - The output buffer
    OutputBufferLength - Length of the output buffer associated with the request.
    BytesTransferred - The number of bytes copied to the output buffer
    Timeout - Transceive timeout in milliseconds

Return Value:

  NTSTATUS.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    size_t cbOutputBuffer = 0;

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    *BytesTransferred = 0;

    status = NfcCxRFInterfaceTransmit(ScInterface->FdoContext->RFInterface,
                                      InputBuffer,
                                      InputBufferLength,
                                      OutputBuffer,
                                      OutputBufferLength,
                                      &cbOutputBuffer,
                                      Timeout);

    // OutputBufferLength is less than or equal to DWORD_MAX
    _Analysis_assume_(DWORD_MAX >= cbOutputBuffer);

    *BytesTransferred = (DWORD)cbOutputBuffer;

    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);

    return status;
}

//
// APDU to Storage card command conversion
//

_Requires_lock_held_(ScInterface->SmartCardLock)
NTSTATUS
NfcCxSCInterfaceLoadStorageClassFromAtrLocked(
    _In_ PNFCCX_SC_INTERFACE ScInterface
    )
/*++

Routine Description:

    This routine loads the storage class based on the ATR string

Arguments:

    ScInterface - Pointer to the SmartCard Reader interface.

Return Value:

  NTSTATUS.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    phNfc_eRemDevType_t deviceType = phNfc_eUnknown_DevType;
    DWORD sak = 0;
    BYTE* pUid = NULL;
    BYTE cbUidLength = 0;
    BYTE Atr[PHNFC_MAX_ATR_LENGTH] = {0};
    DWORD cbAtrLength = 0;

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    if (!ScInterface->SmartCardConnected) {
        TRACE_LINE(LEVEL_ERROR, "SmartCard not connected");
        status = STATUS_INVALID_DEVICE_STATE;
        goto Done;
    }

    status = NfcCxSCInterfaceGetDeviceUidLocked(ScInterface,
                                                &pUid,
                                                &cbUidLength);
    if (!NT_SUCCESS(status)) {
        TRACE_LINE(LEVEL_ERROR, "Failed to get device info, %!STATUS!", status);
        goto Done;
    }

    status = NfcCxSCInterfaceGetAtrLocked(ScInterface,
                                          Atr,
                                          sizeof(Atr),
                                          &cbAtrLength);
    if (!NT_SUCCESS(status)) {
        TRACE_LINE(LEVEL_ERROR, "Failed to get the ATR string, %!STATUS!", status);
        goto Done;
    }

    if (!NfcCxSCInterfaceIsStorageCardConnected(ScInterface)) {
        goto Done;
    }

    if (NULL != ScInterface->StorageCard) {
        TRACE_LINE(LEVEL_INFO, "Storage card already exists");
        goto Done;
    }

    NfcCxSCInterfaceGetDeviceTypeLocked(ScInterface,
                                        &deviceType,
                                        &sak);

    ScInterface->StorageCard = StorageCardManager::Create(deviceType, sak, ScInterface);
    if (NULL == ScInterface->StorageCard) {
        TRACE_LINE(LEVEL_ERROR, "Failed to allocate the storage class");
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }

    // initialize mifare key store with default buffer space
    ScInterface->StorageCardKey = new LoadKey();

    if (NULL == ScInterface->StorageCardKey) {
        TRACE_LINE(LEVEL_ERROR, "Failed to allocate the storage class key");
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }

    status = ScInterface->StorageCardKey->Initialize(DEFAULT_KEY_SPACE);
    if (!NT_SUCCESS(status) ) {
        TRACE_LINE(LEVEL_ERROR, "Failed to initialize Key store, %!STATUS!", status);
        goto Done;
    }

    ScInterface->StorageCard->UpdateUniqueID(pUid, cbUidLength);
    ScInterface->StorageCard->UpdateHistoricalBytes((Atr + 4), MAX_STORAGECARD_HISTOBYTES);

Done:

    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);
    return status;
}

BOOL
NfcCxSCInterfaceValidateLoadKeyCommand(
    _In_bytecount_(InputBufferLength) PBYTE InputBuffer,
    _In_ DWORD InputBufferLength
    )
/*++

Routine Description:

    This routine loads the key

Arguments:

    InputBuffer - The input buffer.
    InputBufferLength - Length of the input buffer.

Return Value:

    BOOL.

--*/
{
    BOOL fLoadKey = FALSE;

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    if (InputBufferLength < MIN_APDU_HEADER) {
        TRACE_LINE(LEVEL_ERROR, "Invalid APDU buffer size");
        goto Done;
    }

    PPcscCommandApduInfo cmdApdu = (PPcscCommandApduInfo)InputBuffer;
    if(0xFF == cmdApdu->Cla && PcscMifareLoadAuthKeyCmd == cmdApdu->Ins) {
        TRACE_LINE(LEVEL_INFO, "Command APDU is Load Key");
        fLoadKey = TRUE;
    }

Done:
    TRACE_FUNCTION_EXIT(LEVEL_VERBOSE);
    return fLoadKey;
}

_Requires_lock_held_(ScInterface->SmartCardLock)
NTSTATUS
NfcCxSCInterfaceLoadKeyLocked(
    _In_ PNFCCX_SC_INTERFACE ScInterface,
    _In_bytecount_(InputBufferLength) PBYTE InputBuffer,
    _In_ DWORD InputBufferLength,
    _Out_bytecap_(Sw1Sw2Length) PBYTE Sw1Sw2,
    _In_ DWORD Sw1Sw2Length
    )
/*++

Routine Description:

    This routine loads the key

Arguments:

    ScInterface - Pointer to the SC Interface
    InputBuffer - The input buffer
    InputBufferLength - Length of the input buffer.
    Sw1Sw2Length - Length of the SW1 SW2 buffer.
    Sw1Sw2 - The SW1 SW2 buffer

Return Value:

    NTSTATUS.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    
    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    status = NfcCxSCInterfaceValidateMifareLoadKeyParameters(InputBuffer, 
                                                             InputBufferLength, 
                                                             Sw1Sw2, 
                                                             Sw1Sw2Length);
    if (NT_SUCCESS(status)) {
       status = NfcCxSCInterfaceKeyDataBaseLocked(ScInterface, 
                                                  InputBuffer, 
                                                  InputBufferLength, 
                                                  Sw1Sw2, 
                                                  Sw1Sw2Length);
    }

    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);
    return status;
}

_Requires_lock_held_(ScInterface->SmartCardLock)
NTSTATUS
NfcCxSCInterfaceKeyDataBaseLocked(
    _In_ PNFCCX_SC_INTERFACE ScInterface,
    _In_bytecount_(InputBufferLength) PBYTE InputBuffer,
    _In_ DWORD InputBufferLength,
    _Out_opt_bytecap_(Sw1Sw2Length) PBYTE Sw1Sw2,
    _In_ DWORD Sw1Sw2Length
    )
/*++

Routine Description:

    This routine loads the key

Arguments:

    ScInterface - Pointer to the SCInterface
    InputBuffer - The input buffer
    InputBufferLength - Length of the input buffer.
    Sw1Sw2Length - Length of the SW1 SW2 buffer.
    Sw1Sw2 - The SW1 SW2 buffer

Return Value:

    NTSTATUS.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    PPcscCommandApduInfo pCommandApdu = (PPcscCommandApduInfo)InputBuffer;
    BYTE KeyNumber = pCommandApdu->P2;
    DWORD dwKeyIndex;
    
    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    RtlZeroMemory(Sw1Sw2, Sw1Sw2Length);

    if (Sw1Sw2Length < DEFAULT_APDU_STATUS_SIZE) {
        TRACE_LINE(LEVEL_ERROR, "Invalid SW1 SW2 buffer size");
        status = STATUS_INVALID_PARAMETER;
        goto Done;
    }

    if (InputBufferLength < MIN_APDU_HEADER) {
        TRACE_LINE(LEVEL_ERROR, "Invalid APDU header size");
        status = STATUS_INVALID_PARAMETER;
        goto Done;
    }

    if (ScInterface->StorageCardKey == NULL) {
        status = STATUS_INVALID_DEVICE_STATE;
        goto Done;
    }

    // Check the KeyNumber is present in KeyTable,
    // If present OverWrite the Keys else Validate KeyTable Size and Write the Keys
    if ((dwKeyIndex = ((LoadKey*)ScInterface->StorageCardKey)->ExtractKeyIndex(KeyNumber)) != (DWORD)-1) {
        TRACE_LINE(LEVEL_INFO, "OverWrite the Keys in the Available Index\n");
        ((LoadKey*)ScInterface->StorageCardKey)->OverWriteKeys(dwKeyIndex, KeyNumber, pCommandApdu->DataIn, MAX_BUFFERSIZE);
    }
    else {
        if(((LoadKey*)ScInterface->StorageCardKey)->KeyTableFull()) {
            TRACE_LINE(LEVEL_ERROR, "Key Table FULL\n");
            RtlCopyMemory(Sw1Sw2, APDU_STATUS_ERROR_INCORRECT_DATA_OBJECT, DEFAULT_APDU_STATUS_SIZE); // Key Number not valid
            status = STATUS_DISK_FULL;
        }
        else {
            if (((LoadKey*)ScInterface->StorageCardKey)->InsertKey(KeyNumber, pCommandApdu->DataIn, MAX_BUFFERSIZE)) {
                TRACE_LINE(LEVEL_INFO, "Key Stored in Specified Location\n");
                RtlCopyMemory(Sw1Sw2, APDU_STATUS_SUCCESS, DEFAULT_APDU_STATUS_SIZE);
            }
        }
    }

Done:

    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);
    return status;
}

NTSTATUS
NfcCxSCInterfaceValidateMifareLoadKeyParameters(
    _In_bytecount_(InputBufferLength) PBYTE InputBuffer,
    _In_ DWORD InputBufferLength,
    _Out_bytecap_(Sw1Sw2Length) PBYTE Sw1Sw2,
    _In_ DWORD Sw1Sw2Length
    )
/*++

Routine Description:

    This routine loads the key

Arguments:
    
    InputBuffer - The input buffer
    InputBufferLength - Length of the input buffer.
    Sw1Sw2Length - Length of the SW1 SW2 buffer.
    Sw1Sw2 - The SW1 SW2 buffer

Return Value:

  NTSTATUS.

--*/
{
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    
    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    if (Sw1Sw2Length < DEFAULT_APDU_STATUS_SIZE) {
        TRACE_LINE(LEVEL_ERROR, "Invalid SW1 SW2 buffer size");
        goto Done;
    }
   
    PPcscCommandApduInfo pCommandApdu = (PPcscCommandApduInfo)InputBuffer;

    // For MIFARE APDU expected to be of size 0xB as per APDU definition
    if (0xB != InputBufferLength) {
        // Wrong APDU Length
        RtlCopyMemory(Sw1Sw2, APDU_STATUS_ERROR_WRONG_APDU_LENGTH, DEFAULT_APDU_STATUS_SIZE);
        TRACE_LINE(LEVEL_ERROR, "Wrong APDU Length");
    }
    else {
        // Check for P1 byte
        if (pCommandApdu->P1 !=0x00) {
            // Check for 7th bit is set
            if (pCommandApdu->P1 & 0x80) {
                // SW1 = Load key Error
                // SW2 = Reader key not supported
                RtlCopyMemory(Sw1Sw2, APDU_STATUS_ERROR_AUTH_METHOD_BLOCKED, DEFAULT_APDU_STATUS_SIZE);
                TRACE_LINE(LEVEL_ERROR, "Reader key not supported");
            }
            else {
                // Check for 6th bit is set
                if (pCommandApdu->P1 & 0x40) {
                    // SW1 = Load key Error
                    // SW2 = Secured Transmission not supported
                    RtlCopyMemory(Sw1Sw2, APDU_STATUS_ERROR_CONDITIONS_OF_USE_NOT_SATISFIED, DEFAULT_APDU_STATUS_SIZE);
                    TRACE_LINE(LEVEL_ERROR, "Secured Transmission not supported");
                }
                else {
                    // Check for 6th bit is set
                    if (pCommandApdu->P1 & 0x20) {
                        // SW1 = Load key Error
                        // SW2 = Non volatile memory is not available
                        RtlCopyMemory(Sw1Sw2, APDU_STATUS_ERROR_DATA_OBJECT_MISSING, DEFAULT_APDU_STATUS_SIZE);
                        TRACE_LINE(LEVEL_ERROR, "Non volatile memory is not available");
                    }
                    else {
                        // SW1 = Load key Error
                        // SW2 = error but no information
                        RtlCopyMemory(Sw1Sw2, APDU_STATUS_WARNING_NO_INFO, DEFAULT_APDU_STATUS_SIZE);
                        TRACE_LINE(LEVEL_ERROR, "Error but no information");
                    }

                }
            }
        }
        // Validate Lc byte now. Key length for MIFARE is always 6 Byte
        else if (pCommandApdu->Lc !=0x6) {
            // Invalid Key length
            // SW1 = Load key Error
            // SW2 = Invalid key length
            RtlCopyMemory(Sw1Sw2, APDU_STATUS_ERROR_INVALID_OBJECT_LENGTH, DEFAULT_APDU_STATUS_SIZE);
            TRACE_LINE(LEVEL_ERROR, "Invalid key length");
        }
        else {
            // STATUS SUCCESS
            RtlCopyMemory(Sw1Sw2, APDU_STATUS_SUCCESS, DEFAULT_APDU_STATUS_SIZE);
            TRACE_LINE(LEVEL_INFO, "Load Key Parameter validation succeeded");
            status = STATUS_SUCCESS;
        }
    }

Done:
    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);
    return status;
}

_Requires_lock_held_(ScInterface->SmartCardLock)
NTSTATUS
NfcCxSCInterfaceGetAtrLocked(
    _In_ PNFCCX_SC_INTERFACE ScInterface,
    _Out_writes_bytes_to_(OutputBufferLength, *ByteCopied) PBYTE OutputBuffer,
    _In_ DWORD OutputBufferLength,
    _Out_ DWORD* BytesCopied
    )
/*++

Routine Description:

    This routine is called from the SmartCard module to retrieve the ATR string of the remote device

Arguments:

    ScInterface - The SmartCard reader Interface
    OutputBuffer - Pointer to the buffer to be filled
    OutputBufferLength - Length of the output buffer
    BytesCopied - Pointer to the length of the buffer used

Return Value:

    NTSTATUS

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    DWORD index = 0;
    BYTE tck = 0;
    BYTE Output[PHNFC_MAX_ATR_LENGTH] = {0};
    DWORD cbBytesUsed = 0;
    phNfc_sRemoteDevInformation_t* pRemDev = &(ScInterface->RemoteDeviceInfo);

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    TRACE_LINE(LEVEL_INFO, "Remote Device Type: %!phNfc_eRFDevType_t!", pRemDev->RemDevType);

    switch (pRemDev->RemDevType)
    {
        case phLibNfc_eISO14443_4A_PICC:
        {
            // Inital Header
            Output[index++] = PCSC_ATR_INIT_HEADER;
            Output[index++] = PCSC_ATR_T0 | ((pRemDev->RemoteDevInfo.Iso14443A_Info.AppDataLength) & PCSC_ATR_T0_MASK);
            Output[index++] = PCSC_ATR_TD1; // TD1
            Output[index++] = PCSC_ATR_TD2; // TD2

            // For type A only
            if (PHNFC_MAX_ATR_LENGTH < (index + pRemDev->RemoteDevInfo.Iso14443A_Info.AppDataLength)) {
                status = STATUS_BUFFER_TOO_SMALL;
                goto Done;
            }
            RtlCopyMemory(Output + index,
                pRemDev->RemoteDevInfo.Iso14443A_Info.AppData,
                pRemDev->RemoteDevInfo.Iso14443A_Info.AppDataLength);

            index += pRemDev->RemoteDevInfo.Iso14443A_Info.AppDataLength;

            tck = NfcCxSCInterfaceComputeChecksum(Output, index);

            if (PHNFC_MAX_ATR_LENGTH < (index + 1)) {
                status = STATUS_BUFFER_TOO_SMALL;
                goto Done;
            }

            Output[index++] = tck;
            cbBytesUsed = index;
            break;
        }

        case phLibNfc_eISO14443_4B_PICC:
        {
            // Inital Header
            Output[index++] = PCSC_ATR_INIT_HEADER;
            Output[index++] = PCSC_ATR_T0 | ((PHNFC_APP_DATA_B_LENGTH + PHNFC_PROT_INFO_B_LENGTH + 1) & PCSC_ATR_T0_MASK);
            Output[index++] = PCSC_ATR_TD1; // TD1
            Output[index++] = PCSC_ATR_TD2; // TD2

            if (PHNFC_MAX_ATR_LENGTH < (index + PHNFC_APP_DATA_B_LENGTH)) {
                status = STATUS_BUFFER_TOO_SMALL;
                goto Done;
            }
            RtlCopyMemory(Output + index,
                pRemDev->RemoteDevInfo.Iso14443B_Info.AtqB.AtqResInfo.AppData,
                PHNFC_APP_DATA_B_LENGTH);

            index += PHNFC_APP_DATA_B_LENGTH;

            if (PHNFC_MAX_ATR_LENGTH < (index + PHNFC_PROT_INFO_B_LENGTH)) {
                status = STATUS_BUFFER_TOO_SMALL;
                goto Done;
            }

            RtlCopyMemory(Output + index,
                pRemDev->RemoteDevInfo.Iso14443B_Info.AtqB.AtqResInfo.ProtInfo,
                PHNFC_PROT_INFO_B_LENGTH);

            index += PHNFC_PROT_INFO_B_LENGTH;

            Output[index++] = 0;

            tck = NfcCxSCInterfaceComputeChecksum(Output, index);

            Output[index++] = tck;
            cbBytesUsed = index;
            break;
        }

        case phLibNfc_eJewel_PICC:
        {
            // Inital Header
            Output[index++] = PCSC_ATR_INIT_HEADER;

            // 15 bytes T0 header
            Output[index++] = PCSC_ATR_T0 | PCSC_ATR_T0_MASK;
            Output[index++] = PCSC_ATR_TD1; // TD1
            Output[index++] = PCSC_ATR_TD2; // TD2
            Output[index++] = PCSC_ATR_STORAGE_CARD_T1; // T1
            Output[index++] = PCSC_ATR_STORAGE_CARD_PRESENCE_INDICATOR; // Application Identifier Presence Indicator
            Output[index++] = PCSC_ATR_STORAGE_CARD_HIST_BYTES_LENGTH; // Length

            Output[index++] = PCSC_ATR_STORAGE_CARD_RID0; // RID 0
            Output[index++] = PCSC_ATR_STORAGE_CARD_RID1; // RID 1
            Output[index++] = PCSC_ATR_STORAGE_CARD_RID2; // RID 2
            Output[index++] = PCSC_ATR_STORAGE_CARD_RID3; // RID 3
            Output[index++] = PCSC_ATR_STORAGE_CARD_RID4; // RID 4

             // 7 Bytes PIX
            Output[index++] = 0x00; // 1 Byte of SS

            if (HINIBBLE(pRemDev->RemoteDevInfo.Jewel_Info.HeaderRom0) != 4) { // 2 Bytes for Card Name
                //
                // Jewel
                //
                Output[index++] = 0x00;
                Output[index++] = PCSC_NN_JEWEL;
            }
            else {
                //
                // Topaz
                //
                Output[index++] = 0x00;
                Output[index++] = PSCS_NN_TOPAZ;
            }

            Output[index++] = 0x00; // 4 Bytes for RFU
            Output[index++] = 0x00;
            Output[index++] = 0x00;
            Output[index++] = 0x00;

            tck = NfcCxSCInterfaceComputeChecksum(Output, index);

            Output[index++] = tck;
            cbBytesUsed = index;
            break;
        }

        case phLibNfc_eFelica_PICC:
        {
            // Inital Header
            Output[index++] = PCSC_ATR_INIT_HEADER;

            // 15 bytes T0 header
            Output[index++] = PCSC_ATR_T0 | PCSC_ATR_T0_MASK;
            Output[index++] = PCSC_ATR_TD1; // TD1
            Output[index++] = PCSC_ATR_TD2; // TD2
            Output[index++] = PCSC_ATR_STORAGE_CARD_T1; // T1
            Output[index++] = PCSC_ATR_STORAGE_CARD_PRESENCE_INDICATOR; // Application Identifier Presence Indicator
            Output[index++] = PCSC_ATR_STORAGE_CARD_HIST_BYTES_LENGTH; // Length

            Output[index++] = PCSC_ATR_STORAGE_CARD_RID0; // RID 0
            Output[index++] = PCSC_ATR_STORAGE_CARD_RID1; // RID 1
            Output[index++] = PCSC_ATR_STORAGE_CARD_RID2; // RID 2
            Output[index++] = PCSC_ATR_STORAGE_CARD_RID3; // RID 3
            Output[index++] = PCSC_ATR_STORAGE_CARD_RID4; // RID 4

            // 7 Bytes PIX
            Output[index++] = PSCS_ATR_SS_FELICA; // 1 Byte of SS
            Output[index++] = 0x00; // 2 Bytes for Card Name
            Output[index++] = PCSC_NN_FELICA;
            Output[index++] = 0x00; // 4 Bytes for RFU
            Output[index++] = 0x00;
            Output[index++] = 0x00;
            Output[index++] = 0x00;

            tck = NfcCxSCInterfaceComputeChecksum(Output, index);

            Output[index++] = tck;
            cbBytesUsed = index;
            break;
        }

        case phLibNfc_eMifare_PICC:
        {
            // Inital Header
            Output[index++] = PCSC_ATR_INIT_HEADER;

            // 15 bytes T0 header
            Output[index++] = PCSC_ATR_T0 | PCSC_ATR_T0_MASK;
            Output[index++] = PCSC_ATR_TD1; // TD1
            Output[index++] = PCSC_ATR_TD2; // TD2
            Output[index++] = PCSC_ATR_STORAGE_CARD_T1; // T1
            Output[index++] = PCSC_ATR_STORAGE_CARD_PRESENCE_INDICATOR; // Application Identifier Presence Indicator
            Output[index++] = PCSC_ATR_STORAGE_CARD_HIST_BYTES_LENGTH; // Length

            Output[index++] = PCSC_ATR_STORAGE_CARD_RID0; // RID 0
            Output[index++] = PCSC_ATR_STORAGE_CARD_RID1; // RID 1
            Output[index++] = PCSC_ATR_STORAGE_CARD_RID2; // RID 2
            Output[index++] = PCSC_ATR_STORAGE_CARD_RID3; // RID 3
            Output[index++] = PCSC_ATR_STORAGE_CARD_RID4; // RID 4

            // 7 Bytes PIX
            Output[index++] = PSCS_ATR_SS_14443A_3; // 1 Byte of SS

            if (pRemDev->RemoteDevInfo.Iso14443A_Info.Sak == SAK_MIFARE_UL) {
                //
                // Mifare UL
                //
                Output[index++] = 0x00;
                if (pRemDev->RemoteDevInfo.Iso14443A_Info.ULType == phNfc_eMifareULType_UltralightC)
                {
                    Output[index++] = PCSC_NN_MIFARE_ULC;
                }
                else if (pRemDev->RemoteDevInfo.Iso14443A_Info.ULType == phNfc_eMifareULType_UltralightEV1)
                {
                    Output[index++] = PCSC_NN_MIFARE_ULEV1;
                }
                else
                {
                    NT_ASSERT(pRemDev->RemoteDevInfo.Iso14443A_Info.ULType == phNfc_eMifareULType_Ultralight);
                    Output[index++] = PCSC_NN_MIFARE_UL;
                }
            }
            else if (pRemDev->RemoteDevInfo.Iso14443A_Info.Sak == SAK_MIFARE_STD_1K) {
                //
                // Mifare Classic 1K
                //
                Output[index++] = 0x00;
                Output[index++] = PCSC_NN_MIFARE_STD_1K;
            }
            else if (pRemDev->RemoteDevInfo.Iso14443A_Info.Sak == SAK_MIFARE_MINI) {
                //
                // Mifare Mini
                //
                Output[index++] = 0x00;
                Output[index++] = PCSC_NN_MIFARE_MINI;
            }
            else if (pRemDev->RemoteDevInfo.Iso14443A_Info.Sak == SAK_MIFARE_PLUS_SL2_2K) {
                //
                // Mifare Plus X SL2 2K
                //
                Output[index++] = 0x00;
                Output[index++] = PCSC_NN_MIFARE_PLUS_SL2_2K;
            }
            else if (pRemDev->RemoteDevInfo.Iso14443A_Info.Sak == SAK_MIFARE_PLUS_SL2_4K) {
                //
                // Mifare Plus X SL2 4K
                //
                Output[index++] = 0x00;
                Output[index++] = PCSC_NN_MIFARE_PLUS_SL2_4K;
            }
            else if (pRemDev->RemoteDevInfo.Iso14443A_Info.Sak == SAK_MIFARE_STD_4K) {
                //
                // Mifare Classic 4K
                //
                Output[index++] = 0x00;
                Output[index++] = PCSC_NN_MIFARE_STD_4K;
            }
            else if (pRemDev->RemoteDevInfo.Iso14443A_Info.Sak == SAK_JCOP_MULTI_PROTOCOL) {
                //
                // JCOP-Multi Protocol Tag
                // Exposing as Mifare Classic 1K 
                //
                Output[index++] = 0x00;
                Output[index++] = PCSC_NN_MIFARE_STD_1K;
            }
            else if (pRemDev->RemoteDevInfo.Iso14443A_Info.Sak == SAK_SMARTMX_MULTI_PROTOCOL) {
                //
                // SmartMx-Multi Protocol Tag
                // Exposing as Mifare Classic 4K
                //
                Output[index++] = 0x00;
                Output[index++] = PCSC_NN_MIFARE_STD_4K;
            }
            else {
                // Any other Tag Type-2 [Mifare Card]
                Output[index++] = 0x00;
                Output[index++] = PCSC_NN_NO_INFO;
            }

            Output[index++] = 0x00; // 4 Bytes for RFU
            Output[index++] = 0x00;
            Output[index++] = 0x00;
            Output[index++] = 0x00;

            tck = NfcCxSCInterfaceComputeChecksum(Output, index);

            Output[index++] = tck;
            cbBytesUsed = index;
            break;
        }

        case phLibNfc_eISO15693_PICC:
        {
            // Inital Header
            Output[index++] = PCSC_ATR_INIT_HEADER;

            // 15 bytes T0 header
            Output[index++] = PCSC_ATR_T0 | PCSC_ATR_T0_MASK;
            Output[index++] = PCSC_ATR_TD1; // TD1
            Output[index++] = PCSC_ATR_TD2; // TD2
            Output[index++] = PCSC_ATR_STORAGE_CARD_T1; // T1
            Output[index++] = PCSC_ATR_STORAGE_CARD_PRESENCE_INDICATOR; // Application Identifier Presence Indicator
            Output[index++] = PCSC_ATR_STORAGE_CARD_HIST_BYTES_LENGTH; // Length

            Output[index++] = PCSC_ATR_STORAGE_CARD_RID0; // RID 0
            Output[index++] = PCSC_ATR_STORAGE_CARD_RID1; // RID 1
            Output[index++] = PCSC_ATR_STORAGE_CARD_RID2; // RID 2
            Output[index++] = PCSC_ATR_STORAGE_CARD_RID3; // RID 3
            Output[index++] = PCSC_ATR_STORAGE_CARD_RID4; // RID 4

            // 7 Bytes PIX
            Output[index++] = PSCS_ATR_SS_ISO15693_4; // 1 Byte of SS

            // 2 Bytes for Card Name
            if (pRemDev->RemoteDevInfo.Iso15693_Info.Uid[6] == ISO15693_MANUFACTURER_NXP)
            {
                if (pRemDev->RemoteDevInfo.Iso15693_Info.Uid[5] == ISO15693_UIDBYTE_5_VALUE_SLI_X ||
                    pRemDev->RemoteDevInfo.Iso15693_Info.Uid[5] == ISO15693_UIDBYTE_5_VALUE_SLI_X_S ||
                    pRemDev->RemoteDevInfo.Iso15693_Info.Uid[5] == ISO15693_UIDBYTE_5_VALUE_SLI_X_L)
                {
                    Output[index++] = 0x00;
                    Output[index++] = PCSC_NN_ICODE_SLI;
                }
                else {
                    Output[index++] = 0x00;
                    Output[index++] = PCSC_NN_NO_INFO;
                }
            }
            else {
                Output[index++] = 0x00;
                Output[index++] = PCSC_NN_NO_INFO;
            }

            Output[index++] = 0x00; // 4 Bytes for RFU
            Output[index++] = 0x00;
            Output[index++] = 0x00;
            Output[index++] = 0x00;

            tck = NfcCxSCInterfaceComputeChecksum(Output, index);

            Output[index++] = tck;
            cbBytesUsed = index;
            break;
        }

        default:
            status = STATUS_NOT_SUPPORTED;
            break;
    }

    if (NT_SUCCESS(status)) {
        if (OutputBufferLength < cbBytesUsed) {
            TRACE_LINE(LEVEL_ERROR, "Output buffer size is too small. Size = %lu, required size = %lu", OutputBufferLength, cbBytesUsed);
            status = STATUS_BUFFER_TOO_SMALL;
            goto Done;
        }

        RtlCopyMemory(OutputBuffer, Output, cbBytesUsed);
        *BytesCopied = cbBytesUsed;
    }

Done:
    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);
    return status;
}

_Requires_lock_held_(ScInterface->SmartCardLock)
NTSTATUS
NfcCxSCInterfaceGetDeviceUidLocked(
    _In_ PNFCCX_SC_INTERFACE ScInterface,
    _Outptr_result_bytebuffer_(*pUidLength) BYTE **ppUid,
    _Out_ BYTE *pUidLength
    )
/*++

Routine Description:

    This routine is called from the SmartCard module to retrieve the remote device UID

Arguments:

    ScInterface - The SC Interface
    ppUid - Pointer to UID array
    pUidLength - Pointer to the length of the UID

Return Value:

    NTSTATUS

--*/
{
    NTSTATUS status = STATUS_SUCCESS;

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    switch (ScInterface->RemoteDeviceInfo.RemDevType)
    {
    case phLibNfc_eJewel_PICC:
        *ppUid = ScInterface->RemoteDeviceInfo.RemoteDevInfo.Jewel_Info.Uid;
        *pUidLength = ScInterface->RemoteDeviceInfo.RemoteDevInfo.Jewel_Info.UidLength;
        break;

    case phLibNfc_eFelica_PICC:
        *ppUid = ScInterface->RemoteDeviceInfo.RemoteDevInfo.Felica_Info.IDm;
        *pUidLength = ScInterface->RemoteDeviceInfo.RemoteDevInfo.Felica_Info.IDmLength;
        break;

    case phLibNfc_eISO15693_PICC:
        *ppUid = ScInterface->RemoteDeviceInfo.RemoteDevInfo.Iso15693_Info.Uid;
        *pUidLength = ScInterface->RemoteDeviceInfo.RemoteDevInfo.Iso15693_Info.UidLength;
        break;

    case phLibNfc_eISO14443_4B_PICC:
        *ppUid = ScInterface->RemoteDeviceInfo.RemoteDevInfo.Iso14443B_Info.AtqB.AtqResInfo.Pupi;
        *pUidLength = PHNFC_PUPI_LENGTH;
        break;

    default:
        *ppUid = ScInterface->RemoteDeviceInfo.RemoteDevInfo.Iso14443A_Info.Uid;
        *pUidLength = ScInterface->RemoteDeviceInfo.RemoteDevInfo.Iso14443A_Info.UidLength;
        break;
    }

    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);

    return status;
}

_Requires_lock_held_(ScInterface->SmartCardLock)
VOID
NfcCxSCInterfaceGetDeviceTypeLocked(
    _In_ PNFCCX_SC_INTERFACE ScInterface,
    _Out_ phNfc_eRemDevType_t* pDevType,
    _Out_ DWORD* pSak
    )
/*++

Routine Description:

    Retrieves the ICC type and SAK of the remote device.

Arguments:

    ScInterface - The SC Interface
    pDevType - Pointer to the remote device type
    pSak - Pointer to the SAK

Return Value:

    NTSTATUS

--*/
{
    *pDevType = ScInterface->RemoteDeviceInfo.RemDevType;
    *pSak = (DWORD)ScInterface->RemoteDeviceInfo.RemoteDevInfo.Iso14443A_Info.Sak;
}

_Requires_lock_held_(ScInterface->SmartCardLock)
BOOLEAN
NfcCxSCInterfaceIsStorageCardConnected(
    _In_ PNFCCX_SC_INTERFACE ScInterface
    )
/*++

Routine Description:

    Determines if the connected smart card is a storage card or not.

Arguments:

    ScInterface - The SC Interface

Return Value:

    TRUE if the connected smart card is a storage card, FALSE otherwise

--*/
{
    NT_ASSERT(ScInterface->SmartCardConnected);

    return (ScInterface->RemoteDeviceInfo.RemDevType == phLibNfc_eISO14443_4A_PICC ||
        ScInterface->RemoteDeviceInfo.RemDevType == phLibNfc_eISO14443_4B_PICC) ? FALSE : TRUE;
}

_Requires_lock_held_(ScInterface->SmartCardLock)
NTSTATUS
NfcCxSCInterfaceGetIccTypePerAtrLocked(
    _In_ PNFCCX_SC_INTERFACE ScInterface,
    _Out_ BYTE* pIccTypePerAtr
    )
/*++

Routine Description:

    This routine is called from the SmartCard module to retrieve the ICC type of the remote device

Arguments:

    ScInterface - The SC Interface
    pIccTypePerAtr - Pointer to the ICC type per ATR

Return Value:

    NTSTATUS

--*/
{
    NTSTATUS status = STATUS_SUCCESS;

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    switch (ScInterface->RemoteDeviceInfo.RemDevType)
    {
    case phLibNfc_eISO14443_4A_PICC:
    case phLibNfc_eISO14443_3A_PICC:
    case phLibNfc_eISO14443_A_PICC:
        *pIccTypePerAtr = ICC_TYPE_14443_TYPE_A; // 14443 Type A
        break;

    case phLibNfc_eISO14443_B_PICC:
    case phLibNfc_eISO14443_4B_PICC:
    case phLibNfc_eISO14443_BPrime_PICC:
        *pIccTypePerAtr = ICC_TYPE_14443_TYPE_B; // 14443 Type B
        break;

    case phLibNfc_eISO15693_PICC:
        *pIccTypePerAtr = ICC_TYPE_ISO_15693; // ISO 15693
        break;

    default:
        *pIccTypePerAtr = ICC_TYPE_UNKNOWN; 
        break;
    }

    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);
    return status;
}

_Requires_lock_not_held_(ScInterface->SmartCardLock)
NTSTATUS
NfcCxSCInterfaceResetCard(
    _In_ PNFCCX_SC_INTERFACE ScInterface
    )
/*++

Routine Description:

    This routine warm resets the smart card

Arguments:

    ScInterface - The SC Interface

Return Value:

    NTSTATUS

--*/
{
    NTSTATUS status = STATUS_SUCCESS;

    TRACE_FUNCTION_ENTRY(LEVEL_VERBOSE);

    NfcCxRFInterfaceTargetReactivate(ScInterface->FdoContext->RFInterface);
    ScInterface->SessionEstablished = FALSE;

    TRACE_FUNCTION_EXIT_NTSTATUS(LEVEL_VERBOSE, status);

    return status;
}

BYTE
NfcCxSCInterfaceComputeChecksum(
    _In_reads_bytes_(cbAtr) BYTE* pAtr,
    _In_ DWORD cbAtr
    )
/*++

Routine Description:

    This routine computes the checksum for the ATR

 --*/
{
    BYTE tck = 0;

    // TCK is Exclusive-OR of bytes T0 to Tk. T0 index is 1
    for (uint32_t i = 1; i < cbAtr; i++) {
        tck ^= pAtr[i];
    }

    return tck;
}
