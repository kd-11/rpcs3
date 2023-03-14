R"(
// TEB flags
#define SPU_HLT               1

// SPU CH
#define SPU_RdEventStat       0   // Read event status with mask applied
#define SPU_WrEventMask       1   // Write event mask
#define SPU_WrEventAck        2   // Write end of event processing
#define SPU_RdSigNotify1      3   // Signal notification 1
#define SPU_RdSigNotify2      4   // Signal notification 2
#define SPU_WrDec             7   // Write decrementer count
#define SPU_RdDec             8   // Read decrementer count
#define SPU_RdEventMask       11  // Read event mask
#define SPU_RdMachStat        13  // Read SPU run status
#define SPU_WrSRR0            14  // Write SPU machine state save/restore register 0 (SRR0)
#define SPU_RdSRR0            15  // Read SPU machine state save/restore register 0 (SRR0)
#define SPU_WrOutMbox         28  // Write outbound mailbox contents
#define SPU_RdInMbox          29  // Read inbound mailbox contents
#define SPU_WrOutIntrMbox     30  // Write outbound interrupt mailbox contents (interrupting PPU)
#define SPU_Set_Bkmk_Tag      69  // Causes an event that can be logged in the performance monitor logic if enabled in the SPU
#define SPU_PM_Start_Ev       70  // Starts the performance monitor event if enabled
#define SPU_PM_Stop_Ev        71  // Stops the performance monitor event if enabled

// MFC Channels
#define MFC_WrMSSyncReq       9   // Write multisource synchronization request
#define MFC_RdTagMask         12  // Read tag mask
#define MFC_LSA               16  // Write local memory address command parameter
#define MFC_EAH               17  // Write high order DMA effective address command parameter
#define MFC_EAL               18  // Write low order DMA effective address command parameter
#define MFC_Size              19  // Write DMA transfer size command parameter
#define MFC_TagID             20  // Write tag identifier command parameter
#define MFC_Cmd               21  // Write and enqueue DMA command with associated class ID
#define MFC_WrTagMask         22  // Write tag mask
#define MFC_WrTagUpdate       23  // Write request for conditional or unconditional tag status update
#define MFC_RdTagStat         24  // Read tag status with mask applied
#define MFC_RdListStallStat   25  // Read DMA list stall-and-notify status
#define MFC_WrListStallAck    26  // Write DMA list stall-and-notify acknowledge
#define MFC_RdAtomicStat      27  // Read completion status of last completed immediate MFC atomic update command
)"
