#include "stdafx.h"
#include "Emu/VFS.h"
#include "Emu/IdManager.h"
#include "Emu/Cell/PPUModule.h"

// STB_IMAGE_IMPLEMENTATION is already defined in stb_image.cpp
#include <stb_image.h>

#include "cellJpgDec.h"

LOG_CHANNEL(cellJpgDec);

template <>
void fmt_class_string<CellJpgDecError>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto error)
	{
		switch (error)
		{
			STR_CASE(CELL_JPGDEC_ERROR_HEADER);
			STR_CASE(CELL_JPGDEC_ERROR_STREAM_FORMAT);
			STR_CASE(CELL_JPGDEC_ERROR_ARG);
			STR_CASE(CELL_JPGDEC_ERROR_SEQ);
			STR_CASE(CELL_JPGDEC_ERROR_BUSY);
			STR_CASE(CELL_JPGDEC_ERROR_FATAL);
			STR_CASE(CELL_JPGDEC_ERROR_OPEN_FILE);
			STR_CASE(CELL_JPGDEC_ERROR_SPU_UNSUPPORT);
			STR_CASE(CELL_JPGDEC_ERROR_CB_PARAM);
		}

		return unknown;
	});
}

error_code cellJpgDecCreate(u32 mainHandle, u32 threadInParam, u32 threadOutParam)
{
	cellJpgDec.todo("cellJpgDecCreate(mainHandle=0x%x, threadInParam=0x%x, threadOutParam=0x%x)", mainHandle, threadInParam, threadOutParam);
	return CELL_OK;
}

error_code cellJpgDecExtCreate(u32 mainHandle, u32 threadInParam, u32 threadOutParam, u32 extThreadInParam, u32 extThreadOutParam)
{
	cellJpgDec.todo("cellJpgDecExtCreate(mainHandle=0x%x, threadInParam=0x%x, threadOutParam=0x%x, extThreadInParam=0x%x, extThreadOutParam=0x%x)", mainHandle, threadInParam, threadOutParam, extThreadInParam, extThreadOutParam);
	return CELL_OK;
}

error_code cellJpgDecDestroy(u32 mainHandle)
{
	cellJpgDec.todo("cellJpgDecDestroy(mainHandle=0x%x)", mainHandle);
	return CELL_OK;
}

namespace jpg_dec_internal
{
	jpg_stream::jpg_stream(utils::serial& ar)
		: image_stream(ar)
	{
		img_dec_internal::serialize_bytes(ar, info);
		img_dec_internal::serialize_bytes(ar, out_param);

		if (!can_read())
		{
			cellJpgDec.error("The file of an open JPG stream could not be reopened after loading the savestate (file=%s). Decoding it will fail", file_name);
		}
	}

	void jpg_stream::save(utils::serial& ar)
	{
		image_stream::save(ar);
		img_dec_internal::serialize_bytes(ar, info);
		img_dec_internal::serialize_bytes(ar, out_param);
	}
}

using namespace jpg_dec_internal;

error_code cellJpgDecOpen(u32 mainHandle, vm::ptr<u32> subHandle, vm::ptr<CellJpgDecSrc> src, vm::ptr<CellJpgDecOpnInfo> openInfo)
{
	cellJpgDec.warning("cellJpgDecOpen(mainHandle=0x%x, subHandle=*0x%x, src=*0x%x, openInfo=*0x%x)", mainHandle, subHandle, src, openInfo);

	if (!subHandle || !src)
	{
		return CELL_JPGDEC_ERROR_ARG;
	}

	// Read the source description once
	const CellJpgDecSrc source = *src;
	u32 id = 0;

	switch (source.srcSelect)
	{
	case CELL_JPGDEC_BUFFER:
	{
		if (source.streamSize > img_dec_internal::MAX_SOURCE_SIZE)
		{
			return { CELL_JPGDEC_ERROR_ARG, "streamSize=0x%x", source.streamSize };
		}

		id = idm::make<jpg_stream>(source.streamPtr, source.streamSize);
		break;
	}
	case CELL_JPGDEC_FILE:
	{
		if (!source.fileName)
		{
			return CELL_JPGDEC_ERROR_OPEN_FILE;
		}

		std::string file_name = source.fileName.get_ptr();
		fs::file file(vfs::get(file_name));

		if (!file)
		{
			return CELL_JPGDEC_ERROR_OPEN_FILE;
		}

		const u64 file_size = file.size();

		if (file_size > img_dec_internal::MAX_SOURCE_SIZE)
		{
			return { CELL_JPGDEC_ERROR_ARG, "file=%s, size=0x%llx", file_name, file_size };
		}

		id = idm::make<jpg_stream>(std::move(file_name), std::move(file), static_cast<u32>(file_size));
		break;
	}
	default:
	{
		return { CELL_JPGDEC_ERROR_ARG, "srcSelect=%d", source.srcSelect };
	}
	}

	if (!id)
	{
		return CELL_JPGDEC_ERROR_FATAL;
	}

	*subHandle = id;
	return CELL_OK;
}

error_code cellJpgDecExtOpen()
{
	cellJpgDec.todo("cellJpgDecExtOpen()");
	return CELL_OK;
}

error_code cellJpgDecClose(u32 mainHandle, u32 subHandle)
{
	cellJpgDec.warning("cellJpgDecClose(mainHandle=0x%x, subHandle=0x%x)", mainHandle, subHandle);

	// The host object owns the file. An operation still running on another thread keeps it alive until it returns.
	if (!idm::remove<jpg_stream>(subHandle))
	{
		return CELL_JPGDEC_ERROR_FATAL;
	}

	return CELL_OK;
}

error_code cellJpgDecReadHeader(u32 mainHandle, u32 subHandle, vm::ptr<CellJpgDecInfo> info)
{
	cellJpgDec.trace("cellJpgDecReadHeader(mainHandle=0x%x, subHandle=0x%x, info=*0x%x)", mainHandle, subHandle, info);

	if (!info)
	{
		return CELL_JPGDEC_ERROR_ARG;
	}

	const auto stream = idm::get_unlocked<jpg_stream>(subHandle);

	if (!stream)
	{
		return CELL_JPGDEC_ERROR_FATAL;
	}

	std::lock_guard lock(stream->mutex);

	const u32 fileSize = stream->stream_size;
	CellJpgDecInfo& current_info = stream->info;

	// Write the header to buffer
	std::vector<u8> buffer(fileSize);

	if (!stream->read(buffer.data(), fileSize))
	{
		return { CELL_JPGDEC_ERROR_HEADER, "The stream is unreadable (size=0x%x)", fileSize };
	}

	if (fileSize < 10 ||                                   // Error: Too short for the SOI and APP0 markers
		read_from_ptr<le_t<u32>>(buffer, 0) != 0xE0FFD8FF || // Error: Not a valid SOI header
		read_from_ptr<u32>(buffer, 6) != "JFIF"_u32)        // Error: Not a valid JFIF string
	{
		return CELL_JPGDEC_ERROR_HEADER;
	}

	u32 i = 4;

	u16 block_length = buffer[i] * 0xFF + buffer[i + 1];

	while (true)
	{
		i += block_length;                                  // Increase the file index to get to the next block
		if (u64{i} + 1 >= fileSize ||                       // Check to protect against segmentation faults
			buffer[i] != 0xFF)                              // Check that we are truly at the start of another block
		{
			return CELL_JPGDEC_ERROR_HEADER;
		}

		const u8 next = buffer[i + 1];
		if (next == 0xC0)
			break;                                          // 0xFFC0 is the "Start of frame" marker which contains the file size

		i += 2;                                             // Skip the block marker
		if (i >= fileSize)
		{
			return CELL_JPGDEC_ERROR_HEADER;
		}

		block_length = buffer[i] * 0xFF + next;             // Go to the next block
	}

	// The start of frame segment holds the height and the width at offsets 5 to 8
	if (u64{i} + 8 >= fileSize)
	{
		return CELL_JPGDEC_ERROR_HEADER;
	}

	current_info.imageWidth    = buffer[i + 7] * 0x100 + buffer[i + 8];
	current_info.imageHeight   = buffer[i + 5] * 0x100 + buffer[i + 6];
	current_info.numComponents = 3; // Unimplemented
	current_info.colorSpace    = CELL_JPG_RGB;

	*info = current_info;

	return CELL_OK;
}

error_code cellJpgDecExtReadHeader()
{
	cellJpgDec.todo("cellJpgDecExtReadHeader()");
	return CELL_OK;
}

error_code cellJpgDecDecodeData(u32 mainHandle, u32 subHandle, vm::ptr<u8> data, vm::cptr<CellJpgDecDataCtrlParam> dataCtrlParam, vm::ptr<CellJpgDecDataOutInfo> dataOutInfo)
{
	cellJpgDec.trace("cellJpgDecDecodeData(mainHandle=0x%x, subHandle=0x%x, data=*0x%x, dataCtrlParam=*0x%x, dataOutInfo=*0x%x)", mainHandle, subHandle, data, dataCtrlParam, dataOutInfo);

	if (!dataOutInfo || !dataCtrlParam)
	{
		return CELL_JPGDEC_ERROR_ARG;
	}

	dataOutInfo->status = CELL_JPGDEC_DEC_STATUS_STOP;

	const auto stream = idm::get_unlocked<jpg_stream>(subHandle);

	if (!stream)
	{
		return CELL_JPGDEC_ERROR_FATAL;
	}

	// Read the guest parameter once
	const u64 bytes_per_line = dataCtrlParam->outputBytesPerLine;

	std::lock_guard lock(stream->mutex);

	const CellJpgDecOutParam& current_outParam = stream->out_param;
	const u32 color_space = current_outParam.outputColorSpace;

	//Copy the JPG file to a buffer
	std::vector<u8> jpg(stream->stream_size);

	if (!stream->read(jpg.data(), stream->stream_size))
	{
		return { CELL_JPGDEC_ERROR_STREAM_FORMAT, "The stream is unreadable (size=0x%x)", stream->stream_size };
	}

	// RGB is decoded with 3 components, so that its rows are packed
	const int nComponents = color_space == CELL_JPG_RGB ? 3 : 4;

	//Decode JPG file. (TODO: Is there any faster alternative? Can we do it without external libraries?)
	int width = 0, height = 0, actual_components = 0;
	auto image = std::unique_ptr<unsigned char,decltype(&::free)>
		(
			stbi_load_from_memory(jpg.data(), static_cast<int>(jpg.size()), &width, &height, &actual_components, nComponents),
			&::free
		);

	if (!image)
		return CELL_JPGDEC_ERROR_STREAM_FORMAT;

	const bool flip = current_outParam.outputMode == CELL_JPGDEC_BOTTOM_TO_TOP;
	u64 image_size = u64{static_cast<u32>(width)} * static_cast<u32>(height);

	switch (color_space)
	{
	case CELL_JPG_RGB:
	case CELL_JPG_RGBA:
	case CELL_JPG_ARGB:
	{
		image_size *= nComponents;

		const auto layout = img_dec_internal::get_output_layout(bytes_per_line, static_cast<u32>(width), static_cast<u32>(height), static_cast<u32>(nComponents));

		if (!layout || !vm::check_addr(data.addr(), vm::page_writable, layout->size))
		{
			return { CELL_JPGDEC_ERROR_ARG, "The output doesn't fit (data=*0x%x, outputBytesPerLine=0x%llx, width=%d, height=%d)", data, bytes_per_line, width, height };
		}

		img_dec_internal::write_rows(vm::_ptr<u8>(data.addr()), *layout, image.get(), static_cast<u32>(height), color_space == CELL_JPG_ARGB, flip);
		break;
	}
	case CELL_JPG_GRAYSCALE:
	case CELL_JPG_YCbCr:
	case CELL_JPG_UPSAMPLE_ONLY:
	case CELL_JPG_GRAYSCALE_TO_ALPHA_RGBA:
	case CELL_JPG_GRAYSCALE_TO_ALPHA_ARGB:
		cellJpgDec.error("cellJpgDecDecodeData: Unsupported color space (%d)", color_space);
		break;

	default:
		return CELL_JPGDEC_ERROR_ARG;
	}

	dataOutInfo->status = CELL_JPGDEC_DEC_STATUS_FINISH;

	if (bytes_per_line)
		dataOutInfo->outputLines = static_cast<u32>(image_size / bytes_per_line);

	return CELL_OK;
}

error_code cellJpgDecExtDecodeData()
{
	cellJpgDec.todo("cellJpgDecExtDecodeData()");
	return CELL_OK;
}

error_code cellJpgDecSetParameter(u32 mainHandle, u32 subHandle, vm::cptr<CellJpgDecInParam> inParam, vm::ptr<CellJpgDecOutParam> outParam)
{
	cellJpgDec.trace("cellJpgDecSetParameter(mainHandle=0x%x, subHandle=0x%x, inParam=*0x%x, outParam=*0x%x)", mainHandle, subHandle, inParam, outParam);

	if (!inParam || !outParam)
	{
		return CELL_JPGDEC_ERROR_ARG;
	}

	const auto stream = idm::get_unlocked<jpg_stream>(subHandle);

	if (!stream)
	{
		return CELL_JPGDEC_ERROR_FATAL;
	}

	// Read the guest parameters once
	const CellJpgDecInParam in = *inParam;

	std::lock_guard lock(stream->mutex);

	const CellJpgDecInfo& current_info = stream->info;
	CellJpgDecOutParam current_outParam = stream->out_param;

	current_outParam.outputWidthByte  = (current_info.imageWidth * current_info.numComponents);
	current_outParam.outputWidth      = current_info.imageWidth;
	current_outParam.outputHeight     = current_info.imageHeight;
	current_outParam.outputColorSpace = in.outputColorSpace;

	switch (current_outParam.outputColorSpace)
	{
	case CELL_JPG_GRAYSCALE:               current_outParam.outputComponents = 1; break;

	case CELL_JPG_RGB:
	case CELL_JPG_YCbCr:                   current_outParam.outputComponents = 3; break;

	case CELL_JPG_UPSAMPLE_ONLY:           current_outParam.outputComponents = current_info.numComponents; break;

	case CELL_JPG_RGBA:
	case CELL_JPG_ARGB:
	case CELL_JPG_GRAYSCALE_TO_ALPHA_RGBA:
	case CELL_JPG_GRAYSCALE_TO_ALPHA_ARGB: current_outParam.outputComponents = 4; break;

	default: return CELL_JPGDEC_ERROR_ARG; // Not supported color space
	}

	current_outParam.outputMode     = in.outputMode;
	current_outParam.downScale      = in.downScale;
	current_outParam.useMemorySpace = 0; // Unimplemented

	stream->out_param = current_outParam;
	*outParam = current_outParam;

	return CELL_OK;
}

error_code cellJpgDecExtSetParameter()
{
	cellJpgDec.todo("cellJpgDecExtSetParameter()");
	return CELL_OK;
}


DECLARE(ppu_module_manager::cellJpgDec)("cellJpgDec", []()
{
	REG_FUNC(cellJpgDec, cellJpgDecCreate);
	REG_FUNC(cellJpgDec, cellJpgDecExtCreate);
	REG_FUNC(cellJpgDec, cellJpgDecOpen);
	REG_FUNC(cellJpgDec, cellJpgDecReadHeader);
	REG_FUNC(cellJpgDec, cellJpgDecSetParameter);
	REG_FUNC(cellJpgDec, cellJpgDecDecodeData);
	REG_FUNC(cellJpgDec, cellJpgDecClose);
	REG_FUNC(cellJpgDec, cellJpgDecDestroy);

	REG_FUNC(cellJpgDec, cellJpgDecExtOpen);
	REG_FUNC(cellJpgDec, cellJpgDecExtReadHeader);
	REG_FUNC(cellJpgDec, cellJpgDecExtSetParameter);
	REG_FUNC(cellJpgDec, cellJpgDecExtDecodeData);
});
