#include "stdafx.h"
#include "Emu/IdManager.h"
#include "Emu/Cell/PPUModule.h"

#ifdef _MSC_VER
#pragma warning(push, 0)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
extern "C"
{
#include "libswscale/swscale.h"
}
#ifdef _MSC_VER
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

#include "cellVpost.h"

LOG_CHANNEL(cellVpost);

template<>
void fmt_class_string<CellVpostError>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto error)
	{
		switch (error)
		{
			STR_CASE(CELL_VPOST_ERROR_Q_ARG_CFG_NULL);
			STR_CASE(CELL_VPOST_ERROR_Q_ARG_CFG_INVALID);
			STR_CASE(CELL_VPOST_ERROR_Q_ARG_ATTR_NULL);
			STR_CASE(CELL_VPOST_ERROR_O_ARG_CFG_NULL);
			STR_CASE(CELL_VPOST_ERROR_O_ARG_CFG_INVALID);
			STR_CASE(CELL_VPOST_ERROR_O_ARG_RSRC_NULL);
			STR_CASE(CELL_VPOST_ERROR_O_ARG_RSRC_INVALID);
			STR_CASE(CELL_VPOST_ERROR_O_ARG_HDL_NULL);
			STR_CASE(CELL_VPOST_ERROR_O_FATAL_QUERY_FAIL);
			STR_CASE(CELL_VPOST_ERROR_O_FATAL_CREATEMON_FAIL);
			STR_CASE(CELL_VPOST_ERROR_O_FATAL_INITSPURS_FAIL);
			STR_CASE(CELL_VPOST_ERROR_C_ARG_HDL_NULL);
			STR_CASE(CELL_VPOST_ERROR_C_ARG_HDL_INVALID);
			STR_CASE(CELL_VPOST_ERROR_C_FATAL_LOCKMON_FAIL);
			STR_CASE(CELL_VPOST_ERROR_C_FATAL_UNLOCKMON_FAIL);
			STR_CASE(CELL_VPOST_ERROR_C_FATAL_DESTROYMON_FAIL);
			STR_CASE(CELL_VPOST_ERROR_C_FATAL_FINSPURS_FAIL);
			STR_CASE(CELL_VPOST_ERROR_E_ARG_HDL_NULL);
			STR_CASE(CELL_VPOST_ERROR_E_ARG_HDL_INVALID);
			STR_CASE(CELL_VPOST_ERROR_E_ARG_INPICBUF_NULL);
			STR_CASE(CELL_VPOST_ERROR_E_ARG_INPICBUF_INVALID);
			STR_CASE(CELL_VPOST_ERROR_E_ARG_CTRL_NULL);
			STR_CASE(CELL_VPOST_ERROR_E_ARG_CTRL_INVALID);
			STR_CASE(CELL_VPOST_ERROR_E_ARG_OUTPICBUF_NULL);
			STR_CASE(CELL_VPOST_ERROR_E_ARG_OUTPICBUF_INVALID);
			STR_CASE(CELL_VPOST_ERROR_E_ARG_PICINFO_NULL);
			STR_CASE(CELL_VPOST_ERROR_E_FATAL_LOCKMON_FAIL);
			STR_CASE(CELL_VPOST_ERROR_E_FATAL_UNLOCKMON_FAIL);
			STR_CASE(CELL_VPOST_ENT_ERROR_Q_ARG_ATTR_NULL);
			STR_CASE(CELL_VPOST_ENT_ERROR_O_ARG_RSRC_NULL);
			STR_CASE(CELL_VPOST_ENT_ERROR_O_ARG_RSRC_INVALID);
			STR_CASE(CELL_VPOST_ENT_ERROR_O_ARG_HDL_NULL);
			STR_CASE(CELL_VPOST_ENT_ERROR_O_FATAL_QUERY_FAIL);
			STR_CASE(CELL_VPOST_ENT_ERROR_O_FATAL_CSPUCORE_FAIL);
			STR_CASE(CELL_VPOST_ENT_ERROR_C_ARG_HDL_NULL);
			STR_CASE(CELL_VPOST_ENT_ERROR_C_ARG_HDL_INVALID);
			STR_CASE(CELL_VPOST_ENT_ERROR_C_FATAL_SNDCMD_FAIL);
			STR_CASE(CELL_VPOST_ENT_ERROR_C_FATAL_RCVRES_FAIL);
			STR_CASE(CELL_VPOST_ENT_ERROR_C_FATAL_DSPUCORE_FAIL);
			STR_CASE(CELL_VPOST_ENT_ERROR_E_ARG_HDL_NULL);
			STR_CASE(CELL_VPOST_ENT_ERROR_E_ARG_HDL_INVALID);
			STR_CASE(CELL_VPOST_ENT_ERROR_E_ARG_INPICBUF_NULL);
			STR_CASE(CELL_VPOST_ENT_ERROR_E_ARG_INPICBUF_INVALID);
			STR_CASE(CELL_VPOST_ENT_ERROR_E_ARG_INPICINFO_NULL);
			STR_CASE(CELL_VPOST_ENT_ERROR_E_ARG_INPICINFO_INVALID);
			STR_CASE(CELL_VPOST_ENT_ERROR_E_ARG_CTRL_NULL);
			STR_CASE(CELL_VPOST_ENT_ERROR_E_ARG_CTRL_INVALID);
			STR_CASE(CELL_VPOST_ENT_ERROR_E_ARG_COMB_INVALID);
			STR_CASE(CELL_VPOST_ENT_ERROR_E_ARG_OUTPICBUF_NULL);
			STR_CASE(CELL_VPOST_ENT_ERROR_E_ARG_OUTPICBUF_INVALID);
			STR_CASE(CELL_VPOST_ENT_ERROR_E_ARG_OUTPICINFO_NULL);
			STR_CASE(CELL_VPOST_ENT_ERROR_E_FATAL_SNDCMD_FAIL);
			STR_CASE(CELL_VPOST_ENT_ERROR_E_FATAL_RCVRES_FAIL);
			STR_CASE(CELL_VPOST_ENT_ERROR_E_FATAL_SPUCORE_FAIL);
			STR_CASE(CELL_VPOST_IPC_ERROR_Q_ARG_ATTR_NULL);
			STR_CASE(CELL_VPOST_IPC_ERROR_O_ARG_RSRC_NULL);
			STR_CASE(CELL_VPOST_IPC_ERROR_O_ARG_RSRC_INVALID);
			STR_CASE(CELL_VPOST_IPC_ERROR_O_ARG_HDL_NULL);
			STR_CASE(CELL_VPOST_IPC_ERROR_O_FATAL_QUERY_FAIL);
			STR_CASE(CELL_VPOST_IPC_ERROR_O_FATAL_CSPUCORE_FAIL);
			STR_CASE(CELL_VPOST_IPC_ERROR_C_ARG_HDL_NULL);
			STR_CASE(CELL_VPOST_IPC_ERROR_C_ARG_HDL_INVALID);
			STR_CASE(CELL_VPOST_IPC_ERROR_C_FATAL_SNDCMD_FAIL);
			STR_CASE(CELL_VPOST_IPC_ERROR_C_FATAL_RCVRES_FAIL);
			STR_CASE(CELL_VPOST_IPC_ERROR_C_FATAL_DSPUCORE_FAIL);
			STR_CASE(CELL_VPOST_IPC_ERROR_E_ARG_HDL_NULL);
			STR_CASE(CELL_VPOST_IPC_ERROR_E_ARG_HDL_INVALID);
			STR_CASE(CELL_VPOST_IPC_ERROR_E_ARG_INPICBUF_NULL);
			STR_CASE(CELL_VPOST_IPC_ERROR_E_ARG_INPICBUF_INVALID);
			STR_CASE(CELL_VPOST_IPC_ERROR_E_ARG_INPICINFO_NULL);
			STR_CASE(CELL_VPOST_IPC_ERROR_E_ARG_INPICINFO_INVALID);
			STR_CASE(CELL_VPOST_IPC_ERROR_E_ARG_CTRL_NULL);
			STR_CASE(CELL_VPOST_IPC_ERROR_E_ARG_CTRL_INVALID);
			STR_CASE(CELL_VPOST_IPC_ERROR_E_ARG_COMB_INVALID);
			STR_CASE(CELL_VPOST_IPC_ERROR_E_ARG_OUTPICBUF_NULL);
			STR_CASE(CELL_VPOST_IPC_ERROR_E_ARG_OUTPICBUF_INVALID);
			STR_CASE(CELL_VPOST_IPC_ERROR_E_ARG_OUTPICINFO_NULL);
			STR_CASE(CELL_VPOST_IPC_ERROR_E_FATAL_SNDCMD_FAIL);
			STR_CASE(CELL_VPOST_IPC_ERROR_E_FATAL_RCVRES_FAIL);
			STR_CASE(CELL_VPOST_IPC_ERROR_E_FATAL_SPUCORE_FAIL);
			STR_CASE(CELL_VPOST_VSC_ERROR_Q_ARG_ATTR_NULL);
			STR_CASE(CELL_VPOST_VSC_ERROR_O_ARG_RSRC_NULL);
			STR_CASE(CELL_VPOST_VSC_ERROR_O_ARG_RSRC_INVALID);
			STR_CASE(CELL_VPOST_VSC_ERROR_O_ARG_HDL_NULL);
			STR_CASE(CELL_VPOST_VSC_ERROR_O_FATAL_QUERY_FAIL);
			STR_CASE(CELL_VPOST_VSC_ERROR_O_FATAL_CSPUCORE_FAIL);
			STR_CASE(CELL_VPOST_VSC_ERROR_C_ARG_HDL_NULL);
			STR_CASE(CELL_VPOST_VSC_ERROR_C_ARG_HDL_INVALID);
			STR_CASE(CELL_VPOST_VSC_ERROR_C_FATAL_SNDCMD_FAIL);
			STR_CASE(CELL_VPOST_VSC_ERROR_C_FATAL_RCVRES_FAIL);
			STR_CASE(CELL_VPOST_VSC_ERROR_C_FATAL_DSPUCORE_FAIL);
			STR_CASE(CELL_VPOST_VSC_ERROR_E_ARG_HDL_NULL);
			STR_CASE(CELL_VPOST_VSC_ERROR_E_ARG_HDL_INVALID);
			STR_CASE(CELL_VPOST_VSC_ERROR_E_ARG_INPICBUF_NULL);
			STR_CASE(CELL_VPOST_VSC_ERROR_E_ARG_INPICBUF_INVALID);
			STR_CASE(CELL_VPOST_VSC_ERROR_E_ARG_INPICINFO_NULL);
			STR_CASE(CELL_VPOST_VSC_ERROR_E_ARG_INPICINFO_INVALID);
			STR_CASE(CELL_VPOST_VSC_ERROR_E_ARG_CTRL_NULL);
			STR_CASE(CELL_VPOST_VSC_ERROR_E_ARG_CTRL_INVALID);
			STR_CASE(CELL_VPOST_VSC_ERROR_E_ARG_COMB_INVALID);
			STR_CASE(CELL_VPOST_VSC_ERROR_E_ARG_OUTPICBUF_NULL);
			STR_CASE(CELL_VPOST_VSC_ERROR_E_ARG_OUTPICBUF_INVALID);
			STR_CASE(CELL_VPOST_VSC_ERROR_E_ARG_OUTPICINFO_NULL);
			STR_CASE(CELL_VPOST_VSC_ERROR_E_FATAL_SNDCMD_FAIL);
			STR_CASE(CELL_VPOST_VSC_ERROR_E_FATAL_RCVRES_FAIL);
			STR_CASE(CELL_VPOST_VSC_ERROR_E_FATAL_SPUCORE_FAIL);
			STR_CASE(CELL_VPOST_CSC_ERROR_Q_ARG_ATTR_NULL);
			STR_CASE(CELL_VPOST_CSC_ERROR_O_ARG_RSRC_NULL);
			STR_CASE(CELL_VPOST_CSC_ERROR_O_ARG_RSRC_INVALID);
			STR_CASE(CELL_VPOST_CSC_ERROR_O_ARG_HDL_NULL);
			STR_CASE(CELL_VPOST_CSC_ERROR_O_FATAL_QUERY_FAIL);
			STR_CASE(CELL_VPOST_CSC_ERROR_O_FATAL_CSPUCORE_FAIL);
			STR_CASE(CELL_VPOST_CSC_ERROR_C_ARG_HDL_NULL);
			STR_CASE(CELL_VPOST_CSC_ERROR_C_ARG_HDL_INVALID);
			STR_CASE(CELL_VPOST_CSC_ERROR_C_FATAL_SNDCMD_FAIL);
			STR_CASE(CELL_VPOST_CSC_ERROR_C_FATAL_RCVRES_FAIL);
			STR_CASE(CELL_VPOST_CSC_ERROR_C_FATAL_DSPUCORE_FAIL);
			STR_CASE(CELL_VPOST_CSC_ERROR_E_ARG_HDL_NULL);
			STR_CASE(CELL_VPOST_CSC_ERROR_E_ARG_HDL_INVALID);
			STR_CASE(CELL_VPOST_CSC_ERROR_E_ARG_INPICBUF_NULL);
			STR_CASE(CELL_VPOST_CSC_ERROR_E_ARG_INPICBUF_INVALID);
			STR_CASE(CELL_VPOST_CSC_ERROR_E_ARG_INPICINFO_NULL);
			STR_CASE(CELL_VPOST_CSC_ERROR_E_ARG_INPICINFO_INVALID);
			STR_CASE(CELL_VPOST_CSC_ERROR_E_ARG_CTRL_NULL);
			STR_CASE(CELL_VPOST_CSC_ERROR_E_ARG_CTRL_INVALID);
			STR_CASE(CELL_VPOST_CSC_ERROR_E_ARG_COMB_INVALID);
			STR_CASE(CELL_VPOST_CSC_ERROR_E_ARG_OUTPICBUF_NULL);
			STR_CASE(CELL_VPOST_CSC_ERROR_E_ARG_OUTPICBUF_INVALID);
			STR_CASE(CELL_VPOST_CSC_ERROR_E_ARG_OUTPICINFO_NULL);
			STR_CASE(CELL_VPOST_CSC_ERROR_E_FATAL_SNDCMD_FAIL);
			STR_CASE(CELL_VPOST_CSC_ERROR_E_FATAL_RCVRES_FAIL);
			STR_CASE(CELL_VPOST_CSC_ERROR_E_FATAL_SPUCORE_FAIL);
		}

		return unknown;
	});
}

namespace vpost_internal
{
	error_code open_instance(vm::cptr<CellVpostCfgParam> cfgParam, vm::ptr<u32> handle)
	{
		if (!cfgParam)
		{
			return CELL_VPOST_ERROR_O_ARG_CFG_NULL;
		}

		if (!handle)
		{
			return CELL_VPOST_ERROR_O_ARG_HDL_NULL;
		}

		const CellVpostCfgParam cfg = *cfgParam;

		if (!is_valid_max_size(cfg.inMaxWidth, cfg.inMaxHeight) || !is_valid_max_size(cfg.outMaxWidth, cfg.outMaxHeight) ||
			cfg.inDepth != CELL_VPOST_PIC_DEPTH_8 || cfg.outDepth != CELL_VPOST_PIC_DEPTH_8 || cfg.inPicFmt != CELL_VPOST_PIC_FMT_IN_YUV420_PLANAR ||
			(cfg.outPicFmt != CELL_VPOST_PIC_FMT_OUT_RGBA_ILV && cfg.outPicFmt != CELL_VPOST_PIC_FMT_OUT_YUV420_PLANAR))
		{
			return { CELL_VPOST_ERROR_O_ARG_CFG_INVALID, "in=%dx%d (depth=%d, fmt=%d), out=%dx%d (depth=%d, fmt=%d)",
				cfg.inMaxWidth, cfg.inMaxHeight, cfg.inDepth, cfg.inPicFmt, cfg.outMaxWidth, cfg.outMaxHeight, cfg.outDepth, cfg.outPicFmt };
		}

		if (cfg.outPicFmt != CELL_VPOST_PIC_FMT_OUT_RGBA_ILV)
		{
			cellVpost.todo("YUV420 output is not implemented, RGBA is written instead");
		}

		const u32 id = idm::make<VpostInstance>(cfg.outPicFmt == CELL_VPOST_PIC_FMT_OUT_RGBA_ILV,
			picture_limits{cfg.inMaxWidth, cfg.inMaxHeight, cfg.outMaxWidth, cfg.outMaxHeight});

		if (!id)
		{
			return CELL_VPOST_ERROR_O_FATAL_CREATEMON_FAIL;
		}

		*handle = id;
		return CELL_OK;
	}
}

using namespace vpost_internal;

error_code cellVpostQueryAttr(vm::cptr<CellVpostCfgParam> cfgParam, vm::ptr<CellVpostAttr> attr)
{
	cellVpost.warning("cellVpostQueryAttr(cfgParam=*0x%x, attr=*0x%x)", cfgParam, attr);

	if (!cfgParam)
	{
		return CELL_VPOST_ERROR_Q_ARG_CFG_NULL;
	}

	if (!attr)
	{
		return CELL_VPOST_ERROR_Q_ARG_ATTR_NULL;
	}

	// TODO: check cfgParam and output values

	attr->delay = 0;
	attr->memSize = 4 * 1024 * 1024; // 4 MB
	attr->vpostVerLower = 0x280000; // from dmux
	attr->vpostVerUpper = 0x260000;

	return CELL_OK;
}

error_code cellVpostOpen(vm::cptr<CellVpostCfgParam> cfgParam, vm::cptr<CellVpostResource> resource, vm::ptr<u32> handle)
{
	cellVpost.warning("cellVpostOpen(cfgParam=*0x%x, resource=*0x%x, handle=*0x%x)", cfgParam, resource, handle);

	return open_instance(cfgParam, handle);
}

error_code cellVpostOpenEx(vm::cptr<CellVpostCfgParam> cfgParam, vm::cptr<CellVpostResourceEx> resource, vm::ptr<u32> handle)
{
	cellVpost.warning("cellVpostOpenEx(cfgParam=*0x%x, resource=*0x%x, handle=*0x%x)", cfgParam, resource, handle);

	return open_instance(cfgParam, handle);
}

error_code cellVpostClose(u32 handle)
{
	cellVpost.warning("cellVpostClose(handle=0x%x)", handle);

	const auto vpost = idm::get_unlocked<VpostInstance>(handle);

	if (!vpost)
	{
		return CELL_VPOST_ERROR_C_ARG_HDL_INVALID;
	}

	idm::remove<VpostInstance>(handle);
	return CELL_OK;
}

error_code cellVpostExec(u32 handle, vm::cptr<u8> inPicBuff, vm::cptr<CellVpostCtrlParam> ctrlParam, vm::ptr<u8> outPicBuff, vm::ptr<CellVpostPictureInfo> picInfo)
{
	cellVpost.trace("cellVpostExec(handle=0x%x, inPicBuff=*0x%x, ctrlParam=*0x%x, outPicBuff=*0x%x, picInfo=*0x%x)", handle, inPicBuff, ctrlParam, outPicBuff, picInfo);

	const auto vpost = idm::get_unlocked<VpostInstance>(handle);

	if (!vpost)
	{
		return CELL_VPOST_ERROR_E_ARG_HDL_INVALID;
	}

	if (!inPicBuff)
	{
		return CELL_VPOST_ERROR_E_ARG_INPICBUF_NULL;
	}

	if (!ctrlParam)
	{
		return CELL_VPOST_ERROR_E_ARG_CTRL_NULL;
	}

	if (!outPicBuff)
	{
		return CELL_VPOST_ERROR_E_ARG_OUTPICBUF_NULL;
	}

	if (!picInfo)
	{
		return CELL_VPOST_ERROR_E_ARG_PICINFO_NULL;
	}

	// Read the parameters once: the guest can change them while the picture is converted
	const CellVpostCtrlParam ctrl = *ctrlParam;

	const u32 w = ctrl.inWidth;
	const u32 h = ctrl.inHeight;
	const u32 ow = ctrl.outWidth;
	const u32 oh = ctrl.outHeight;

	//ctrl.inWindow; // ignored
	if (ctrl.inWindow.x) cellVpost.notice("*** inWindow.x = %d", ctrl.inWindow.x);
	if (ctrl.inWindow.y) cellVpost.notice("*** inWindow.y = %d", ctrl.inWindow.y);
	if (ctrl.inWindow.width != w) cellVpost.notice("*** inWindow.width = %d", ctrl.inWindow.width);
	if (ctrl.inWindow.height != h) cellVpost.notice("*** inWindow.height = %d", ctrl.inWindow.height);
	//ctrl.outWindow; // ignored
	if (ctrl.outWindow.x) cellVpost.notice("*** outWindow.x = %d", ctrl.outWindow.x);
	if (ctrl.outWindow.y) cellVpost.notice("*** outWindow.y = %d", ctrl.outWindow.y);
	if (ctrl.outWindow.width != ow) cellVpost.notice("*** outWindow.width = %d", ctrl.outWindow.width);
	if (ctrl.outWindow.height != oh) cellVpost.notice("*** outWindow.height = %d", ctrl.outWindow.height);
	//ctrl.execType; // ignored
	//ctrl.scalerType; // ignored
	//ctrl.ipcType; // ignored

	const auto layout = get_picture_layout(vpost->limits, w, h, ow, oh);

	if (!layout)
	{
		return { CELL_VPOST_ERROR_E_ARG_CTRL_INVALID, "in=%dx%d, out=%dx%d, max in=%dx%d, max out=%dx%d", w, h, ow, oh,
			vpost->limits.in_max_width, vpost->limits.in_max_height, vpost->limits.out_max_width, vpost->limits.out_max_height };
	}

	if (!vm::check_addr(inPicBuff.addr(), vm::page_readable, layout->in_size))
	{
		return { CELL_VPOST_ERROR_E_ARG_INPICBUF_INVALID, "inPicBuff=*0x%x, size=0x%x", inPicBuff, layout->in_size };
	}

	if (!vm::check_addr(outPicBuff.addr(), vm::page_writable, layout->out_size))
	{
		return { CELL_VPOST_ERROR_E_ARG_OUTPICBUF_INVALID, "outPicBuff=*0x%x, size=0x%x", outPicBuff, layout->out_size };
	}

	picInfo->inWidth = w; // copy
	picInfo->inHeight = h; // copy
	picInfo->inDepth = CELL_VPOST_PIC_DEPTH_8; // fixed
	picInfo->inScanType = CELL_VPOST_SCAN_TYPE_P; // TODO
	picInfo->inPicFmt = CELL_VPOST_PIC_FMT_IN_YUV420_PLANAR; // fixed
	picInfo->inChromaPosType = ctrl.inChromaPosType; // copy
	picInfo->inPicStruct = CELL_VPOST_PIC_STRUCT_PFRM; // TODO
	picInfo->inQuantRange = ctrl.inQuantRange; // copy
	picInfo->inColorMatrix = ctrl.inColorMatrix; // copy

	picInfo->outWidth = ow; // copy
	picInfo->outHeight = oh; // copy
	picInfo->outDepth = CELL_VPOST_PIC_DEPTH_8; // fixed
	picInfo->outScanType = CELL_VPOST_SCAN_TYPE_P; // TODO
	picInfo->outPicFmt = CELL_VPOST_PIC_FMT_OUT_RGBA_ILV; // TODO
	picInfo->outChromaPosType = ctrl.inChromaPosType; // ignored
	picInfo->outPicStruct = picInfo->inPicStruct; // ignored
	picInfo->outQuantRange = ctrl.inQuantRange; // ignored
	picInfo->outColorMatrix = ctrl.inColorMatrix; // ignored

	picInfo->userData = ctrl.userData; // copy
	picInfo->reserved1 = 0;
	picInfo->reserved2 = 0;

	std::lock_guard lock(vpost->mutex);

	// swscale converts between host buffers. Its SIMD paths may access a few bytes past the end of a plane, hence the padding.
	constexpr usz padding = 64;
	const u32 y_size = w * h;

	vpost->in_buf.resize(layout->in_size + padding);
	vpost->alpha_buf.resize(y_size + padding);
	vpost->out_buf.resize(layout->out_size + padding);

	std::memcpy(vpost->in_buf.data(), vm::_ptr<const u8>(inPicBuff.addr()), layout->in_size);
	std::memset(vpost->alpha_buf.data(), ctrl.outAlpha, y_size);

	vpost->sws = sws_getCachedContext(vpost->sws, w, h, AV_PIX_FMT_YUVA420P, ow, oh, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);

	if (!vpost->sws)
	{
		return { CELL_VPOST_ERROR_E_ARG_CTRL_INVALID, "sws_getCachedContext failed (in=%dx%d, out=%dx%d)", w, h, ow, oh };
	}

	const u8* in = vpost->in_buf.data();
	const u8* in_data[4] = { in, in + y_size, in + y_size * 5 / 4, vpost->alpha_buf.data() };
	const int in_line[4] = { static_cast<int>(w), static_cast<int>(w / 2), static_cast<int>(w / 2), static_cast<int>(w) };
	u8* out_data[4] = { vpost->out_buf.data(), nullptr, nullptr, nullptr };
	const int out_line[4] = { static_cast<int>(ow * 4), 0, 0, 0 };

	if (const int ret = sws_scale(vpost->sws, in_data, in_line, 0, h, out_data, out_line); ret < 0)
	{
		return { CELL_VPOST_ERROR_E_ARG_CTRL_INVALID, "sws_scale failed (in=%dx%d, out=%dx%d, ret=%d)", w, h, ow, oh, ret };
	}

	std::memcpy(vm::_ptr<u8>(outPicBuff.addr()), vpost->out_buf.data(), layout->out_size);
	return CELL_OK;
}

DECLARE(ppu_module_manager::cellVpost)("cellVpost", []()
{
	REG_FUNC(cellVpost, cellVpostQueryAttr);
	REG_FUNC(cellVpost, cellVpostOpen);
	REG_FUNC(cellVpost, cellVpostOpenEx);
	//REG_FUNC(cellVpost, cellVpostOpenExt); // 0x9f1795df
	REG_FUNC(cellVpost, cellVpostClose);
	REG_FUNC(cellVpost, cellVpostExec);
});
