#include <Uefi.h>

#include <Protocol/PciRootBridgeIo.h>

#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>

#define MAX_PCI_DEVS  4096

typedef enum {
  DISP_BYTE  = 0,
  DISP_WORD  = 1,
  DISP_DWORD = 2
} DISP_MODE;

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

STATIC EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *mRbIo = NULL;
STATIC BOOLEAN gDangerousUnlocked = FALSE;

// -----------------------------
// Helpers: Console / Keys
// -----------------------------
STATIC
VOID
WaitKey(OUT EFI_INPUT_KEY *Key)
{
  while (gST->ConIn->ReadKeyStroke(gST->ConIn, Key) == EFI_NOT_READY) {}
}

STATIC
VOID
ClearScreen(VOID)
{
  gST->ConOut->ClearScreen(gST->ConOut);
}

STATIC
BOOLEAN
IsEsc(IN EFI_INPUT_KEY *Key)
{
  return (Key->UnicodeChar == 0 && Key->ScanCode == SCAN_ESC);
}

STATIC
BOOLEAN
IsEnter(IN EFI_INPUT_KEY *Key)
{
  return (Key->UnicodeChar == CHAR_CARRIAGE_RETURN);
}

STATIC
BOOLEAN
IsTab(IN EFI_INPUT_KEY *Key)
{
  return (Key->UnicodeChar == CHAR_TAB);
}

// -----------------------------
// Helpers: PCI RBIO access
// -----------------------------
STATIC
EFI_STATUS
InitRbIo(VOID)
{
  return gBS->LocateProtocol(&gEfiPciRootBridgeIoProtocolGuid, NULL, (VOID**)&mRbIo);
}

// Address[7:0]=Reg, [15:8]=Func, [23:16]=Dev, [31:24]=Bus  (0x00~0xFF)
STATIC
UINT64
PciCfgAddr(UINT8 Bus, UINT8 Dev, UINT8 Func, UINT16 Reg)
{
  return (UINT64)(Reg & 0xFF) |
         ((UINT64)Func << 8) |
         ((UINT64)Dev  << 16) |
         ((UINT64)Bus  << 24);
}

STATIC EFI_STATUS PciRead8 (UINT8 B, UINT8 D, UINT8 F, UINT16 R, UINT8  *V){ return mRbIo->Pci.Read (mRbIo, EfiPciWidthUint8,  PciCfgAddr(B,D,F,R), 1, V); }
STATIC EFI_STATUS PciRead16(UINT8 B, UINT8 D, UINT8 F, UINT16 R, UINT16 *V){ return mRbIo->Pci.Read (mRbIo, EfiPciWidthUint16, PciCfgAddr(B,D,F,R), 1, V); }
STATIC EFI_STATUS PciRead32(UINT8 B, UINT8 D, UINT8 F, UINT16 R, UINT32 *V){ return mRbIo->Pci.Read (mRbIo, EfiPciWidthUint32, PciCfgAddr(B,D,F,R), 1, V); }

STATIC EFI_STATUS PciWrite8 (UINT8 B, UINT8 D, UINT8 F, UINT16 R, UINT8  V){ return mRbIo->Pci.Write(mRbIo, EfiPciWidthUint8,  PciCfgAddr(B,D,F,R), 1, &V); }
STATIC EFI_STATUS PciWrite16(UINT8 B, UINT8 D, UINT8 F, UINT16 R, UINT16 V){ return mRbIo->Pci.Write(mRbIo, EfiPciWidthUint16, PciCfgAddr(B,D,F,R), 1, &V); }
STATIC EFI_STATUS PciWrite32(UINT8 B, UINT8 D, UINT8 F, UINT16 R, UINT32 V){ return mRbIo->Pci.Write(mRbIo, EfiPciWidthUint32, PciCfgAddr(B,D,F,R), 1, &V); }

// -----------------------------
// Cursor helpers
// -----------------------------
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

// -----------------------------
// Safety / Policy
// -----------------------------
typedef enum {
  WP_BLOCK_RO,
  WP_RW_DIRECT,
  WP_RW1C,
  WP_DANGEROUS_BAR,
  WP_DANGEROUS_CAP
} WRITE_POLICY;

STATIC
WRITE_POLICY
GetWritePolicy(UINT8 Bus, UINT8 Dev, UINT8 Func, UINT16 Off, DISP_MODE Mode)
{
  (VOID)Bus; (VOID)Dev; (VOID)Func;
  Off = AlignCursor(Off, Mode);

  // RO standard fields
  if (Off < 0x04) return WP_BLOCK_RO; // VID/DID
  if (Off == 0x08) return WP_BLOCK_RO; // Revision
  if (Off == 0x09 || Off == 0x0A || Off == 0x0B) return WP_BLOCK_RO; // Class/ProgIF
  if (Off == 0x0E) return WP_BLOCK_RO; // HeaderType

  // RW1C (Status, WORD)
  if (Off == 0x06 && Mode == DISP_WORD) return WP_RW1C;

  // BAR/resource
  if (Off >= 0x10 && Off <= 0x24) return WP_DANGEROUS_BAR;

  // Capabilities / extended area
  if (Off >= 0x34) return WP_DANGEROUS_CAP;

  return WP_RW_DIRECT;
}

STATIC
BOOLEAN
IsProbeSafe(UINT16 Off, DISP_MODE Mode)
{
  (VOID)Mode;
  // 最保守：只允許 0x40~0xFF 做 probe，避免副作用
  return (Off >= 0x40 && Off < 0x100);
}

// -----------------------------
// Probe
// -----------------------------
STATIC
EFI_STATUS
ProbeWritableMaskAtCursor(
  UINT8 Bus, UINT8 Dev, UINT8 Func,
  DISP_MODE Mode, UINT16 Cursor,
  OUT UINT64 *OutOld,
  OUT UINT64 *OutTest,
  OUT UINT64 *OutReadBack,
  OUT UINT64 *OutMask
  )
{
  Cursor = AlignCursor(Cursor, Mode);

  *OutOld = *OutTest = *OutReadBack = *OutMask = 0;

  if (!IsProbeSafe(Cursor, Mode)) {
    return EFI_ACCESS_DENIED;
  }

  EFI_STATUS Status;

  if (Mode == DISP_BYTE) {
    UINT8 Old=0, Rb=0, Test=0;
    Status = PciRead8(Bus,Dev,Func,Cursor,&Old);
    if (EFI_ERROR(Status)) return Status;

    Test = (UINT8)~Old;

    Status = PciWrite8(Bus,Dev,Func,Cursor,Test);
    if (EFI_ERROR(Status)) return Status;

    Status = PciRead8(Bus,Dev,Func,Cursor,&Rb);
    PciWrite8(Bus,Dev,Func,Cursor,Old); // restore anyway
    if (EFI_ERROR(Status)) return Status;

    *OutOld = Old;
    *OutTest = Test;
    *OutReadBack = Rb;
    *OutMask = (UINT8)(Old ^ Rb);
    return EFI_SUCCESS;

  } else if (Mode == DISP_WORD) {
    UINT16 Old=0, Rb=0, Test=0;
    Status = PciRead16(Bus,Dev,Func,Cursor,&Old);
    if (EFI_ERROR(Status)) return Status;

    Test = (UINT16)~Old;

    Status = PciWrite16(Bus,Dev,Func,Cursor,Test);
    if (EFI_ERROR(Status)) return Status;

    Status = PciRead16(Bus,Dev,Func,Cursor,&Rb);
    PciWrite16(Bus,Dev,Func,Cursor,Old); // restore anyway
    if (EFI_ERROR(Status)) return Status;

    *OutOld = Old;
    *OutTest = Test;
    *OutReadBack = Rb;
    *OutMask = (UINT16)(Old ^ Rb);
    return EFI_SUCCESS;

  } else {
    UINT32 Old=0, Rb=0, Test=0;
    Status = PciRead32(Bus,Dev,Func,Cursor,&Old);
    if (EFI_ERROR(Status)) return Status;

    Test = ~Old;

    Status = PciWrite32(Bus,Dev,Func,Cursor,Test);
    if (EFI_ERROR(Status)) return Status;

    Status = PciRead32(Bus,Dev,Func,Cursor,&Rb);
    PciWrite32(Bus,Dev,Func,Cursor,Old); // restore anyway
    if (EFI_ERROR(Status)) return Status;

    *OutOld = Old;
    *OutTest = Test;
    *OutReadBack = Rb;
    *OutMask = (UINT32)(Old ^ Rb);
    return EFI_SUCCESS;
  }
}

// -----------------------------
// PCI scan
// -----------------------------
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
        continue;
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

// -----------------------------
// UI: List screen
// -----------------------------
STATIC
VOID
RenderListScreen(PCI_DEV_INFO *List, UINTN Count, UINTN Sel, UINTN Page, UINTN PageSize)
{
  ClearScreen();

  Print(L"VendorID  DeviceID  Class     Bus/Dev/Func\n");
  Print(L"------------------------------------------\n");

  UINTN Start = Page * PageSize;
  UINTN End   = Start + PageSize;
  if (End > Count) End = Count;

  for (UINTN i = Start; i < End; i++) {
    PCI_DEV_INFO *p = &List[i];
    BOOLEAN isSel = (i == Sel);

    if (isSel) Print(L"> ");
    else       Print(L"  ");

    Print(L"%04x      %04x      %02x%02x%02x   %02x/%02x/%02x\n",
          p->Vid, p->Did, p->BaseClass, p->SubClass, p->ProgIf,
          p->Bus, p->Dev, p->Func);
  }

  Print(L"\nUp/Down:Select  Enter:Open  Esc:Exit  F1:PgDn  F2:PgUp\n");
  Print(L"[Page:%u/%u]  Devices:%u\n",
        (UINT32)(Page + 1),
        (UINT32)((Count + PageSize - 1) / PageSize),
        (UINT32)Count);
}

// -----------------------------
// UI: Config view / edit
// -----------------------------
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
VOID
RenderConfigScreen(UINT8 Bus, UINT8 Dev, UINT8 Func, UINT8 *Buf, DISP_MODE Mode, UINT16 Cursor)
{
  ClearScreen();

  Print(L"PCI Config Space (0x00-0xFF)   Bus:%02x Dev:%02x Func:%02x\n", Bus, Dev, Func);
  Print(L"Mode:%s  Tab:Switch  Arrows:Move  Enter:Write  P:Probe  F9:Unlock  Esc:Back\n",
        (Mode == DISP_BYTE) ? L"BYTE" : (Mode == DISP_WORD) ? L"WORD" : L"DWORD");
  Print(L"Dangerous Writes: %s\n", gDangerousUnlocked ? L"UNLOCKED" : L"LOCKED");
  Print(L"------------------------------------------------------------\n");

  Cursor = AlignCursor(Cursor, Mode);

  for (UINT16 row = 0; row < 0x100; row += 0x10) {
    Print(L"%02x  ", row);

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

// -----------------------------
// Hex input
// -----------------------------
STATIC
BOOLEAN
IsHexChar(CHAR16 c)
{
  return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

STATIC
UINTN
HexVal(CHAR16 c)
{
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

// -----------------------------
// Write at cursor (safe)
// -----------------------------
STATIC
EFI_STATUS
DoWriteAtCursor(UINT8 Bus, UINT8 Dev, UINT8 Func, DISP_MODE Mode, UINT16 Cursor)
{
  Cursor = AlignCursor(Cursor, Mode);
  WRITE_POLICY Pol = GetWritePolicy(Bus, Dev, Func, Cursor, Mode);

  if (Pol == WP_BLOCK_RO) {
    ClearScreen();
    Print(L"WRITE BLOCKED (RO)\nBus:%02x Dev:%02x Func:%02x Offset:0x%02x\n\n", Bus, Dev, Func, Cursor);
    Print(L"Press any key...\n");
    EFI_INPUT_KEY K; WaitKey(&K);
    return EFI_ACCESS_DENIED;
  }

  if ((Pol == WP_DANGEROUS_BAR || Pol == WP_DANGEROUS_CAP) && !gDangerousUnlocked) {
    ClearScreen();
    Print(L"WRITE BLOCKED (Dangerous)\nBus:%02x Dev:%02x Func:%02x Offset:0x%02x\n\n", Bus, Dev, Func, Cursor);
    Print(L"BAR(0x10-0x24) / CAP(>=0x34) blocked. Press F9 to unlock.\n");
    Print(L"Press any key...\n");
    EFI_INPUT_KEY K; WaitKey(&K);
    return EFI_ACCESS_DENIED;
  }

  ClearScreen();
  Print(L"WRITE PCI CONFIG  Bus:%02x Dev:%02x Func:%02x  Offset:0x%02x\n", Bus, Dev, Func, Cursor);
  Print(L"Input HEX (%u digits).  Esc:Cancel\n\n", (Mode==DISP_BYTE)?2U:(Mode==DISP_WORD)?4U:8U);
  if (Pol == WP_RW1C) {
    Print(L"(RW1C) Input is ClearMask (write-1-to-clear)\n\n");
  }
  Print(L"Value: ");

  UINT64 Val = 0;
  EFI_STATUS Status = ReadFixedHex((Mode==DISP_BYTE)?2U:(Mode==DISP_WORD)?4U:8U, &Val);
  if (EFI_ERROR(Status)) return Status;

  Print(L"\n\nWriting...\n");

  // Special handling
  if (Mode == DISP_WORD && Cursor == 0x04) {
    // Command RMW: keep reserved bits, allow safe bits
    UINT16 Old = 0;
    UINT16 New = (UINT16)Val;
    PciRead16(Bus, Dev, Func, 0x04, &Old);

    UINT16 Mask  = (UINT16)((1U<<0) | (1U<<1) | (1U<<2) | (1U<<10)); // IO/MEM/BM/INTxDisable
    UINT16 Final = (UINT16)((Old & ~Mask) | (New & Mask));

    Status = PciWrite16(Bus, Dev, Func, 0x04, Final);
    Print(L"Command Old:0x%04x  Input:0x%04x  Final(RMW):0x%04x\n", Old, New, Final);

    if (!EFI_ERROR(Status)) {
      UINT16 Rb = 0;
      PciRead16(Bus, Dev, Func, 0x04, &Rb);
      if (Rb != Final) Print(L"NOTE: Read-back mismatch. Read=0x%04x (masked/RO?)\n", Rb);
    }

  } else if (Pol == WP_RW1C && Mode == DISP_WORD && Cursor == 0x06) {
    // Status RW1C: input is clear mask
    UINT16 Before = 0;
    UINT16 ClearMask = (UINT16)Val;
    PciRead16(Bus, Dev, Func, 0x06, &Before);

    Status = PciWrite16(Bus, Dev, Func, 0x06, ClearMask);
    Print(L"Status Before:0x%04x  ClearMask:0x%04x\n", Before, ClearMask);

    if (!EFI_ERROR(Status)) {
      UINT16 After = 0;
      PciRead16(Bus, Dev, Func, 0x06, &After);
      Print(L"Status After :0x%04x\n", After);
    }

  } else {
    // Direct write + read-back verify
    if (Mode == DISP_BYTE) {
      Status = PciWrite8(Bus, Dev, Func, Cursor, (UINT8)Val);
      if (!EFI_ERROR(Status)) {
        UINT8 rb = 0; PciRead8(Bus, Dev, Func, Cursor, &rb);
        if (rb != (UINT8)Val) Print(L"NOTE: Read-back mismatch. Read=0x%02x (masked/RO/ignored)\n", rb);
      }
    } else if (Mode == DISP_WORD) {
      Status = PciWrite16(Bus, Dev, Func, Cursor, (UINT16)Val);
      if (!EFI_ERROR(Status)) {
        UINT16 rb = 0; PciRead16(Bus, Dev, Func, Cursor, &rb);
        if (rb != (UINT16)Val) Print(L"NOTE: Read-back mismatch. Read=0x%04x (masked/RO/ignored)\n", rb);
      }
    } else {
      Status = PciWrite32(Bus, Dev, Func, Cursor, (UINT32)Val);
      if (!EFI_ERROR(Status)) {
        UINT32 rb = 0; PciRead32(Bus, Dev, Func, Cursor, &rb);
        if (rb != (UINT32)Val) Print(L"NOTE: Read-back mismatch. Read=0x%08x (masked/RO/ignored)\n", rb);
      }
    }
  }

  Print(L"\nWrite Status: %r\n", Status);
  Print(L"Press any key...\n");
  EFI_INPUT_KEY K; WaitKey(&K);
  return Status;
}

// -----------------------------
// Config view loop
// -----------------------------
STATIC
VOID
ConfigViewLoop(UINT8 Bus, UINT8 Dev, UINT8 Func)
{
  UINT8 Buf[0x100];
  DISP_MODE Mode = DISP_DWORD;
  UINT16 Cursor = 0;

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

    // Probe hotkey
    if (Key.UnicodeChar == L'p' || Key.UnicodeChar == L'P') {
      UINT16 Cur = AlignCursor(Cursor, Mode);
      WRITE_POLICY Pol = GetWritePolicy(Bus, Dev, Func, Cur, Mode);

      ClearScreen();
      Print(L"PROBE WRITABLE MASK\n");
      Print(L"Bus:%02x Dev:%02x Func:%02x  Offset:0x%02x  Mode:%s\n",
            Bus, Dev, Func, Cur,
            (Mode==DISP_BYTE)?L"BYTE":(Mode==DISP_WORD)?L"WORD":L"DWORD");

      Print(L"Policy: ");
      if (Pol == WP_BLOCK_RO) Print(L"RO (blocked)\n");
      else if (Pol == WP_RW1C) Print(L"RW1C (Status-like)\n");
      else if (Pol == WP_DANGEROUS_BAR) Print(L"DANGEROUS BAR\n");
      else if (Pol == WP_DANGEROUS_CAP) Print(L"DANGEROUS CAP\n");
      else Print(L"RW (verify)\n");

      if (!IsProbeSafe(Cur, Mode)) {
        Print(L"\nProbe blocked: only allow 0x40~0xFF to avoid side effects.\n");
        Print(L"Press any key...\n");
        EFI_INPUT_KEY K; WaitKey(&K);
        continue;
      }

      UINT64 Old, Test, Rb, Mask;
      EFI_STATUS St = ProbeWritableMaskAtCursor(Bus, Dev, Func, Mode, Cur, &Old, &Test, &Rb, &Mask);

      Print(L"\nProbe Status: %r\n", St);
      if (!EFI_ERROR(St)) {
        if (Mode == DISP_BYTE) {
          Print(L"Old     : 0x%02x\n", (UINT8)Old);
          Print(L"Test(~) : 0x%02x\n", (UINT8)Test);
          Print(L"ReadBack: 0x%02x\n", (UINT8)Rb);
          Print(L"Mask    : 0x%02x\n", (UINT8)Mask);
        } else if (Mode == DISP_WORD) {
          Print(L"Old     : 0x%04x\n", (UINT16)Old);
          Print(L"Test(~) : 0x%04x\n", (UINT16)Test);
          Print(L"ReadBack: 0x%04x\n", (UINT16)Rb);
          Print(L"Mask    : 0x%04x\n", (UINT16)Mask);
        } else {
          Print(L"Old     : 0x%08x\n", (UINT32)Old);
          Print(L"Test(~) : 0x%08x\n", (UINT32)Test);
          Print(L"ReadBack: 0x%08x\n", (UINT32)Rb);
          Print(L"Mask    : 0x%08x\n", (UINT32)Mask);
        }

        Print(L"\nInterpretation:\n");
        if (Mask == 0) {
          Print(L"- Likely RO / write ignored.\n");
        } else {
          BOOLEAN FullRw = FALSE;
          if (Mode == DISP_BYTE)  FullRw = ((UINT8)Rb  == (UINT8)Test);
          if (Mode == DISP_WORD)  FullRw = ((UINT16)Rb == (UINT16)Test);
          if (Mode == DISP_DWORD) FullRw = ((UINT32)Rb == (UINT32)Test);

          if (FullRw) Print(L"- RW: Most bits writable.\n");
          else        Print(L"- Masked RW: Only Mask bits respond.\n");
        }
      }

      Print(L"\nPress any key...\n");
      EFI_INPUT_KEY K; WaitKey(&K);
      continue;
    }

    if (IsTab(&Key)) {
      Mode = (DISP_MODE)((Mode + 1) % 3);
      Cursor = AlignCursor(Cursor, Mode);
      continue;
    }

    if (IsEnter(&Key)) {
      DoWriteAtCursor(Bus, Dev, Func, Mode, Cursor);
      ReadConfig256(Bus, Dev, Func, Buf);
      continue;
    }

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

// -----------------------------
// Main
// -----------------------------
EFI_STATUS
EFIAPI
UefiMain(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  (VOID)ImageHandle; (VOID)SystemTable;

  EFI_STATUS Status = InitRbIo();
  if (EFI_ERROR(Status) || mRbIo == NULL) {
    Print(L"LocateProtocol(PciRootBridgeIo) failed: %r\n", Status);
    return Status;
  }

  PCI_DEV_INFO *List = NULL;
  UINTN Count = ScanAllPci(&List);
  if (Count == 0 || List == NULL) {
    Print(L"No PCI devices found (or alloc failed).\n");
    return EFI_NOT_FOUND;
  }

  UINTN Sel = 0;
  UINTN PageSize = 18;
  UINTN Page = 0;

  while (TRUE) {
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
