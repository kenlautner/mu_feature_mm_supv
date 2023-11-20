/** @file
  SMM STM support functions

  Copyright (c) 2015 - 2018, Intel Corporation. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <PiMm.h>
#include <SpamResponder.h>
#include <Library/FvLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/HobLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MmServicesTableLib.h>
#include <Library/TpmMeasurementLib.h>
#include <Guid/MpInformation.h>
#include <Protocol/MmEndOfDxe.h>
#include <Register/Intel/Cpuid.h>
#include <Register/Intel/ArchitecturalMsr.h>
#include <Register/Intel/SmramSaveStateMap.h>

#include <Protocol/MpService.h>

#include "CpuFeaturesLib.h"
#include "SmmStm.h"

#define TXT_EVTYPE_BASE      0x400
#define TXT_EVTYPE_STM_HASH  (TXT_EVTYPE_BASE + 14)

#define CODE_SEL  0x38
#define DATA_SEL  0x40
#define TR_SEL    0x68

#define RDWR_ACCS  3
#define FULL_ACCS  7

BOOLEAN  mLockLoadMonitor = FALSE;

//
// Template of STM_RSC_END structure for copying.
//
GLOBAL_REMOVE_IF_UNREFERENCED STM_RSC_END  mRscEndNode = {
  { END_OF_RESOURCES, sizeof (STM_RSC_END) },
};

GLOBAL_REMOVE_IF_UNREFERENCED UINT8  *mStmResourcesPtr         = NULL;
GLOBAL_REMOVE_IF_UNREFERENCED UINTN  mStmResourceTotalSize     = 0x0;
GLOBAL_REMOVE_IF_UNREFERENCED UINTN  mStmResourceSizeUsed      = 0x0;
GLOBAL_REMOVE_IF_UNREFERENCED UINTN  mStmResourceSizeAvailable = 0x0;

GLOBAL_REMOVE_IF_UNREFERENCED UINT32  mStmState = 0;

//
// System Configuration Table pointing to STM Configuration Table
//
GLOBAL_REMOVE_IF_UNREFERENCED
EFI_SM_MONITOR_INIT_PROTOCOL  mSmMonitorInitProtocol = {
  .LoadMonitor = LoadMonitor,
  .GetMonitorState = GetMonitorState,
};

#define   CPUID1_EDX_XD_SUPPORT  0x100000

//
// External global variables associated with SMI Handler Template
//
extern CONST TXT_PROCESSOR_SMM_DESCRIPTOR  gcStmPsd;
extern UINT32                              gStmSmbase;
extern volatile UINT32                     gStmSmiStack;
extern UINT32                              gStmSmiCr3;

//
// Global variables and symbols pulled in from MmSupervisor
//
extern BOOLEAN mCetSupported;
extern BOOLEAN gXdSupported;
extern BOOLEAN gPatchMsrIa32MiscEnableSupported;
extern BOOLEAN gPatch5LevelPagingNeeded;

extern UINT32 mCetPl0Ssp;
extern UINT32 mCetInterruptSsp;
extern UINT32 mCetInterruptSspTable;

VOID
EFIAPI
CpuSmmDebugEntry (
  IN UINTN  CpuIndex
  );

VOID
EFIAPI
CpuSmmDebugExit (
  IN UINTN  CpuIndex
  );

VOID
EFIAPI
SmiRendezvous (
  IN      UINTN  CpuIndex
  );

VOID
EFIAPI
OnStmSetup (
  VOID
  );

VOID
EFIAPI
OnStmTeardown (
  VOID
  );

VOID
EFIAPI
OnException (
  VOID
  );

//
// This structure serves as a template for all processors.
//
CONST TXT_PROCESSOR_SMM_DESCRIPTOR mPsdTemplate = {
  .Signature = TXT_PROCESSOR_SMM_DESCRIPTOR_SIGNATURE,
  .Size = sizeof (TXT_PROCESSOR_SMM_DESCRIPTOR),
  .SmmDescriptorVerMajor = TXT_PROCESSOR_SMM_DESCRIPTOR_VERSION_MAJOR,
  .SmmDescriptorVerMinor = TXT_PROCESSOR_SMM_DESCRIPTOR_VERSION_MINOR,
  .LocalApicId = 0,
  .SmmEntryState = 0x0F, // Cr4Pse;Cr4Pae;Intel64Mode;ExecutionDisableOutsideSmrr
  .SmmResumeState = 0, // BIOS to STM
  .StmSmmState = 0, // STM to BIOS
  .Reserved4 = 0,
  .SmmCs = CODE_SEL,
  .SmmDs = DATA_SEL,
  .SmmSs = DATA_SEL,
  .SmmOtherSegment = DATA_SEL,
  .SmmTr = TR_SEL,
  .Reserved5 = 0,
  .SmmCr3 = 0,
  .SmmStmSetupRip = (UINT64)OnStmSetup,
  .SmmStmTeardownRip = (UINT64)OnStmTeardown,
  .SmmSmiHandlerRip = 0, // SmmSmiHandlerRip - SMM guest entrypoint
  .SmmSmiHandlerRsp = 0, // SmmSmiHandlerRsp
  .SmmGdtPtr = 0,
  .SmmGdtSize = 0,
  .RequiredStmSmmRevId = 0x80010100,
  .StmProtectionExceptionHandler = {
    .SpeRip = (UINT64)OnException,
    .SpeRsp = 0,
    .SpeSs = DATA_SEL,
    .PageViolationException = 1,
    .MsrViolationException = 1,
    .RegisterViolationException = 1,
    .IoViolationException = 1,
    .PciViolationException = 1,
  },
  .Reserved6 = 0,
  .BiosHwResourceRequirementsPtr = 0,
  .AcpiRsdp = 0,
  .PhysicalAddressBits = 0,
};

//
// Variables used by SMI Handler
//
IA32_DESCRIPTOR  gStmSmiHandlerIdtr;

//
// MP Information HOB data
//
MP_INFORMATION_HOB_DATA  *mMpInformationHobData;

//
// MSEG Base and Length in SMRAM
//
UINTN  mMsegBase = 0;
UINTN  mMsegSize = 0;

//
// MMI Entry Base and Length in FV
//
EFI_PHYSICAL_ADDRESS  mMmiEntryBaseAddress  = 0;
UINTN                 mMmiEntrySize         = 0;

BOOLEAN  mStmConfigurationTableInitialized = FALSE;

/**
  Discovers Standalone MM drivers in FV HOBs and adds those drivers to the Standalone MM
  dispatch list.

  This function will also set the Standalone MM BFV address to the FV that contains this
  Standalone MM core driver.

  @retval   EFI_SUCCESS           An error was not encountered discovering Standalone MM drivers.
  @retval   EFI_NOT_FOUND         The HOB list could not be found.

**/
EFI_STATUS
DiscoverSmiEntryInFvHobs (
  VOID
  )
{
  UINT16                          ExtHeaderOffset;
  EFI_FIRMWARE_VOLUME_HEADER      *FwVolHeader;
  EFI_FIRMWARE_VOLUME_EXT_HEADER  *ExtHeader;
  EFI_FFS_FILE_HEADER             *FileHeader;
  EFI_PEI_HOB_POINTERS            Hob;
  EFI_STATUS                      Status;

  Hob.Raw = GetHobList ();
  if (Hob.Raw == NULL) {
    Status = EFI_NOT_FOUND;
    goto Done;
  }

  do {
    Hob.Raw = GetNextHob (EFI_HOB_TYPE_FV, Hob.Raw);
    if (Hob.Raw != NULL) {
      FwVolHeader = (EFI_FIRMWARE_VOLUME_HEADER *)(UINTN)(Hob.FirmwareVolume->BaseAddress);

      DEBUG ((
        DEBUG_INFO,
        "[%a] Found FV HOB referencing FV at 0x%x. Size is 0x%x.\n",
        __FUNCTION__,
        (UINTN)FwVolHeader,
        FwVolHeader->FvLength
        ));

      ExtHeaderOffset = ReadUnaligned16 (&FwVolHeader->ExtHeaderOffset);
      if (ExtHeaderOffset != 0) {
        ExtHeader = (EFI_FIRMWARE_VOLUME_EXT_HEADER *)((UINT8 *)FwVolHeader + ExtHeaderOffset);
        DEBUG ((DEBUG_INFO, "[%a]   FV GUID = {%g}.\n", __FUNCTION__, &ExtHeader->FvName));
      }

      //
      // If a MM_STANDALONE or MM_CORE_STANDALONE driver is in the FV. Add the drivers
      // to the dispatch list. Mark the FV with this driver as the Standalone BFV.
      //
      FileHeader = NULL;
      Status     =  FfsFindNextFile (
                      EFI_FV_FILETYPE_RAW,
                      FwVolHeader,
                      &FileHeader
                      );
      if (!EFI_ERROR (Status)) {
        if (CompareGuid (&FileHeader->Name, &gMmiEntrySpamFileGuid)) {
          mMmiEntryBaseAddress  = (EFI_PHYSICAL_ADDRESS)(UINTN)FileHeader;
          mMmiEntrySize         = 0;
          CopyMem (&mMmiEntrySize, FileHeader->Size, sizeof (FileHeader->Size));
          DEBUG ((
            DEBUG_INFO,
            "[%a]   Discovered MMI Entry for SPAM [%g] in FV at 0x%p of %x bytes.\n",
            __FUNCTION__,
            &gMmiEntrySpamFileGuid,
            mMmiEntryBaseAddress,
            mMmiEntrySize
            ));
          Status = EFI_SUCCESS;
          break;
        }
      }
      Hob.Raw = GetNextHob (EFI_HOB_TYPE_FV, GET_NEXT_HOB (Hob));
    }
  } while (Hob.Raw != NULL);

Done:
  return Status;
}

/**
  The constructor function for the Traditional MM library instance with STM.

  @param[in]  ImageHandle  The firmware allocated handle for the EFI image.
  @param[in]  SystemTable  A pointer to the EFI System Table.

  @retval EFI_SUCCESS      The constructor always returns EFI_SUCCESS.

**/
EFI_STATUS
EFIAPI
SmmCpuFeaturesLibStmConstructor (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS              Status;
  CPUID_VERSION_INFO_ECX  RegEcx;
  EFI_HOB_GUID_TYPE       *GuidHob;
  EFI_SMRAM_DESCRIPTOR    *SmramDescriptor;

  //
  // First locate the MMI entry blob in the FV
  Status = DiscoverSmiEntryInFvHobs ();
  if (EFI_ERROR (Status)) {
    ASSERT_EFI_ERROR (Status);
    return Status;
  }

  //
  // Perform library initialization common across all instances
  //
  CpuFeaturesLibInitialization ();

  //
  // Lookup the MP Information
  //
  GuidHob = GetFirstGuidHob (&gMpInformationHobGuid);
  ASSERT (GuidHob != NULL);
  mMpInformationHobData = GET_GUID_HOB_DATA (GuidHob);

  //
  // If CPU supports VMX, then determine SMRAM range for MSEG.
  //
  AsmCpuid (CPUID_VERSION_INFO, NULL, NULL, &RegEcx.Uint32, NULL);
  if (RegEcx.Bits.VMX == 1) {
    GuidHob = GetFirstGuidHob (&gMsegSmramGuid);
    if (GuidHob != NULL) {
      //
      // Retrieve MSEG location from MSEG SRAM HOB
      //
      SmramDescriptor = (EFI_SMRAM_DESCRIPTOR *)GET_GUID_HOB_DATA (GuidHob);
      if (SmramDescriptor->PhysicalSize > 0) {
        mMsegBase = (UINTN)SmramDescriptor->CpuStart;
        mMsegSize = (UINTN)SmramDescriptor->PhysicalSize;
      }
    } else if (PcdGet32 (PcdCpuMsegSize) > 0) {
      //
      // Allocate MSEG from SMRAM memory
      //
      mMsegBase = (UINTN)AllocatePages (EFI_SIZE_TO_PAGES (PcdGet32 (PcdCpuMsegSize)));
      if (mMsegBase > 0) {
        mMsegSize = ALIGN_VALUE (PcdGet32 (PcdCpuMsegSize), EFI_PAGE_SIZE);
      } else {
        DEBUG ((DEBUG_ERROR, "Not enough SMRAM resource to allocate MSEG size %08x\n", PcdGet32 (PcdCpuMsegSize)));
      }
    }

    if (mMsegBase > 0) {
      DEBUG ((DEBUG_INFO, "MsegBase: 0x%08x, MsegSize: 0x%08x\n", mMsegBase, mMsegSize));
    }
  }

  return EFI_SUCCESS;
}

/**
  Internal worker function that is called to complete CPU initialization at the
  end of SmmCpuFeaturesInitializeProcessor().

**/
VOID
FinishSmmCpuFeaturesInitializeProcessor (
  VOID
  )
{
  MSR_IA32_SMM_MONITOR_CTL_REGISTER  SmmMonitorCtl;

  //
  // Set MSEG Base Address in SMM Monitor Control MSR.
  //
  if (mMsegBase > 0) {
    SmmMonitorCtl.Uint64        = 0;
    SmmMonitorCtl.Bits.MsegBase = (UINT32)mMsegBase >> 12;
    SmmMonitorCtl.Bits.Valid    = 1;
    AsmWriteMsr64 (MSR_IA32_SMM_MONITOR_CTL, SmmMonitorCtl.Uint64);
  }
}

/**
  Return the size, in bytes, of a custom SMI Handler in bytes.  If 0 is
  returned, then a custom SMI handler is not provided by this library,
  and the default SMI handler must be used.

  @retval 0    Use the default SMI handler.
  @retval > 0  Use the SMI handler installed by SmmCpuFeaturesInstallSmiHandler()
               The caller is required to allocate enough SMRAM for each CPU to
               support the size of the custom SMI handler.
**/
UINTN
EFIAPI
SmmCpuFeaturesGetSmiHandlerSize (
  VOID
  )
{
  return mMmiEntrySize;
}

/**
  Install a custom SMI handler for the CPU specified by CpuIndex.  This function
  is only called if SmmCpuFeaturesGetSmiHandlerSize() returns a size is greater
  than zero and is called by the CPU that was elected as monarch during System
  Management Mode initialization.

  @param[in] CpuIndex   The index of the CPU to install the custom SMI handler.
                        The value must be between 0 and the NumberOfCpus field
                        in the System Management System Table (SMST).
  @param[in] SmBase     The SMBASE address for the CPU specified by CpuIndex.
  @param[in] SmiStack   The stack to use when an SMI is processed by the
                        the CPU specified by CpuIndex.
  @param[in] StackSize  The size, in bytes, if the stack used when an SMI is
                        processed by the CPU specified by CpuIndex.
  @param[in] GdtBase    The base address of the GDT to use when an SMI is
                        processed by the CPU specified by CpuIndex.
  @param[in] GdtSize    The size, in bytes, of the GDT used when an SMI is
                        processed by the CPU specified by CpuIndex.
  @param[in] IdtBase    The base address of the IDT to use when an SMI is
                        processed by the CPU specified by CpuIndex.
  @param[in] IdtSize    The size, in bytes, of the IDT used when an SMI is
                        processed by the CPU specified by CpuIndex.
  @param[in] Cr3        The base address of the page tables to use when an SMI
                        is processed by the CPU specified by CpuIndex.
**/
VOID
EFIAPI
SmmCpuFeaturesInstallSmiHandler (
  IN UINTN   CpuIndex,
  IN UINT32  SmBase,
  IN VOID    *SmiStack,
  IN UINTN   StackSize,
  IN UINTN   GdtBase,
  IN UINTN   GdtSize,
  IN UINTN   IdtBase,
  IN UINTN   IdtSize,
  IN UINT32  Cr3
  )
{
  TXT_PROCESSOR_SMM_DESCRIPTOR  *Psd;
  VOID                          *Hob;
  UINT32                        RegEax;
  EFI_PROCESSOR_INFORMATION     ProcessorInfo;
  PER_CORE_MMI_ENTRY_STRUCT_HDR *SmiEntryStructHdrPtr = NULL;
  UINT32                        SmiEntryStructHdrAddr;
  UINT32                        WholeStructSize;
  UINT16                        *FixStructPtr;
  UINT32                        *Fixup32Ptr;
  UINT64                        *Fixup64Ptr;
  UINT8                         *Fixup8Ptr;

  CopyMem ((VOID *)((UINTN)SmBase + TXT_SMM_PSD_OFFSET), &gcStmPsd, sizeof (gcStmPsd));
  Psd             = (TXT_PROCESSOR_SMM_DESCRIPTOR *)(VOID *)((UINTN)SmBase + TXT_SMM_PSD_OFFSET);
  Psd->SmmGdtPtr  = GdtBase;
  Psd->SmmGdtSize = (UINT32)GdtSize;

  //
  // Initialize values in template before copy
  //
  gStmSmiStack             = (UINT32)((UINTN)SmiStack + StackSize - sizeof (UINTN));
  gStmSmiCr3               = Cr3;
  gStmSmbase               = SmBase;
  gStmSmiHandlerIdtr.Base  = IdtBase;
  gStmSmiHandlerIdtr.Limit = (UINT16)(IdtSize - 1);

  //
  // Set the value at the top of the CPU stack to the CPU Index
  //
  *(UINTN *)(UINTN)gStmSmiStack = CpuIndex;

  //
  // Copy template to CPU specific SMI handler location from what is located from the FV
  //
  CopyMem (
    (VOID *)((UINTN)SmBase + SMM_HANDLER_OFFSET),
    (VOID *)mMmiEntryBaseAddress,
    mMmiEntrySize
    );

  // Populate the fix up addresses
  // Get Whole structure size
  WholeStructSize = (UINT32)*(EFI_PHYSICAL_ADDRESS *)(UINTN)(SmBase + SMM_HANDLER_OFFSET + mMmiEntrySize - sizeof(UINT32));

  // Get header address
  SmiEntryStructHdrAddr = (UINT32)(SmBase + SMM_HANDLER_OFFSET + mMmiEntrySize - sizeof(UINT32) - WholeStructSize);
  SmiEntryStructHdrPtr = (PER_CORE_MMI_ENTRY_STRUCT_HDR *)(UINTN)(SmiEntryStructHdrAddr);

  // Navegiate to the fixup arrays
  FixStructPtr = (UINT16 *)(UINTN)(SmiEntryStructHdrAddr + SmiEntryStructHdrPtr->FixUpStructOffset);
  Fixup32Ptr = (UINT32 *)(UINTN)(SmiEntryStructHdrAddr + SmiEntryStructHdrPtr->FixUp32Offset);
  Fixup64Ptr = (UINT64 *)(UINTN)(SmiEntryStructHdrAddr + SmiEntryStructHdrPtr->FixUp64Offset);

  //Do the fixup
  
  Fixup32Ptr[FIXUP32_mPatchCetPl0Ssp] = mCetPl0Ssp;
  Fixup32Ptr[FIXUP32_GDTR] = (UINT32)GdtBase;
  Fixup32Ptr[FIXUP32_CR3_OFFSET] = Cr3;
  Fixup32Ptr[FIXUP32_mPatchCetInterruptSsp] = mCetInterruptSsp;
  Fixup32Ptr[FIXUP32_mPatchCetInterruptSspTable] = mCetInterruptSspTable;
  Fixup32Ptr[FIXUP32_STACK_OFFSET_CPL0] = (UINT32)(UINTN)SmiStack;
  Fixup32Ptr[FIXUP32_MSR_SMM_BASE] = SmBase;

  Fixup64Ptr[FIXUP64_SMM_DBG_ENTRY] = (UINT64)CpuSmmDebugEntry;
  Fixup64Ptr[FIXUP64_SMM_DBG_EXIT] = (UINT64)CpuSmmDebugExit;
  Fixup64Ptr[FIXUP64_SMI_RDZ_ENTRY] = (UINT64)SmiRendezvous;
  Fixup64Ptr[FIXUP64_XD_SUPPORTED] = (UINT64)&gXdSupported;
  Fixup64Ptr[FIXUP64_CET_SUPPORTED] = (UINT64)&mCetSupported;
  Fixup64Ptr[FIXUP64_SMI_HANDLER_IDTR] = IdtBase;

  Fixup8Ptr[FIXUP8_gPatchXdSupported] = gXdSupported;
  Fixup8Ptr[FIXUP8_gPatchMsrIa32MiscEnableSupported] = gPatchMsrIa32MiscEnableSupported;
  Fixup8Ptr[FIXUP8_gPatch5LevelPagingNeeded] = gPatch5LevelPagingNeeded;
  Fixup8Ptr[FIXUP8_mPatchCetSupported] = mCetSupported;

  // TODO: Sort out this values, if needed
  Psd->SmmSmiHandlerRip = 0;
  Psd->SmmSmiHandlerRsp = (UINTN)SmiStack + StackSize - sizeof (UINTN);
  Psd->SmmCr3           = Cr3;

  DEBUG ((DEBUG_INFO, "CpuSmmStmExceptionStackSize - %x\n", PcdGet32 (PcdCpuSmmStmExceptionStackSize)));
  DEBUG ((DEBUG_INFO, "Pages - %x\n", EFI_SIZE_TO_PAGES (PcdGet32 (PcdCpuSmmStmExceptionStackSize))));
  Psd->StmProtectionExceptionHandler.SpeRsp  = (UINT64)(UINTN)AllocatePages (EFI_SIZE_TO_PAGES (PcdGet32 (PcdCpuSmmStmExceptionStackSize)));
  Psd->StmProtectionExceptionHandler.SpeRsp += EFI_PAGES_TO_SIZE (EFI_SIZE_TO_PAGES (PcdGet32 (PcdCpuSmmStmExceptionStackSize)));

  Psd->BiosHwResourceRequirementsPtr = (UINT64)(UINTN)GetStmResource ();

  //
  // Get the APIC ID for the CPU specified by CpuIndex
  //
  CopyMem (&ProcessorInfo, &mMpInformationHobData->ProcessorInfoBuffer[CpuIndex], sizeof (EFI_PROCESSOR_INFORMATION));

  Psd->LocalApicId = (UINT32)ProcessorInfo.ProcessorId;
  Psd->AcpiRsdp    = 0;

  Hob = GetFirstHob (EFI_HOB_TYPE_CPU);
  if (Hob != NULL) {
    Psd->PhysicalAddressBits = ((EFI_HOB_CPU *)Hob)->SizeOfMemorySpace;
  } else {
    AsmCpuid (0x80000000, &RegEax, NULL, NULL, NULL);
    if (RegEax >= 0x80000008) {
      AsmCpuid (0x80000008, &RegEax, NULL, NULL, NULL);
      Psd->PhysicalAddressBits = (UINT8)RegEax;
    } else {
      Psd->PhysicalAddressBits = 36;
    }
  }

  if (!mStmConfigurationTableInitialized) {
    StmSmmConfigurationTableInit ();
    mStmConfigurationTableInitialized = TRUE;
  }
}

/**
  SMM End Of Dxe event notification handler.

  STM support need patch AcpiRsdp in TXT_PROCESSOR_SMM_DESCRIPTOR.

  @param[in] Protocol   Points to the protocol's unique identifier.
  @param[in] Interface  Points to the interface instance.
  @param[in] Handle     The handle on which the interface was installed.

  @retval EFI_SUCCESS   Notification handler runs successfully.
**/
EFI_STATUS
EFIAPI
MmEndOfDxeEventNotify (
  IN CONST EFI_GUID  *Protocol,
  IN VOID            *Interface,
  IN EFI_HANDLE      Handle
  )
{
  UINTN                         Index;
  TXT_PROCESSOR_SMM_DESCRIPTOR  *Psd;

  DEBUG ((DEBUG_INFO, "MmEndOfDxeEventNotify\n"));

  for (Index = 0; Index < gMmst->NumberOfCpus; Index++) {
    Psd = (TXT_PROCESSOR_SMM_DESCRIPTOR *)((UINTN)gMmst->CpuSaveState[Index] - SMRAM_SAVE_STATE_MAP_OFFSET + TXT_SMM_PSD_OFFSET);
    DEBUG ((DEBUG_INFO, "Index=%d  Psd=%p  Rsdp=%p\n", Index, Psd, NULL));
    Psd->AcpiRsdp = (UINT64)(UINTN)NULL;
  }

  mLockLoadMonitor = TRUE;

  return EFI_SUCCESS;
}

/**
  This function initializes the STM configuration table.
**/
VOID
StmSmmConfigurationTableInit (
  VOID
  )
{
  EFI_STATUS  Status;
  VOID        *Registration;

  //
  //
  // Register SMM End of DXE Event
  //
  Status = gMmst->MmRegisterProtocolNotify (
                    &gEfiMmEndOfDxeProtocolGuid,
                    MmEndOfDxeEventNotify,
                    &Registration
                    );
  ASSERT_EFI_ERROR (Status);
}

/**

  Set valid bit for MSEG MSR.

  @param Buffer Ap function buffer. (not used)

**/
VOID
EFIAPI
EnableMsegMsr (
  IN VOID  *Buffer
  )
{
  MSR_IA32_SMM_MONITOR_CTL_REGISTER  SmmMonitorCtl;

  SmmMonitorCtl.Uint64     = AsmReadMsr64 (MSR_IA32_SMM_MONITOR_CTL);
  SmmMonitorCtl.Bits.Valid = 1;
  AsmWriteMsr64 (MSR_IA32_SMM_MONITOR_CTL, SmmMonitorCtl.Uint64);
}

/**

  Get 4K page aligned VMCS size.

  @return 4K page aligned VMCS size

**/
UINT32
GetVmcsSize (
  VOID
  )
{
  MSR_IA32_VMX_BASIC_REGISTER  VmxBasic;

  //
  // Read VMCS size and and align to 4KB
  //
  VmxBasic.Uint64 = AsmReadMsr64 (MSR_IA32_VMX_BASIC);
  return ALIGN_VALUE (VmxBasic.Bits.VmcsSize, SIZE_4KB);
}

/**

  Check STM image size.

  @param StmImage      STM image
  @param StmImageSize  STM image size

  @retval TRUE  check pass
  @retval FALSE check fail
**/
BOOLEAN
StmCheckStmImage (
  IN EFI_PHYSICAL_ADDRESS  StmImage,
  IN UINTN                 StmImageSize
  )
{
  UINTN                   MinMsegSize;
  STM_HEADER              *StmHeader;
  IA32_VMX_MISC_REGISTER  VmxMiscMsr;

  //
  // Check to see if STM image is compatible with CPU
  //
  StmHeader         = (STM_HEADER *)(UINTN)StmImage;
  VmxMiscMsr.Uint64 = AsmReadMsr64 (MSR_IA32_VMX_MISC);
  if (StmHeader->HwStmHdr.MsegHeaderRevision != VmxMiscMsr.Bits.MsegRevisionIdentifier) {
    DEBUG ((DEBUG_ERROR, "STM Image not compatible with CPU\n"));
    DEBUG ((DEBUG_ERROR, "  StmHeader->HwStmHdr.MsegHeaderRevision = %08x\n", StmHeader->HwStmHdr.MsegHeaderRevision));
    DEBUG ((DEBUG_ERROR, "  VmxMiscMsr.Bits.MsegRevisionIdentifier = %08x\n", VmxMiscMsr.Bits.MsegRevisionIdentifier));
    return FALSE;
  }

  //
  // Get Minimal required Mseg size
  //
  MinMsegSize = (EFI_PAGES_TO_SIZE (EFI_SIZE_TO_PAGES (StmHeader->SwStmHdr.StaticImageSize)) +
                 StmHeader->SwStmHdr.AdditionalDynamicMemorySize +
                 (StmHeader->SwStmHdr.PerProcDynamicMemorySize + GetVmcsSize () * 2) * gMmst->NumberOfCpus);
  if (MinMsegSize < StmImageSize) {
    MinMsegSize = StmImageSize;
  }

  if (StmHeader->HwStmHdr.Cr3Offset >= StmHeader->SwStmHdr.StaticImageSize) {
    //
    // We will create page table, just in case that SINIT does not create it.
    //
    if (MinMsegSize < StmHeader->HwStmHdr.Cr3Offset + EFI_PAGES_TO_SIZE (6)) {
      MinMsegSize = StmHeader->HwStmHdr.Cr3Offset + EFI_PAGES_TO_SIZE (6);
    }
  }

  //
  // Check if it exceeds MSEG size
  //
  if (MinMsegSize > mMsegSize) {
    DEBUG ((DEBUG_ERROR, "MSEG too small.  Min MSEG Size = %08x  Current MSEG Size = %08x\n", MinMsegSize, mMsegSize));
    DEBUG ((DEBUG_ERROR, "  StmHeader->SwStmHdr.StaticImageSize             = %08x\n", StmHeader->SwStmHdr.StaticImageSize));
    DEBUG ((DEBUG_ERROR, "  StmHeader->SwStmHdr.AdditionalDynamicMemorySize = %08x\n", StmHeader->SwStmHdr.AdditionalDynamicMemorySize));
    DEBUG ((DEBUG_ERROR, "  StmHeader->SwStmHdr.PerProcDynamicMemorySize    = %08x\n", StmHeader->SwStmHdr.PerProcDynamicMemorySize));
    DEBUG ((DEBUG_ERROR, "  VMCS Size                                       = %08x\n", GetVmcsSize ()));
    DEBUG ((DEBUG_ERROR, "  Max CPUs                                        = %08x\n", gMmst->NumberOfCpus));
    DEBUG ((DEBUG_ERROR, "  StmHeader->HwStmHdr.Cr3Offset                   = %08x\n", StmHeader->HwStmHdr.Cr3Offset));
    return FALSE;
  }

  return TRUE;
}

/**

  Load STM image to MSEG.

  @param StmImage      STM image
  @param StmImageSize  STM image size

**/
VOID
StmLoadStmImage (
  IN EFI_PHYSICAL_ADDRESS  StmImage,
  IN UINTN                 StmImageSize
  )
{
  MSR_IA32_SMM_MONITOR_CTL_REGISTER  SmmMonitorCtl;
  UINT32                             MsegBase;
  STM_HEADER                         *StmHeader;

  //
  // Get MSEG base address from MSR_IA32_SMM_MONITOR_CTL
  //
  SmmMonitorCtl.Uint64 = AsmReadMsr64 (MSR_IA32_SMM_MONITOR_CTL);
  MsegBase             = SmmMonitorCtl.Bits.MsegBase << 12;

  //
  // Zero all of MSEG base address
  //
  ZeroMem ((VOID *)(UINTN)MsegBase, mMsegSize);

  //
  // Copy STM Image into MSEG
  //
  CopyMem ((VOID *)(UINTN)MsegBase, (VOID *)(UINTN)StmImage, StmImageSize);

  //
  // STM Header is at the beginning of the STM Image
  //
  StmHeader = (STM_HEADER *)(UINTN)StmImage;

  StmGen4GPageTable ((UINTN)MsegBase + StmHeader->HwStmHdr.Cr3Offset);
}

/**

  Load STM image to MSEG.

  @param StmImage      STM image
  @param StmImageSize  STM image size

  @retval EFI_SUCCESS            Load STM to MSEG successfully
  @retval EFI_ALREADY_STARTED    STM image is already loaded to MSEG
  @retval EFI_BUFFER_TOO_SMALL   MSEG is smaller than minimal requirement of STM image
  @retval EFI_UNSUPPORTED        MSEG is not enabled

**/
EFI_STATUS
EFIAPI
LoadMonitor (
  IN EFI_PHYSICAL_ADDRESS  StmImage,
  IN UINTN                 StmImageSize
  )
{
  MSR_IA32_SMM_MONITOR_CTL_REGISTER  SmmMonitorCtl;

  if (mLockLoadMonitor) {
    return EFI_ACCESS_DENIED;
  }

  SmmMonitorCtl.Uint64 = AsmReadMsr64 (MSR_IA32_SMM_MONITOR_CTL);
  if (SmmMonitorCtl.Bits.MsegBase == 0) {
    return EFI_UNSUPPORTED;
  }

  if (!StmCheckStmImage (StmImage, StmImageSize)) {
    return EFI_BUFFER_TOO_SMALL;
  }

  // Record STM_HASH to PCR 0, just in case it is NOT TXT launch, we still need provide the evidence.
  TpmMeasureAndLogData (
    0,                        // PcrIndex
    TXT_EVTYPE_STM_HASH,      // EventType
    NULL,                     // EventLog
    0,                        // LogLen
    (VOID *)(UINTN)StmImage,  // HashData
    StmImageSize              // HashDataLen
    );

  StmLoadStmImage (StmImage, StmImageSize);

  mStmState |= EFI_SM_MONITOR_STATE_ENABLED;

  return EFI_SUCCESS;
}

/**
  This function return BIOS STM resource.
  Produced by SmmStm.
  Consumed by SmmMpService when Init.

  @return BIOS STM resource

**/
VOID *
GetStmResource (
  VOID
  )
{
  return mStmResourcesPtr;
}

/**
  This is STM setup BIOS callback.
**/
VOID
EFIAPI
SmmStmSetup (
  VOID
  )
{
  mStmState |= EFI_SM_MONITOR_STATE_ACTIVATED;
}

/**
  This is STM teardown BIOS callback.
**/
VOID
EFIAPI
SmmStmTeardown (
  VOID
  )
{
  mStmState &= ~EFI_SM_MONITOR_STATE_ACTIVATED;
}
