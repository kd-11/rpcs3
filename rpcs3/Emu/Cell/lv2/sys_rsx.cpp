#include "stdafx.h"
#include "Emu/Memory/Memory.h"
#include "Emu/System.h"
#include "Emu/Cell/PPUModule.h"
#include "Emu/RSX/GSRender.h"
#include "Emu/IdManager.h"
#include "Emu/Cell/ErrorCodes.h"
#include "sys_rsx.h"
#include "sys_event.h"

namespace vm { using namespace ps3; }

logs::channel sys_rsx("sys_rsx");

struct RsxDriverInfo {
    be_t<u32> version_driver;     // 0x0
    be_t<u32> version_gpu;        // 0x4
    be_t<u32> memory_size;        // 0x8
    be_t<u32> hardware_channel;   // 0xC
    be_t<u32> nvcore_frequency;   // 0x10
    be_t<u32> memory_frequency;   // 0x14
    be_t<u32> unk1[4];            // 0x18 - 0x24
    be_t<u32> unk2;               // 0x28 -- pgraph stuff
    be_t<u32> reportsNotifyOffset;// 0x2C offset to notify memory
    be_t<u32> reportsOffset;      // 0x30 offset to reports memory
    be_t<u32> reportsReportOffset;// 0x34 offset to reports in reports memory
    be_t<u32> unk3[6];            // 0x38-0x54
    be_t<u32> systemModeFlags;    // 0x54
    u8 unk4[0x105C];              // 0x10B0
    struct Head {
        be_t<u64> unk;             // 0x0
        be_t<u64> lastFlip;        // 0x8 last flip time
        be_t<u32> flipFlags;       // 0x10 flags to handle flip/queue
        be_t<u32> unk1;            // 0x14
        be_t<u32> bufferId;        // 0x18
        be_t<u32> unk2;            // 0x1C
        be_t<u64> unk3;            // 0x20
        be_t<u64> lastSecondVTime; // 0x28 last time for second vhandler freq
        be_t<u64> vBlankCount;     // 0x30 
        be_t<u64> unk4;
    } head[8]; // size = 0x40
    be_t<u32> unk5;          // 0x12B0
    be_t<u32> unk6;          // 0x12B4
    be_t<u32> unk7;          // 0x12B8
    be_t<u32> unk8;          // 0x12BC
    be_t<u32> handlers;      // 0x12C0 -- flags showing which handlers are set
    be_t<u32> unk9;          // 0x12C4
    be_t<u32> unk10;         // 0x12C8
    be_t<u32> userCmdParam;  // 0x12CC
    be_t<u32> handler_queue; // 0x12D0
    be_t<u32> unk11;         // 0x12D4
    // todo: theres more to this 
};
template <size_t S> class Sizer { };
Sizer<sizeof(RsxDriverInfo)> foo;
static_assert(sizeof(RsxDriverInfo) == 0x12D8, "rsxSizeTest");
static_assert(sizeof(RsxDriverInfo::Head) == 0x40, "rsxHeadSizeTest");

struct RsxDmaControl {
    u8 resv[0x40];
    be_t<u32> put;
    be_t<u32> get;
    be_t<u32> ref;
    be_t<u32> unk[2];
    be_t<u32> unk1;
};

struct RsxSemaphore {
    be_t<u32> val;
    be_t<u32> pad;
    be_t<u64> timestamp;
};

struct RsxNotify {
    be_t<u64> timestamp;
    be_t<u64> zero;
};

struct RsxReport {
    be_t<u64> timestamp;
    be_t<u32> val;
    be_t<u32> pad;
};

struct RsxReports {
    RsxSemaphore semaphore[0x100];
    RsxNotify notify[64];
    RsxReport report[2048];
};

be_t<u32> g_rsx_event_port{ 0 };
u32 g_driverInfo{ 0 };

// this timestamp is a complete guess, it seems 'roughly' right for now so im just leaving it 
u64 rsxTimeStamp() {
    return (get_system_time() / 1000000 * 0x5F5E100);
}

s32 sys_rsx_device_open()
{
	sys_rsx.todo("sys_rsx_device_open()");

	return CELL_OK;
}

s32 sys_rsx_device_close()
{
	sys_rsx.todo("sys_rsx_device_close()");

	return CELL_OK;
}

/*
 * lv2 SysCall 668 (0x29C): sys_rsx_memory_allocate
 * @param mem_handle (OUT): Context / ID, which is used by sys_rsx_memory_free to free allocated memory.
 * @param mem_addr (OUT): Returns the local memory base address, usually 0xC0000000.
 * @param size (IN): Local memory size. E.g. 0x0F900000 (249 MB).
 * @param flags (IN): E.g. Immediate value passed in cellGcmSys is 8.
 * @param a5 (IN): E.g. Immediate value passed in cellGcmSys is 0x00300000 (3 MB?).
 * @param a6 (IN): E.g. Immediate value passed in cellGcmSys is 16. 
 * @param a7 (IN): E.g. Immediate value passed in cellGcmSys is 8.
 */
s32 sys_rsx_memory_allocate(vm::ptr<u32> mem_handle, vm::ptr<u64> mem_addr, u32 size, u64 flags, u64 a5, u64 a6, u64 a7)
{
	sys_rsx.todo("sys_rsx_memory_allocate(mem_handle=*0x%x, mem_addr=*0x%x, size=0x%x, flags=0x%llx, a5=0x%llx, a6=0x%llx, a7=0x%llx)", mem_handle, mem_addr, size, flags, a5, a6, a7);

    *mem_handle = 1;
    *mem_addr = vm::falloc(0xC0000000, size, vm::video);

	return CELL_OK;
}

/*
 * lv2 SysCall 669 (0x29D): sys_rsx_memory_free
 * @param mem_handle (OUT): Context / ID, for allocated local memory generated by sys_rsx_memory_allocate
 */
s32 sys_rsx_memory_free(u32 mem_handle)
{
	sys_rsx.todo("sys_rsx_memory_free(mem_handle=0x%x)", mem_handle);

	return CELL_OK;
}

/*
 * lv2 SysCall 670 (0x29E): sys_rsx_context_allocate
 * @param context_id (OUT): RSX context, E.g. 0x55555555 (in vsh.self)
 * @param lpar_dma_control (OUT): Control register area. E.g. 0x60100000 (in vsh.self)
 * @param lpar_driver_info (OUT): RSX data like frequencies, sizes, version... E.g. 0x60200000 (in vsh.self)
 * @param lpar_reports (OUT): Report data area. E.g. 0x60300000 (in vsh.self)
 * @param mem_ctx (IN): mem_ctx given by sys_rsx_memory_allocate
 * @param system_mode (IN):
 */
s32 sys_rsx_context_allocate(vm::ptr<u32> context_id, vm::ptr<u64> lpar_dma_control, vm::ptr<u64> lpar_driver_info, vm::ptr<u64> lpar_reports, u64 mem_ctx, u64 system_mode)
{
    sys_rsx.todo("sys_rsx_context_allocate(context_id=*0x%x, lpar_dma_control=*0x%x, lpar_driver_info=*0x%x, lpar_reports=*0x%x, mem_ctx=0x%llx, system_mode=0x%llx)",
        context_id, lpar_dma_control, lpar_driver_info, lpar_reports, mem_ctx, system_mode);

    vm::falloc(0x40000000, 0x10000000, vm::rsx_context);

    *context_id = 0x55555555;

    *lpar_dma_control = 0x40100000;
    *lpar_driver_info = 0x40200000;
    *lpar_reports = 0x40300000;

    auto &driverInfo = vm::_ref<RsxDriverInfo>(*lpar_driver_info);

    std::memset(&driverInfo, 0, sizeof(RsxDriverInfo));

    driverInfo.version_driver = 0x211;
    driverInfo.version_gpu = 0x5c;
    driverInfo.memory_size = 0xFE00000;
    driverInfo.nvcore_frequency = 500000000; // 0x1DCD6500
    driverInfo.memory_frequency = 650000000; // 0x26BE3680
    driverInfo.reportsNotifyOffset = 0x1000;
    driverInfo.reportsOffset = 0;
    driverInfo.reportsReportOffset = 0x1400;
    driverInfo.systemModeFlags = system_mode;

    g_driverInfo = *lpar_driver_info;

    auto &dmaControl = vm::_ref<RsxDmaControl>(*lpar_dma_control);
    dmaControl.get = 0;
    dmaControl.put = 0;

    if (false/*system_mode == CELL_GCM_SYSTEM_MODE_IOMAP_512MB*/)
        RSXIOMem.SetRange(0, 0x20000000 /*512MB*/);
    else
        RSXIOMem.SetRange(0, 0x10000000 /*256MB*/);

    sys_event_queue_attribute_t attr;
    attr.protocol = SYS_SYNC_PRIORITY;
    attr.type = SYS_PPU_QUEUE;
    auto queueId = vm::make_var<u32>(0);
    sys_event_queue_create(queueId, vm::make_var(attr), 0, 0x20);
    driverInfo.handler_queue = queueId->value();

    sys_event_port_create(queueId, SYS_EVENT_PORT_LOCAL, 0);
    sys_event_port_connect_local(queueId->value(), driverInfo.handler_queue);

    g_rsx_event_port = queueId->value();

    const auto render = fxm::get<GSRender>();
    render->ctrl = vm::_ptr<CellGcmControl>(*lpar_dma_control);
    //render->intr_thread = idm::make_ptr<ppu_thread>("_gcm_intr_thread", 1, 0x4000);
    //render->intr_thread->run();
    //render->ctxt_addr = 0;
    render->gcm_buffers.set(vm::alloc(sizeof(CellGcmDisplayInfo) * 8, vm::main));
    render->zculls_addr = vm::alloc(sizeof(CellGcmZcullInfo) * 8, vm::main);
    render->tiles_addr = vm::alloc(sizeof(CellGcmTileInfo) * 15, vm::main);
    render->gcm_buffers_count = 7;
    render->gcm_current_buffer = 0;
    render->main_mem_addr = 0;
    render->label_addr = *lpar_reports;
    render->init(0x30100000, 0x200000, *lpar_dma_control, 0xC0000000);

	return CELL_OK;
}

/*
 * lv2 SysCall 671 (0x29F): sys_rsx_context_free
 * @param context_id (IN): RSX context generated by sys_rsx_context_allocate to free the context.
 */
s32 sys_rsx_context_free(u32 context_id)
{
	sys_rsx.todo("sys_rsx_context_free(context_id=0x%x)", context_id);

	return CELL_OK;
}

/*
 * lv2 SysCall 672 (0x2A0): sys_rsx_context_iomap
 * @param context_id (IN): RSX context, E.g. 0x55555555 (in vsh.self)
 * @param io (IN): IO offset mapping area. E.g. 0x00600000
 * @param ea (IN): Start address of mapping area. E.g. 0x20400000
 * @param size (IN): Size of mapping area in bytes. E.g. 0x00200000
 * @param flags (IN):
 */
s32 sys_rsx_context_iomap(u32 context_id, u32 io, u32 ea, u32 size, u64 flags)
{
	sys_rsx.todo("sys_rsx_context_iomap(context_id=0x%x, io=0x%x, ea=0x%x, size=0x%x, flags=0x%llx)", context_id, io, ea, size, flags);
    if (RSXIOMem.Map(ea, size, io))
	    return CELL_OK;
    LOG_ERROR(RSX, "rsx_iomap failed");
    return CELL_EINVAL;
}

/*
 * lv2 SysCall 673 (0x2A1): sys_rsx_context_iounmap
 * @param context_id (IN): RSX context, E.g. 0x55555555 (in vsh.self)
 * @param a2 (IN): ?
 * @param io_addr (IN): IO address. E.g. 0x00600000 (Start page 6)
 * @param size (IN): Size to unmap in byte. E.g. 0x00200000
 */
s32 sys_rsx_context_iounmap(u32 context_id, u32 io_addr, u32 a3, u32 size)
{
	sys_rsx.todo("sys_rsx_context_iounmap(context_id=0x%x, io_addr=0x%x, a3=0x%x, size=0x%x)", context_id, io_addr, a3, size);
    if (RSXIOMem.UnmapAddress(io_addr, size))
	    return CELL_OK;
    LOG_ERROR(RSX, "rsx_iounmap failed");
    return CELL_EINVAL;
}

/*
 * lv2 SysCall 674 (0x2A2): sys_rsx_context_attribute
 * @param context_id (IN): RSX context, e.g. 0x55555555
 * @param package_id (IN): 
 * @param a3 (IN): 
 * @param a4 (IN): 
 * @param a5 (IN): 
 * @param a6 (IN): 
 */
s32 sys_rsx_context_attribute(s32 context_id, u32 package_id, u64 a3, u64 a4, u64 a5, u64 a6)
{
    if (package_id != 0x101)
	sys_rsx.todo("sys_rsx_context_attribute(context_id=0x%x, package_id=0x%x, a3=0x%llx, a4=0x%llx, a5=0x%llx, a6=0x%llx)", context_id, package_id, a3, a4, a5, a6);

    // hle/lle protection
    if (g_driverInfo == 0)
        return CELL_OK;
    // todo: these event ports probly 'shouldnt' be here as i think its supposed to be interrupts that are sent from rsx somewhere in lv1

    const auto render = fxm::get<GSRender>();
    auto &driverInfo = vm::_ref<RsxDriverInfo>(g_driverInfo);
	switch(package_id)
	{
	case 0x001: // FIFO
        render->ctrl->get = a3;
        render->ctrl->put = a4;
		break;
	
	case 0x100: // Display mode set
        break;
    case 0x101: // Display sync
        // todo: this is wrong and should be 'second' vblank handler and freq
        // although gcmSys seems just hardcoded at 1, so w/e
        driverInfo.head[1].vBlankCount++;
        driverInfo.head[1].lastSecondVTime = rsxTimeStamp();
        sys_event_port_send(g_rsx_event_port, 0, (1 << 1), 0);
        sys_event_port_send(g_rsx_event_port, 0, (1 << 11), 0); // second vhandler
		break;

	case 0x102: // Display flip
        driverInfo.head[a3].flipFlags |= 0x80000000;
        driverInfo.head[a3].lastFlip = rsxTimeStamp(); // should rsxthread set this?
        // lets give this a shot for giving bufferid back to gcm
        driverInfo.head[a3].bufferId = a4 & 0xFF;
        if (a3 == 0)
            sys_event_port_send(g_rsx_event_port, 0, (1 << 3), 0);
        if (a3 == 1)
            sys_event_port_send(g_rsx_event_port, 0, (1 << 4), 0);
		break;

	case 0x103: // Display Queue
        driverInfo.head[a3].flipFlags |= 0x40000000 | (1 << a4);
        if (a3 == 0)
            sys_event_port_send(g_rsx_event_port, 0, (1 << 5), 0);
        if (a3 == 1)
            sys_event_port_send(g_rsx_event_port, 0, (1 << 6), 0);
		break;
	case 0x104: // Display buffer
    {
        u8 id = a3 & 0xFF;
        u32 width = (a4 >> 32) & 0xFFFFFFFF;
        u32 height = a4 & 0xFFFFFFFF;
        u32 pitch = (a5 >> 32) & 0xFFFFFFFF;
        u32 offset = a5 & 0xFFFFFFFF;
        if (id > 7)
            return -17;
        render->gcm_buffers[id].width = width;
        render->gcm_buffers[id].height = height;
        render->gcm_buffers[id].pitch = pitch;
        render->gcm_buffers[id].offset = offset;
    }
	break;
    case 0x105: // destroy buffer?
        break;

	case 0x106: // ? (Used by cellGcmInitPerfMon)
		break;
    case 0x108: // ? set interrupt freq?
        break;
	case 0x10a: // ? Involved in managing flip status through cellGcmResetFlipStatus
    {
        if (a3 > 7)
            return -17;
        u32 flipStatus = driverInfo.head[a3].flipFlags;
        flipStatus = (flipStatus & a4) | a5;
        driverInfo.head[a3].flipFlags = flipStatus;
    }
	break;

    case 0x10D: // Called by cellGcmInitCursor
        break;

	case 0x300: // Tiles
		break;

	case 0x301: // Depth-buffer (Z-cull)
		break;
    case 0x302: // something with zcull
        break;
	case 0x600: // Framebuffer setup
		break;

	case 0x601: // Framebuffer blit
		break;

	case 0x602: // Framebuffer blit sync
		break;

    case 0x603: // Framebuffer close
        break;

    case 0xFEF: // hack: user command
        // 'custom' invalid package id for now 
        // as i think we need custom lv1 interrupts to handle this accurately
        // this also should probly be set by rsxthread
        driverInfo.userCmdParam = a4;
        sys_event_port_send(g_rsx_event_port, 0, (1 << 7), 0);
        break;
	default:
		return CELL_EINVAL;
	}

	return CELL_OK;
}

/*
 * lv2 SysCall 675 (0x2A3): sys_rsx_device_map
 * @param a1 (OUT): For example: In vsh.self it is 0x60000000, global semaphore. For a game it is 0x40000000.
 * @param a2 (OUT): Unused?
 * @param dev_id (IN): An immediate value and always 8. (cellGcmInitPerfMon uses 11, 10, 9, 7, 12 successively).
 */
s32 sys_rsx_device_map(vm::ptr<u64> addr, vm::ptr<u64> a2, u32 dev_id)
{
	sys_rsx.todo("sys_rsx_device_map(addr=*0x%x, a2=*0x%x, dev_id=0x%x)", addr, a2, dev_id);

    if (dev_id != 8) {
		// TODO: lv1 related 
        fmt::throw_exception("sys_rsx_device_map: Invalid dev_id %d", dev_id);
	}

    // a2 seems to not be referenced in cellGcmSys
    *a2 = 0;

    *addr = 0x40000000;

	return CELL_OK;
}

/*
 * lv2 SysCall 676 (0x2A4): sys_rsx_device_unmap
 * @param dev_id (IN): An immediate value and always 8.
 */
s32 sys_rsx_device_unmap(u32 dev_id)
{
	sys_rsx.todo("sys_rsx_device_unmap(dev_id=0x%x)", dev_id);

	return CELL_OK;
}

s32 sys_rsx_attribute(u32 packageId, u32 a2, u32 a3, u32 a4, u32 a5)
{
	sys_rsx.todo("sys_rsx_attribute(packageId=0x%x, a2=0x%x, a3=0x%x, a4=0x%x, a5=0x%x)", packageId, a2, a3, a4, a5);

	return CELL_OK;
}
