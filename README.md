# PciUtilityPkg
以下是「PciUtility」這份工具的 **README 筆記版**（以你現在的程式架構為基準：RBIO 掃描 → 清單 UI → Config View → Write/Probe/Unlock）。

---

# PciUtility README（筆記版）

## 1) 目的與功能

PciUtility 是一個 UEFI Shell Application，用來：

* **列出系統上已 enumerate 的 PCI 裝置**（Bus/Dev/Func）
* **顯示 PCI Config Space（0x00~0xFF）**
* 支援 **BYTE / WORD / DWORD** 顯示切換
* 支援 **寫入 PCI config**（含安全策略：RO/RW1C/危險區擋寫、可解鎖）
* 支援 **Probe 可寫 mask**（偵測 masked-RW：只有部分 bit 寫得進去）

---

## 2) 核心協定：EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL

### 2.1 LocateProtocol 取得 RBIO

**用途**：取得 PCI config 存取能力（讀寫 config space）

**API**

```c
EFI_STATUS
gBS->LocateProtocol(
  IN  EFI_GUID  *Protocol,
  IN  VOID      *Registration OPTIONAL,
  OUT VOID      **Interface
);
```

**你程式怎麼用**

* `Protocol = &gEfiPciRootBridgeIoProtocolGuid`
* `Registration = NULL`
* `Interface = (VOID**)&mRbIo`

**成功條件**

* 回傳 `EFI_SUCCESS`
* 且 `mRbIo != NULL`

**常見錯誤**

* `EFI_NOT_FOUND`：此平台/階段沒有 RBIO（或 Shell 環境不支援）
* `EFI_INVALID_PARAMETER`：Interface 指標不合法

---

## 3) PCI Config Address Encoding（你封裝的 PciCfgAddr）

你封裝成 `PciCfgAddr(B,D,F,R)`，目的：組合成 RBIO 的 `Address`。

你 README 可寫成：

* `Address[7:0] = Reg (offset)`
* `Address[15:8] = Function`
* `Address[23:16] = Device`
* `Address[31:24] = Bus`

> 這是 UEFI 內常用的 config space address encoding（給 RootBridgeIo 解碼做交易）。

---

## 4) 讀寫封裝：PciRead / PciWrite

### 4.1 mRbIo->Pci.Read

**用途**：讀 PCI config

**Prototype（概念）**

```c
EFI_STATUS
(EFIAPI *EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_IO_MEM)(
  IN  EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL       *This,
  IN  EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH Width,
  IN  UINT64                                Address,
  IN  UINTN                                 Count,
  IN OUT VOID                               *Buffer
);
```

**你常用 width**

* `EfiPciWidthUint8`
* `EfiPciWidthUint16`
* `EfiPciWidthUint32`

**你封裝**

* `PciRead8(B,D,F,Off,&v)`
* `PciRead16(...)`
* `PciRead32(...)`

**注意**

* `Count=1` 表示只做一次 transfer
* `Width` 決定一次 transfer 幾 bytes
* Offset 要對齊：WORD 要 2-byte aligned，DWORD 要 4-byte aligned（你用 AlignCursor 做）

---

### 4.2 mRbIo->Pci.Write

**用途**：寫 PCI config

**你封裝**

* `PciWrite8(B,D,F,Off,val)`
* `PciWrite16(...)`
* `PciWrite32(...)`

**成功不代表值一定變**

* 很多欄位是 **RO / masked RW / vendor lock / side-effect protected**
* 所以你程式做了 **write + read-back verify**（很重要）

---

## 5) PCI 掃描 ScanAllPci：如何 enumerate

### 5.1 掃描範圍

* Bus: 0~255
* Dev: 0~31
* Func: 0~7（只有 multi-function 才掃 1~7）

### 5.2 判斷 function 是否存在

* 讀 `VendorID (0x00, WORD)`
* 若 `Vid == 0xFFFF` → 不存在（PCI spec 常見）

### 5.3 判斷 multi-function

* 讀 `HeaderType (0x0E, BYTE)`
* bit7 = 1 → multi-function，掃 func1~7
* bit7 = 0 → single function，只保留 func0

---

## 6) UI 操作手冊（使用者視角）

### 6.1 Device List 畫面

顯示欄位：

* `VID (VendorName)`
* `DID`
* `Class (ClassName)`
* `Bus/Dev/Func`

按鍵：

* `↑/↓`：選擇裝置
* `Enter`：進入 Config View
* `Esc`：退出工具
* `F1`：Page Down
* `F2`：Page Up

---

### 6.2 Config View 畫面（0x00~0xFF）

顯示模式：

* `Tab`：在 BYTE/WORD/DWORD 切換
* 游標框 `[ ]`：當前 offset（會依模式對齊）

按鍵：

* `↑/↓/←/→`：移動游標（步進依 mode：1/2/4 bytes）
* `Enter`：寫入（DoWriteAtCursor）
* `P`：Probe 可寫 mask（只允許 0x40~0xFF）
* `F9`：Unlock（允許寫 BAR/CAP 危險區）
* `Esc`：回到 Device List

---

## 7) Write（DoWriteAtCursor）規則與安全策略

### 7.1 你程式內建的分類（概念）

1. **RO（禁止寫）**

   * 例如：VendorID/DeviceID、ClassCode、HeaderType
2. **RW1C（write-1-to-clear）**

   * 常見：Status register（0x06 WORD）
3. **masked RW**

   * 寫入只有部分 bit 會生效（read-back 會不等於 input）
4. **BAR/resource / CAP**（預設擋寫）

   * 0x10~0x24：BAR
   * 0x34~：capability list（實務上很容易有副作用）
   * 需要 `F9` 解鎖才讓寫

### 7.2 寫入流程（建議你 README 用流程圖式寫法）

1. `Cursor = AlignCursor(Cursor, Mode)`
2. `Policy = GetWritePolicy(Offset, Mode)`
3. 若 `RO` → `EFI_ACCESS_DENIED`
4. 若 `Dangerous` 且未解鎖 → `EFI_ACCESS_DENIED`
5. 讀固定長度 hex（2/4/8 digits）
6. 寫入（8/16/32）
7. 讀回 verify（提示 masked/ignored/RO）

### 7.3 為什麼你會遇到「只能寫 B0~BF」

常見原因（README 可寫成 Troubleshooting）：

* 0xB0~ 可能是 **device-specific 的 vendor 寄存器**，很多裝置允許寫
* 其他 offset 屬於 **RO / vendor-locked / masked RW**
* 或該平台對 PCI config 有 **寫入限制（SMM/lockdown）**
* 或寫入必須先做 **magic unlock sequence**（vendor key）

---

## 8) Probe（偵測 masked RW / 可寫 bit）

### 8.1 Probe 的目的

你要回答：「這格到底是 RO？還是 masked RW？哪些 bit 寫得進去？」

### 8.2 Probe 的做法（你已實作）

* 讀 `Old`
* 寫 `Test = ~Old`
* 讀回 `ReadBack`
* 還原寫回 `Old`
* `WritableMask = Old XOR ReadBack`

### 8.3 Probe 為什麼限制 0x40~0xFF

避免碰到：

* Command/Status、BAR、CAP 這些會改變裝置行為的欄位
* 尤其 BAR 寫入會觸發 resource sizing / disable decoding

---

## 9) 常見錯誤與解法（你可以放 README）

### 9.1 “Write Status: EFI_SUCCESS 但值沒變”

原因：

* RO / masked RW（硬體遮罩）
* RW1C 你用「新值」寫會看起來沒變（因為邏輯是寫 1 清）
* 需要 vendor unlock key

建議：

* 一律做 read-back verify
* 用 Probe 取得 mask，再做 RMW：

  * `Final = (Old & ~Mask) | (New & Mask)`

### 9.2 “只能寫 B0~BF”

建議：

* 在 UI 顯示每格的：

  * `Policy`
  * `WritableMask`
* 讓使用者知道「能寫多少」

---

## 10) 編譯 / 專案配置要點（簡短版）

### 10.1 你的 `.inf` 必須包含

* `ENTRY_POINT = UefiMain`
* `LibraryClasses`：

  * `UefiLib`
  * `UefiBootServicesTableLib`
  * `MemoryAllocationLib`
  * `BaseLib`
  * `BaseMemoryLib`
  * `PrintLib`

### 10.2 你遇到的 `VPrint` 未定義

原因：

* 你用到 `VPrint` 但沒正確宣告/連結它的 library（或 toolchain 把 warning 當 error）
  解法（你現在版本已採用更穩的方式）：
* 用 `UnicodeVSPrint + Print("%s")` 取代 VPrint

---


______________________________________________________________________________________________________________________________________
cd /d D:\BIOS\MyWorkSpace\edk2

edksetup.bat Rebuild

chcp 65001

set PYTHONUTF8=1

set PYTHONIOENCODING=utf-8

rmdir /s /q Build\PciUtilityPkg

build -p PciUtilityPkg\PciUtilityPkg.dsc -a X64 -t VS2019 -b DEBUG
