/** @file
 
  
  Interactive PCI Tool for UEFI Shell.
  Features:
  - Scan all PCI devices (Bus 0-255)
  - View Config Space (Hex Dump)
  - Write to Config Space (Byte/Word/Dword)
  - Raw Write support (No artificial masks on Cmd/Status regs)
**/

#include <Uefi.h>
#include <Protocol/PciRootBridgeIo.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>

#define MAX_PCI_DEVS  4096

// Display Modes
typedef enum {
  DISP_BYTE  = 0,
  DISP_WORD  = 1,
  DISP_DWORD = 2
} DISP_MODE;

// Stored Device Information
typedef struct {
  UINT8  Bus;
  UINT8  Dev;
  UINT8  Func;
  UINT16 Vid;
  UINT16 Did;
  UINT8  BaseClass;
  UINT8  SubClass;
  UINT8  ProgIf;
} PCI_DEV_INFO;

// Globals
STATIC EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *mRbIo = NULL;
STATIC BOOLEAN gDangerousUnlocked = FALSE;

// -----------------------------------------------------------------------------
// Helper: Optimized WaitKey (Replaces Busy Loop)
// -----------------------------------------------------------------------------
STATIC
VOID
WaitKey(OUT EFI_INPUT_KEY *Key)
{
  UINTN Index;
  // Use WaitForEvent to sleep until a key is pressed (Low CPU usage)
  gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &Index);
  gST->ConIn->ReadKeyStroke(gST->ConIn, Key);
}

STATIC
VOID
ClearScreen(VOID)
{
  gST->ConOut->ClearScreen(gST->ConOut);
}

// -----------------------------------------------------------------------------
// Helper: Key Checks
// -----------------------------------------------------------------------------
STATIC BOOLEAN IsEsc(IN EFI_INPUT_KEY *Key)   { return (Key->ScanCode == SCAN_ESC); }
STATIC BOOLEAN IsEnter(IN EFI_INPUT_KEY *Key) { return (Key->UnicodeChar == CHAR_CARRIAGE_RETURN); }
STATIC BOOLEAN IsTab(IN EFI_INPUT_KEY *Key)   { return (Key->UnicodeChar == CHAR_TAB); }

// -----------------------------------------------------------------------------
// Wrapper: PCI Root Bridge IO Access
// -----------------------------------------------------------------------------
STATIC
EFI_STATUS
InitRbIo(VOID)
{
  // Locate the first instance of PciRootBridgeIo
  return gBS->LocateProtocol(&gEfiPciRootBridgeIoProtocolGuid, NULL, (VOID**)&mRbIo);
}

// Calculate Address for Pci.Read/Write
// Format: Bus[31:24] Dev[23:16] Func[15:8] Reg[7:0]
STATIC
UINT64
PciCfgAddr(UINT8 Bus, UINT8 Dev, UINT8 Func, UINT16 Reg)
{
  return (UINT64)(Reg & 0xFF) |
         ((UINT64)Func << 8) |
         ((UINT64)Dev  << 16) |
         ((UINT64)Bus  << 24);
}

// Read Wrappers
STATIC EFI_STATUS PciRead8 (UINT8 B, UINT8 D, UINT8 F, UINT16 R, UINT8  *V){ return mRbIo->Pci.Read (mRbIo, EfiPciWidthUint8,  PciCfgAddr(B,D,F,R), 1, V); }
STATIC EFI_STATUS PciRead16(UINT8 B, UINT8 D, UINT8 F, UINT16 R, UINT16 *V){ return mRbIo->Pci.Read (mRbIo, EfiPciWidthUint16, PciCfgAddr(B,D,F,R), 1, V); }
STATIC EFI_STATUS PciRead32(UINT8 B, UINT8 D, UINT8 F, UINT16 R, UINT32 *V){ return mRbIo->Pci.Read (mRbIo, EfiPciWidthUint32, PciCfgAddr(B,D,F,R), 1, V); }

// Write Wrappers
STATIC EFI_STATUS PciWrite8 (UINT8 B, UINT8 D, UINT8 F, UINT16 R, UINT8  V){ return mRbIo->Pci.Write(mRbIo, EfiPciWidthUint8,  PciCfgAddr(B,D,F,R), 1, &V); }
STATIC EFI_STATUS PciWrite16(UINT8 B, UINT8 D, UINT8 F, UINT16 R, UINT16 V){ return mRbIo->Pci.Write(mRbIo, EfiPciWidthUint16, PciCfgAddr(B,D,F,R), 1, &V); }
STATIC EFI_STATUS PciWrite32(UINT8 B, UINT8 D, UINT8 F, UINT16 R, UINT32 V){ return mRbIo->Pci.Write(mRbIo, EfiPciWidthUint32, PciCfgAddr(B,D,F,R), 1, &V); }

// -----------------------------------------------------------------------------
// Logic: Safety Checks
// -----------------------------------------------------------------------------
STATIC BOOLEAN IsBarOffset(UINT16 Off) { return (Off >= 0x10 && Off <= 0x24); }
STATIC BOOLEAN IsCapArea(UINT16 Off)   { return (Off >= 0x34); }

// -----------------------------------------------------------------------------
// Logic: Scan all PCI Devices
// -----------------------------------------------------------------------------
STATIC
BOOLEAN
ReadPciFuncInfo(UINT8 Bus, UINT8 Dev, UINT8 Func, OUT PCI_DEV_INFO *Out)
{
  UINT16 Vid;
  if (EFI_ERROR(PciRead16(Bus, Dev, Func, 0x00, &Vid)) || Vid == 0xFFFF) {
    return FALSE;
  }

  UINT16 Did; PciRead16(Bus, Dev, Func, 0x02, &Did);
  UINT8  ProgIf, Sub, Base;
  PciRead8(Bus, Dev, Func, 0x09, &ProgIf);
  PciRead8(Bus, Dev, Func, 0x0A, &Sub);
  PciRead8(Bus, Dev, Func, 0x0B, &Base);

  Out->Bus = Bus; Out->Dev = Dev; Out->Func = Func;
  Out->Vid = Vid; Out->Did = Did;
  Out->ProgIf = ProgIf; Out->SubClass = Sub; Out->BaseClass = Base;
  return TRUE;
}

STATIC
UINTN
ScanAllPci(OUT PCI_DEV_INFO **OutList)
{
  PCI_DEV_INFO *List = AllocateZeroPool(sizeof(PCI_DEV_INFO) * MAX_PCI_DEVS);
  if (List == NULL) return 0;

  UINTN Count = 0;

  for (UINT16 Bus = 0; Bus <= 255; Bus++) {
    for (UINT16 Dev = 0; Dev <= 31; Dev++) {

      PCI_DEV_INFO Info0;
      if (!ReadPciFuncInfo((UINT8)Bus, (UINT8)Dev, 0, &Info0)) {
        continue;
      }

      if (Count < MAX_PCI_DEVS) List[Count++] = Info0;

      UINT8 HdrType = 0;
      PciRead8((UINT8)Bus, (UINT8)Dev, 0, 0x0E, &HdrType);
      if ((HdrType & 0x80) == 0) {
        continue; // Single function device
      }

      for (UINT16 Func = 1; Func <= 7; Func++) {
        PCI_DEV_INFO Info;
        if (ReadPciFuncInfo((UINT8)Bus, (UINT8)Dev, (UINT8)Func, &Info)) {
          if (Count < MAX_PCI_DEVS) List[Count++] = Info;
        }
      }
    }
  }

  *OutList = List;
  return Count;
}

// -----------------------------------------------------------------------------
// UI: List View
// -----------------------------------------------------------------------------
STATIC
VOID
RenderListScreen(PCI_DEV_INFO *List, UINTN Count, UINTN Sel, UINTN Page, UINTN PageSize)
{
  ClearScreen();
  Print(L"VendorID  DeviceID  Class      Bus/Dev/Func\n");
  Print(L"------------------------------------------\n");

  UINTN Start = Page * PageSize;
  UINTN End   = Start + PageSize;
  if (End > Count) End = Count;

  for (UINTN i = Start; i < End; i++) {
    PCI_DEV_INFO *p = &List[i];
    BOOLEAN isSel = (i == Sel);

    Print(L"%s%04x      %04x      %02x%02x%02x   %02x/%02x/%02x\n",
          (isSel) ? L"> " : L"  ",
          p->Vid, p->Did, p->BaseClass, p->SubClass, p->ProgIf,
          p->Bus, p->Dev, p->Func);
  }

  Print(L"\nUp/Down:Select  Enter:Open  Esc:Exit  F1:PgDn  F2:PgUp\n");
  Print(L"[Page:%u/%u]  Devices:%u\n",
        (UINT32)(Page + 1),
        (UINT32)((Count + PageSize - 1) / PageSize),
        (UINT32)Count);
}

// -----------------------------------------------------------------------------
// UI: Config View & Helpers
// -----------------------------------------------------------------------------
STATIC
VOID
ReadConfig256(UINT8 Bus, UINT8 Dev, UINT8 Func, UINT8 *Buf256)
{
  for (UINT16 off = 0; off < 0x100; off++) {
    UINT8 v = 0;
    PciRead8(Bus, Dev, Func, off, &v);
    Buf256[off] = v;
  }
}

STATIC
UINT16
AlignCursor(UINT16 Cursor, DISP_MODE Mode)
{
  if (Mode == DISP_WORD)  return (UINT16)(Cursor & ~1U);
  if (Mode == DISP_DWORD) return (UINT16)(Cursor & ~3U);
  return Cursor;
}

STATIC
UINT16
StepByMode(DISP_MODE Mode)
{
  if (Mode == DISP_WORD)  return 2;
  if (Mode == DISP_DWORD) return 4;
  return 1;
}

STATIC
VOID
RenderConfigScreen(UINT8 Bus, UINT8 Dev, UINT8 Func, UINT8 *Buf, DISP_MODE Mode, UINT16 Cursor)
{
  ClearScreen();

  Print(L"PCI Config Space (0x00-0xFF)   Bus:%02x Dev:%02x Func:%02x\n", Bus, Dev, Func);
  Print(L"Mode:%s  Tab:Switch  Arrows:Move  Enter:Write  Esc:Back\n",
        (Mode == DISP_BYTE) ? L"BYTE" : (Mode == DISP_WORD) ? L"WORD" : L"DWORD");
  Print(L"Protection: %s (F9 Toggle)\n", gDangerousUnlocked ? L"OFF" : L"ON (BAR/Cap Safe)");
  Print(L"------------------------------------------------------------\n");

  Cursor = AlignCursor(Cursor, Mode);

  for (UINT16 row = 0; row < 0x100; row += 0x10) {
    Print(L"%02x  ", row);

    // Render Data Columns
    if (Mode == DISP_BYTE) {
      for (UINT16 i = 0; i < 0x10; i++) {
        UINT16 off = (UINT16)(row + i);
        if (off == Cursor) Print(L"[");
        Print(L"%02x", Buf[off]);
        if (off == Cursor) Print(L"]");
        Print(L" ");
      }
    } else if (Mode == DISP_WORD) {
      for (UINT16 i = 0; i < 0x10; i += 2) {
        UINT16 off = (UINT16)(row + i);
        UINT16 v = *(UINT16*)&Buf[off];
        if (off == Cursor) Print(L"[");
        Print(L"%04x", v);
        if (off == Cursor) Print(L"]");
        Print(L" ");
      }
    } else {
      for (UINT16 i = 0; i < 0x10; i += 4) {
        UINT16 off = (UINT16)(row + i);
        UINT32 v = *(UINT32*)&Buf[off];
        if (off == Cursor) Print(L"[");
        Print(L"%08x", v);
        if (off == Cursor) Print(L"]");
        Print(L" ");
      }
    }
    Print(L"\n");
  }
  Print(L"\nCursor Offset: 0x%02x\n", Cursor);
}

// -----------------------------------------------------------------------------
// Input: Hex Parsing
// -----------------------------------------------------------------------------
STATIC BOOLEAN IsHexChar(CHAR16 c) {
  return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

STATIC UINTN HexVal(CHAR16 c) {
  if (c >= L'0' && c <= L'9') return (UINTN)(c - L'0');
  if (c >= L'a' && c <= L'f') return 10U + (UINTN)(c - L'a');
  return 10U + (UINTN)(c - L'A');
}

STATIC
EFI_STATUS
ReadFixedHex(UINTN Digits, OUT UINT64 *OutVal)
{
  *OutVal = 0;
  UINTN got = 0;

  while (got < Digits) {
    EFI_INPUT_KEY Key;
    WaitKey(&Key);

    if (IsEsc(&Key)) return EFI_ABORTED;

    if (IsHexChar(Key.UnicodeChar)) {
      *OutVal = ((*OutVal) << 4) | HexVal(Key.UnicodeChar);
      got++;
      Print(L"%c", Key.UnicodeChar);
    }
  }
  return EFI_SUCCESS;
}

// -----------------------------------------------------------------------------
// Logic: Write with Verification (FIXED)
// -----------------------------------------------------------------------------
STATIC
EFI_STATUS
DoWriteAtCursor(UINT8 Bus, UINT8 Dev, UINT8 Func, DISP_MODE Mode, UINT16 Cursor)
{
  EFI_STATUS Status;
  Cursor = AlignCursor(Cursor, Mode);

  // Safety Check: Only block critical areas if locked. 
  // Command(0x04) and Status(0x06) are ALLOWED now.
  if ((IsBarOffset(Cursor) || IsCapArea(Cursor)) && !gDangerousUnlocked) {
    ClearScreen();
    Print(L"WRITE BLOCKED (Dangerous Area)\n\n");
    Print(L"You are trying to write to BARs or Capability area.\n");
    Print(L"Press F9 in the main screen to unlock if you really mean it.\n");
    Print(L"\nPress any key...");
    EFI_INPUT_KEY K; WaitKey(&K);
    return EFI_ACCESS_DENIED;
  }

  ClearScreen();
  Print(L"WRITE PCI CONFIG  Bus:%02x Dev:%02x Func:%02x  Offset:0x%02x\n", Bus, Dev, Func, Cursor);
  
  // Informational warnings only
  if (Cursor == 0x04) Print(L"Target: Command Reg (Be careful!)\n");
  if (Cursor == 0x06) Print(L"Target: Status Reg (Write-1-to-Clear)\n");

  Print(L"Input HEX (%u digits).  Esc:Cancel\n\n", (Mode==DISP_BYTE)?2U:(Mode==DISP_WORD)?4U:8U);
  Print(L"Value: ");

  UINT64 Val = 0;
  Status = ReadFixedHex((Mode==DISP_BYTE)?2U:(Mode==DISP_WORD)?4U:8U, &Val);
  if (EFI_ERROR(Status)) return Status;

  Print(L"\n\nWriting 0x%X...\n", Val);

  // --- RAW WRITE EXECUTION ---
  // Removed artificial masks/RMW logic. What you type is what you write.
  if (Mode == DISP_BYTE) {
    Status = PciWrite8(Bus, Dev, Func, Cursor, (UINT8)Val);
  } else if (Mode == DISP_WORD) {
    Status = PciWrite16(Bus, Dev, Func, Cursor, (UINT16)Val);
  } else {
    Status = PciWrite32(Bus, Dev, Func, Cursor, (UINT32)Val);
  }

  if (EFI_ERROR(Status)) {
    Print(L"Write Protocol Error: %r\n", Status);
  } else {
    // Read-back Verify
    UINT64 ReadBack = 0;
    if (Mode == DISP_BYTE) {
      UINT8 v; PciRead8(Bus, Dev, Func, Cursor, &v); ReadBack = v;
    } else if (Mode == DISP_WORD) {
      UINT16 v; PciRead16(Bus, Dev, Func, Cursor, &v); ReadBack = v;
    } else {
      UINT32 v; PciRead32(Bus, Dev, Func, Cursor, &v); ReadBack = v;
    }
    
    Print(L"Write Success.\n");
    Print(L"Input:    0x%X\n", Val);
    Print(L"ReadBack: 0x%X\n", ReadBack);
    
    if (ReadBack != Val) {
      Print(L"Error: Mismatch is  (W1C), RO bits, or Reserved bits.\n");
    }
  }

  Print(L"\nPress any key to return...\n");
  EFI_INPUT_KEY K; WaitKey(&K);
  return Status;
}

// -----------------------------------------------------------------------------
// Logic: Config View Loop
// -----------------------------------------------------------------------------
STATIC
VOID
ConfigViewLoop(UINT8 Bus, UINT8 Dev, UINT8 Func)
{
  UINT8 Buf[0x100];
  DISP_MODE Mode = DISP_DWORD;
  UINT16 Cursor = 0;

  // Initial Read
  ReadConfig256(Bus, Dev, Func, Buf);

  while (TRUE) {
    RenderConfigScreen(Bus, Dev, Func, Buf, Mode, Cursor);

    EFI_INPUT_KEY Key;
    WaitKey(&Key);

    if (IsEsc(&Key)) return;

    if (Key.ScanCode == SCAN_F9) {
      gDangerousUnlocked = !gDangerousUnlocked;
      continue;
    }

    if (IsTab(&Key)) {
      Mode = (DISP_MODE)((Mode + 1) % 3);
      Cursor = AlignCursor(Cursor, Mode);
      continue;
    }

    if (IsEnter(&Key)) {
      DoWriteAtCursor(Bus, Dev, Func, Mode, Cursor);
      // Re-read after write to show updates
      ReadConfig256(Bus, Dev, Func, Buf);
      continue;
    }

    // Navigation
    UINT16 Step = StepByMode(Mode);
    switch (Key.ScanCode) {
      case SCAN_UP:
        if (Cursor >= 0x10) Cursor = (UINT16)(Cursor - 0x10);
        break;
      case SCAN_DOWN:
        if (Cursor + 0x10 < 0x100) Cursor = (UINT16)(Cursor + 0x10);
        break;
      case SCAN_LEFT:
        if (Cursor >= Step) Cursor = (UINT16)(Cursor - Step);
        break;
      case SCAN_RIGHT:
        if (Cursor + Step < 0x100) Cursor = (UINT16)(Cursor + Step);
        break;
      default:
        break;
    }
    Cursor = AlignCursor(Cursor, Mode);
  }
}

// -----------------------------------------------------------------------------
// Main Entry Point
// -----------------------------------------------------------------------------
EFI_STATUS
EFIAPI
UefiMain(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status = InitRbIo();
  if (EFI_ERROR(Status) || mRbIo == NULL) {
    Print(L"Error: LocateProtocol(PciRootBridgeIo) failed: %r\n", Status);
    return Status;
  }

  Print(L"Scanning PCI Devices...\n");
  PCI_DEV_INFO *List = NULL;
  UINTN Count = ScanAllPci(&List);

  if (Count == 0 || List == NULL) {
    Print(L"No PCI devices found.\n");
    return EFI_NOT_FOUND;
  }

  UINTN Sel = 0;
  UINTN PageSize = 18;
  UINTN Page = 0;

  while (TRUE) {
    // Pagination Logic
    UINTN MaxPage = (Count + PageSize - 1) / PageSize;
    if (Page >= MaxPage) Page = (MaxPage == 0) ? 0 : (MaxPage - 1);

    UINTN SelPage = Sel / PageSize;
    if (SelPage != Page) Page = SelPage;

    RenderListScreen(List, Count, Sel, Page, PageSize);

    EFI_INPUT_KEY Key;
    WaitKey(&Key);

    if (IsEsc(&Key)) break;

    if (IsEnter(&Key)) {
      PCI_DEV_INFO *p = &List[Sel];
      ConfigViewLoop(p->Bus, p->Dev, p->Func);
      continue;
    }

    if (Key.ScanCode == SCAN_F1) { // PageDown
      if (Page + 1 < MaxPage) {
        Page++;
        Sel = Page * PageSize;
        if (Sel >= Count) Sel = Count - 1;
      }
      continue;
    }
    if (Key.ScanCode == SCAN_F2) { // PageUp
      if (Page > 0) {
        Page--;
        Sel = Page * PageSize;
      }
      continue;
    }

    switch (Key.ScanCode) {
      case SCAN_UP:
        if (Sel > 0) Sel--;
        break;
      case SCAN_DOWN:
        if (Sel + 1 < Count) Sel++;
        break;
      default:
        break;
    }
  }

  if (List) FreePool(List);
  ClearScreen();
  return EFI_SUCCESS;
}