#include "stdafx.h"
#include "Emu/VFS.h"
#include "Emu/IdManager.h"
#include "Emu/Cell/PPUModule.h"

// STB_IMAGE_IMPLEMENTATION is already defined in stb_image.cpp
#include <stb_image.h>

#include "cellGifDec.h"

LOG_CHANNEL(cellGifDec);

template <>
void fmt_class_string<CellGifDecError>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto error)
	{
		switch (error)
		{
			STR_CASE(CELL_GIFDEC_ERROR_OPEN_FILE);
			STR_CASE(CELL_GIFDEC_ERROR_STREAM_FORMAT);
			STR_CASE(CELL_GIFDEC_ERROR_SEQ);
			STR_CASE(CELL_GIFDEC_ERROR_ARG);
			STR_CASE(CELL_GIFDEC_ERROR_FATAL);
			STR_CASE(CELL_GIFDEC_ERROR_SPU_UNSUPPORT);
			STR_CASE(CELL_GIFDEC_ERROR_SPU_ERROR);
			STR_CASE(CELL_GIFDEC_ERROR_CB_PARAM);
		}

		return unknown;
	});
}

error_code cellGifDecCreate(vm::ptr<GifDecoder> mainHandle, vm::cptr<CellGifDecThreadInParam> threadInParam, vm::ptr<CellGifDecThreadOutParam> threadOutParam)
{
	cellGifDec.todo("cellGifDecCreate(mainHandle=*0x%x, threadInParam=*0x%x, threadOutParam=*0x%x)", mainHandle, threadInParam, threadOutParam);

	if (!mainHandle)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	*mainHandle = {};

	if (!threadOutParam || !threadInParam || !threadInParam->cbCtrlMallocFunc || !threadInParam->cbCtrlFreeFunc ||
		(threadInParam->spuThreadEnable != CELL_GIFDEC_SPU_THREAD_DISABLE &&
			(threadInParam->spuThreadEnable != CELL_GIFDEC_SPU_THREAD_ENABLE ||
				threadInParam->ppuThreadPriority > 3071 ||
				threadInParam->spuThreadPriority > 255)))
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	threadOutParam->gifCodecVersion = 0x240000;

	return CELL_OK;
}

error_code cellGifDecExtCreate(vm::ptr<GifDecoder> mainHandle, vm::cptr<CellGifDecThreadInParam> threadInParam, vm::ptr<CellGifDecThreadOutParam> threadOutParam, vm::cptr<CellGifDecExtThreadInParam> extThreadInParam, vm::ptr<CellGifDecExtThreadOutParam> extThreadOutParam)
{
	cellGifDec.todo("cellGifDecExtCreate(mainHandle=*0x%x, threadInParam=*0x%x, threadOutParam=*0x%x, extThreadInParam=*0x%x, extThreadOutParam=*0x%x)", mainHandle, threadInParam, threadOutParam, extThreadInParam, extThreadOutParam);

	if (!mainHandle)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	*mainHandle = {};

	if (!threadOutParam || !extThreadOutParam || !extThreadInParam || !threadInParam || !threadInParam->cbCtrlMallocFunc || !threadInParam->cbCtrlFreeFunc ||
		(threadInParam->spuThreadEnable != CELL_GIFDEC_SPU_THREAD_DISABLE && threadInParam->spuThreadEnable != CELL_GIFDEC_SPU_THREAD_ENABLE))
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	if (threadInParam->spuThreadEnable == CELL_GIFDEC_SPU_THREAD_ENABLE && !extThreadInParam->spurs)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	if (extThreadInParam->maxContention == 0u || extThreadInParam->maxContention >= 8u)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	for (u32 i = 0; i < 8; i++)
	{
		if (extThreadInParam->priority[i] > 15)
		{
			return CELL_GIFDEC_ERROR_ARG;
		}
	}

	threadOutParam->gifCodecVersion = 0x240000;

	return CELL_OK;
}

namespace gif_dec_internal
{
	gif_stream::gif_stream(utils::serial& ar)
		: image_stream(ar)
	{
		img_dec_internal::serialize_bytes(ar, info);
		img_dec_internal::serialize_bytes(ar, out_param);

		if (!can_read())
		{
			cellGifDec.error("The file of an open GIF stream could not be reopened after loading the savestate (file=%s). Decoding it will fail", file_name);
		}
	}

	void gif_stream::save(utils::serial& ar)
	{
		image_stream::save(ar);
		img_dec_internal::serialize_bytes(ar, info);
		img_dec_internal::serialize_bytes(ar, out_param);
	}

	static error_code open_stream(vm::ptr<u32> subHandle, vm::cptr<CellGifDecSrc> src)
	{
		// Read the source description once
		const CellGifDecSrc source = *src;
		u32 id = 0;

		switch (source.srcSelect)
		{
		case CELL_GIFDEC_BUFFER:
		{
			if (!source.streamPtr || !source.streamSize)
			{
				return CELL_GIFDEC_ERROR_ARG;
			}

			if (source.streamSize > img_dec_internal::MAX_SOURCE_SIZE)
			{
				return { CELL_GIFDEC_ERROR_ARG, "streamSize=0x%x", source.streamSize };
			}

			id = idm::make<gif_stream>(source.streamPtr.addr(), source.streamSize);
			break;
		}
		case CELL_GIFDEC_FILE:
		{
			if (!source.fileName)
			{
				return CELL_GIFDEC_ERROR_OPEN_FILE;
			}

			std::string file_name = source.fileName.get_ptr();
			fs::file file(vfs::get(file_name));

			if (!file)
			{
				return CELL_GIFDEC_ERROR_OPEN_FILE;
			}

			if (source.fileOffset < 0)
			{
				return CELL_GIFDEC_ERROR_ARG;
			}

			const u64 file_size = file.size();

			if (file_size > img_dec_internal::MAX_SOURCE_SIZE)
			{
				return { CELL_GIFDEC_ERROR_ARG, "file=%s, size=0x%llx", file_name, file_size };
			}

			id = idm::make<gif_stream>(std::move(file_name), std::move(file), static_cast<u32>(file_size));
			break;
		}
		default:
		{
			return CELL_GIFDEC_ERROR_ARG;
		}
		}

		if (!id)
		{
			return CELL_GIFDEC_ERROR_FATAL;
		}

		*subHandle = id;
		return CELL_OK;
	}

	static void set_parameter(gif_stream& stream, s32 color_space)
	{
		CellGifDecInfo& current_info = stream.info;
		CellGifDecOutParam& current_outParam = stream.out_param;

		current_outParam.outputWidthByte  = (current_info.SWidth * current_info.SColorResolution * 3) / 8;
		current_outParam.outputWidth      = current_info.SWidth;
		current_outParam.outputHeight     = current_info.SHeight;
		current_outParam.outputColorSpace = color_space;
		current_outParam.outputComponents = 4;
		current_outParam.outputBitDepth = 0;   // Unimplemented
		current_outParam.useMemorySpace = 0;   // Unimplemented
	}
}

using namespace gif_dec_internal;

error_code cellGifDecOpen(vm::ptr<GifDecoder> mainHandle, vm::ptr<u32> subHandle, vm::cptr<CellGifDecSrc> src, vm::ptr<CellGifDecOpnInfo> openInfo)
{
	cellGifDec.warning("cellGifDecOpen(mainHandle=*0x%x, subHandle=*0x%x, src=*0x%x, openInfo=*0x%x)", mainHandle, subHandle, src, openInfo);

	if (!mainHandle || !subHandle)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	if (false) // TODO
	{
		return CELL_GIFDEC_ERROR_SEQ;
	}

	if (!openInfo || !src)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	return open_stream(subHandle, src);
}

error_code cellGifDecExtOpen(vm::ptr<GifDecoder> mainHandle, vm::ptr<u32> subHandle, vm::cptr<CellGifDecSrc> src, vm::ptr<CellGifDecOpnInfo> openInfo, vm::cptr<CellGifDecCbCtrlStrm> cbCtrlStrm)
{
	cellGifDec.todo("cellGifDecExtOpen(mainHandle=*0x%x, subHandle=*0x%x, src=*0x%x, openInfo=*0x%x, cbCtrlStrm=*0x%x)", mainHandle, subHandle, src, openInfo, cbCtrlStrm);

	if (!mainHandle || !subHandle)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	if (false) // TODO
	{
		return CELL_GIFDEC_ERROR_SEQ;
	}

	if (!openInfo || !src)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	// TODO: cbCtrlStrm
	return open_stream(subHandle, src);
}

error_code cellGifDecReadHeader(vm::ptr<GifDecoder> mainHandle, u32 subHandle, vm::ptr<CellGifDecInfo> info)
{
	cellGifDec.warning("cellGifDecReadHeader(mainHandle=*0x%x, subHandle=0x%x, info=*0x%x)", mainHandle, subHandle, info);

	if (!mainHandle)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	if (false) // TODO: check main handle
	{
		return CELL_GIFDEC_ERROR_SEQ;
	}

	if (!subHandle)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	if (!info)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	const auto stream = idm::get_unlocked<gif_stream>(subHandle);

	if (!stream)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	std::lock_guard lock(stream->mutex);

	// Write the header to buffer
	u8 buffer[13];

	if (!stream->read(buffer, sizeof(buffer)))
	{
		return { CELL_GIFDEC_ERROR_STREAM_FORMAT, "The stream is shorter than the header or unreadable (size=0x%x)", stream->stream_size };
	}

	if (read_from_ptr<be_t<u32>>(buffer, 0) != 0x47494638u ||
		(read_from_ptr<le_t<u16>>(buffer, 4) != 0x6139u && read_from_ptr<le_t<u16>>(buffer, 4) != 0x6137u)) // Error: The first 6 bytes are not a valid GIF signature
	{
		return CELL_GIFDEC_ERROR_STREAM_FORMAT; // Surprisingly there is no error code related with headerss
	}

	CellGifDecInfo& current_info = stream->info;

	const u8 packedField = buffer[10];
	current_info.SWidth                  = buffer[6] + buffer[7] * 0x100;
	current_info.SHeight                 = buffer[8] + buffer[9] * 0x100;
	current_info.SGlobalColorTableFlag   = packedField >> 7;
	current_info.SColorResolution        = ((packedField >> 4) & 7)+1;
	current_info.SSortFlag               = (packedField >> 3) & 1;
	current_info.SSizeOfGlobalColorTable = (packedField & 7)+1;
	current_info.SBackGroundColor        = buffer[11];
	current_info.SPixelAspectRatio       = buffer[12];

	*info = current_info;

	return CELL_OK;
}

error_code cellGifDecExtReadHeader(vm::ptr<GifDecoder> mainHandle, u32 subHandle, vm::ptr<CellGifDecInfo> info, vm::ptr<CellGifDecExtInfo> extInfo)
{
	cellGifDec.todo("cellGifDecExtReadHeader(mainHandle=*0x%x, subHandle=0x%x, info=*0x%x, extInfo=*0x%x)", mainHandle, subHandle, info, extInfo);

	if (!mainHandle)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	if (false) // TODO: check main handle
	{
		return CELL_GIFDEC_ERROR_SEQ;
	}

	if (!subHandle)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	if (false) // TODO: check sub handle
	{
		return CELL_GIFDEC_ERROR_SEQ;
	}

	if (!info || !extInfo)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	return CELL_OK;
}

error_code cellGifDecSetParameter(vm::ptr<GifDecoder> mainHandle, u32 subHandle, vm::cptr<CellGifDecInParam> inParam, vm::ptr<CellGifDecOutParam> outParam)
{
	cellGifDec.warning("cellGifDecSetParameter(mainHandle=*0x%x, subHandle=0x%x, inParam=*0x%x, outParam=*0x%x)", mainHandle, subHandle, inParam, outParam);

	if (!mainHandle)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	if (false) // TODO: check main handle
	{
		return CELL_GIFDEC_ERROR_SEQ;
	}

	if (!subHandle)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	if (!inParam || !outParam)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	const auto stream = idm::get_unlocked<gif_stream>(subHandle);

	if (!stream)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	const s32 color_space = inParam->colorSpace;

	switch (color_space)
	{
	case CELL_GIFDEC_RGBA:
	case CELL_GIFDEC_ARGB: break;
	default: return CELL_GIFDEC_ERROR_ARG; // Not supported color space
	}

	std::lock_guard lock(stream->mutex);

	set_parameter(*stream, color_space);

	*outParam = stream->out_param;

	return CELL_OK;
}

error_code cellGifDecExtSetParameter(vm::ptr<GifDecoder> mainHandle, u32 subHandle, vm::cptr<CellGifDecInParam> inParam, vm::ptr<CellGifDecOutParam> outParam, vm::cptr<CellGifDecExtInParam> extInParam, vm::ptr<CellGifDecExtOutParam> extOutParam)
{
	cellGifDec.todo("cellGifDecExtSetParameter(mainHandle=*0x%x, subHandle=0x%x, inParam=*0x%x, outParam=*0x%x, extInParam=*0x%x, extOutParam=*0x%x)", mainHandle, subHandle, inParam, outParam, extInParam, extOutParam);

	if (!mainHandle)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	if (false) // TODO: check main handle
	{
		return CELL_GIFDEC_ERROR_SEQ;
	}

	if (!subHandle)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	if (!inParam || !outParam)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	const auto stream = idm::get_unlocked<gif_stream>(subHandle);

	if (!stream)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	const s32 color_space = inParam->colorSpace;

	switch (color_space)
	{
	case CELL_GIFDEC_RGBA:
	case CELL_GIFDEC_ARGB: break;
	default: return CELL_GIFDEC_ERROR_ARG; // Not supported color space
	}

	{
		std::lock_guard lock(stream->mutex);

		set_parameter(*stream, color_space);

		*outParam = stream->out_param;
	}

	if (!extInParam || extInParam->bufferMode != CELL_GIFDEC_LINE_MODE || !extOutParam)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	return CELL_OK;
}

error_code cellGifDecDecodeData(vm::ptr<GifDecoder> mainHandle, u32 subHandle, vm::ptr<u8> data, vm::cptr<CellGifDecDataCtrlParam> dataCtrlParam, vm::ptr<CellGifDecDataOutInfo> dataOutInfo)
{
	cellGifDec.warning("cellGifDecDecodeData(mainHandle=*0x%x, subHandle=0x%x, data=*0x%x, dataCtrlParam=*0x%x, dataOutInfo=*0x%x)", mainHandle, subHandle, data, dataCtrlParam, dataOutInfo);

	if (!mainHandle)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	if (false) // TODO: check main handle
	{
		return CELL_GIFDEC_ERROR_SEQ;
	}

	if (!subHandle)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	if (!dataOutInfo || !dataCtrlParam)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	const auto stream = idm::get_unlocked<gif_stream>(subHandle);

	if (!stream)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	dataOutInfo->status = CELL_GIFDEC_DEC_STATUS_STOP;

	// Read the guest parameter once
	const u64 bytes_per_line = dataCtrlParam->outputBytesPerLine;

	std::lock_guard lock(stream->mutex);

	// Copy the GIF file to a buffer
	std::vector<u8> gif(stream->stream_size);

	if (!stream->read(gif.data(), stream->stream_size))
	{
		return { CELL_GIFDEC_ERROR_STREAM_FORMAT, "The stream is unreadable (size=0x%x)", stream->stream_size };
	}

	//Decode GIF file. (TODO: Is there any faster alternative? Can we do it without external libraries?)
	int width = 0, height = 0, actual_components = 0;
	auto image = std::unique_ptr<u8, decltype(&::free)>
		(
			stbi_load_from_memory(gif.data(), static_cast<int>(gif.size()), &width, &height, &actual_components, 4),
			&::free
		);

	if (!image)
		return CELL_GIFDEC_ERROR_STREAM_FORMAT;

	const s32 color_space = stream->out_param.outputColorSpace;

	if (color_space != CELL_GIFDEC_RGBA && color_space != CELL_GIFDEC_ARGB)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	const auto layout = img_dec_internal::get_output_layout(bytes_per_line, static_cast<u32>(width), static_cast<u32>(height), 4);

	if (!layout || !vm::check_addr(data.addr(), vm::page_writable, layout->size))
	{
		return { CELL_GIFDEC_ERROR_ARG, "The output doesn't fit (data=*0x%x, outputBytesPerLine=0x%llx, width=%d, height=%d)", data, bytes_per_line, width, height };
	}

	img_dec_internal::write_rows(vm::_ptr<u8>(data.addr()), *layout, image.get(), static_cast<u32>(height), color_space == CELL_GIFDEC_ARGB, false);

	dataOutInfo->status = CELL_GIFDEC_DEC_STATUS_FINISH;
	dataOutInfo->recordType = CELL_GIFDEC_RECORD_TYPE_IMAGE_DESC;

	return CELL_OK;
}

error_code cellGifDecExtDecodeData(vm::ptr<GifDecoder> mainHandle, u32 subHandle, vm::ptr<u8> data, vm::cptr<CellGifDecDataCtrlParam> dataCtrlParam, vm::ptr<CellGifDecDataOutInfo> dataOutInfo, vm::cptr<CellGifDecCbCtrlDisp> cbCtrlDisp, vm::ptr<CellGifDecDispParam> dispParam)
{
	cellGifDec.todo("cellGifDecExtDecodeData(mainHandle=*0x%x, subHandle=0x%x, data=*0x%x, dataCtrlParam=*0x%x, dataOutInfo=*0x%x, cbCtrlDisp=*0x%x, dispParam=*0x%x)", mainHandle, subHandle, data, dataCtrlParam, dataOutInfo, cbCtrlDisp, dispParam);

	if (!mainHandle)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	if (false) // TODO: check main handle
	{
		return CELL_GIFDEC_ERROR_SEQ;
	}

	if (!subHandle)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	if (false) // TODO: check sub handle
	{
		return CELL_GIFDEC_ERROR_SEQ;
	}

	if (!dataOutInfo || !dataCtrlParam)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	return CELL_OK;
}

error_code cellGifDecClose(vm::ptr<GifDecoder> mainHandle, u32 subHandle)
{
	cellGifDec.warning("cellGifDecClose(mainHandle=*0x%x, subHandle=0x%x)", mainHandle, subHandle);

	if (!mainHandle)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	if (false) // TODO: check main handle
	{
		return CELL_GIFDEC_ERROR_SEQ;
	}

	if (!subHandle)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	// The host object owns the file. An operation still running on another thread keeps it alive until it returns.
	if (!idm::remove<gif_stream>(subHandle))
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	return CELL_OK;
}

error_code cellGifDecDestroy(vm::ptr<GifDecoder> mainHandle)
{
	cellGifDec.todo("cellGifDecDestroy(mainHandle=*0x%x)", mainHandle);

	if (!mainHandle)
	{
		return CELL_GIFDEC_ERROR_ARG;
	}

	if (false) // TODO: check main handle
	{
		return CELL_GIFDEC_ERROR_SEQ;
	}

	return CELL_OK;
}

DECLARE(ppu_module_manager::cellGifDec)("cellGifDec", []()
{
	REG_FUNC(cellGifDec, cellGifDecCreate);
	REG_FUNC(cellGifDec, cellGifDecExtCreate);
	REG_FUNC(cellGifDec, cellGifDecOpen);
	REG_FUNC(cellGifDec, cellGifDecReadHeader);
	REG_FUNC(cellGifDec, cellGifDecSetParameter);
	REG_FUNC(cellGifDec, cellGifDecDecodeData);
	REG_FUNC(cellGifDec, cellGifDecClose);
	REG_FUNC(cellGifDec, cellGifDecDestroy);

	REG_FUNC(cellGifDec, cellGifDecExtOpen);
	REG_FUNC(cellGifDec, cellGifDecExtReadHeader);
	REG_FUNC(cellGifDec, cellGifDecExtSetParameter);
	REG_FUNC(cellGifDec, cellGifDecExtDecodeData);
});
