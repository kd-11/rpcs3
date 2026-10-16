#pragma once

#include "Emu/Memory/vm_ptr.h"

#include "png.h"

#include <optional>

enum : u32
{
	PNGDEC_CODEC_VERSION = 0x00420000,
};

// Return Codes
enum CellPngDecError
{
	CELL_PNGDEC_ERROR_HEADER        = 0x80611201,
	CELL_PNGDEC_ERROR_STREAM_FORMAT = 0x80611202,
	CELL_PNGDEC_ERROR_ARG           = 0x80611203,
	CELL_PNGDEC_ERROR_SEQ           = 0x80611204,
	CELL_PNGDEC_ERROR_BUSY          = 0x80611205,
	CELL_PNGDEC_ERROR_FATAL         = 0x80611206,
	CELL_PNGDEC_ERROR_OPEN_FILE     = 0x80611207,
	CELL_PNGDEC_ERROR_SPU_UNSUPPORT = 0x80611208,
	CELL_PNGDEC_ERROR_SPU_ERROR     = 0x80611209,
	CELL_PNGDEC_ERROR_CB_PARAM      = 0x8061120a,
};

// Consts
enum CellPngDecColorSpace : s32
{
	CELL_PNGDEC_GRAYSCALE       = 1,
	CELL_PNGDEC_RGB             = 2,
	CELL_PNGDEC_PALETTE         = 4,
	CELL_PNGDEC_GRAYSCALE_ALPHA = 9,
	CELL_PNGDEC_RGBA            = 10,
	CELL_PNGDEC_ARGB            = 20,
};

enum CellPngDecSpuThreadEna : s32
{
	CELL_PNGDEC_SPU_THREAD_DISABLE = 0,
	CELL_PNGDEC_SPU_THREAD_ENABLE  = 1,
};

enum CellPngDecStreamSrcSel : s32
{
	CELL_PNGDEC_FILE   = 0,
	CELL_PNGDEC_BUFFER = 1,
};

enum CellPngDecInterlaceMode : s32
{
	CELL_PNGDEC_NO_INTERLACE = 0,
	CELL_PNGDEC_ADAM7_INTERLACE = 1,
};

enum CellPngDecOutputMode : s32
{
	CELL_PNGDEC_TOP_TO_BOTTOM = 0,
	CELL_PNGDEC_BOTTOM_TO_TOP = 1,
};

enum CellPngDecPackFlag : s32
{
	CELL_PNGDEC_1BYTE_PER_NPIXEL = 0,
	CELL_PNGDEC_1BYTE_PER_1PIXEL = 1,
};

enum CellPngDecAlphaSelect : s32
{
	CELL_PNGDEC_STREAM_ALPHA = 0,
	CELL_PNGDEC_FIX_ALPHA    = 1,
};

enum CellPngDecCommand : s32
{
	CELL_PNGDEC_CONTINUE = 0,
	CELL_PNGDEC_STOP     = 1,
};

enum CellPngDecDecodeStatus : s32
{
	CELL_PNGDEC_DEC_STATUS_FINISH = 0,
	CELL_PNGDEC_DEC_STATUS_STOP   = 1,
};

enum CellPngDecBufferMode : s32
{
	CELL_PNGDEC_LINE_MODE = 1,
};

enum CellPngDecSpuMode : s32
{
	CELL_PNGDEC_RECEIVE_EVENT    = 0,
	CELL_PNGDEC_TRYRECEIVE_EVENT = 1,
};

// Callbacks for memory management
using CellPngDecCbControlMalloc = vm::ptr<void>(u32 size, vm::ptr<void> cbCtrlMallocArg);
using CellPngDecCbControlFree   = s32(vm::ptr<void> ptr, vm::ptr<void> cbCtrlFreeArg);

// Structs
struct CellPngDecThreadInParam
{
	be_t<s32> spuThreadEnable; // CellPngDecSpuThreadEna
	be_t<u32> ppuThreadPriority;
	be_t<u32> spuThreadPriority;
	vm::bptr<CellPngDecCbControlMalloc> cbCtrlMallocFunc;
	vm::bptr<void> cbCtrlMallocArg;
	vm::bptr<CellPngDecCbControlFree> cbCtrlFreeFunc;
	vm::bptr<void> cbCtrlFreeArg;
};

struct CellPngDecExtThreadInParam
{
	vm::bptr<struct CellSpurs> spurs;
	u8 priority[8];
	be_t<u32> maxContention;
};

struct CellPngDecThreadOutParam
{
	be_t<u32> pngCodecVersion;
};

struct CellPngDecExtThreadOutParam
{
	be_t<u32> reserved;
};

struct CellPngDecSrc
{
	be_t<s32> srcSelect; // CellPngDecStreamSrcSel
	vm::bcptr<char> fileName;
	be_t<s64> fileOffset;
	be_t<u32> fileSize;
	vm::bptr<void> streamPtr;
	be_t<u32> streamSize;
	be_t<s32> spuThreadEnable; // CellGifDecSpuThreadEna
};

struct CellPngDecOpnInfo
{
	be_t<u32> initSpaceAllocated;
};

struct CellPngDecInfo
{
	be_t<u32> imageWidth;
	be_t<u32> imageHeight;
	be_t<u32> numComponents;
	be_t<s32> colorSpace; // CellPngDecColorSpace
	be_t<u32> bitDepth;
	be_t<s32> interlaceMethod; // CellPngDecInterlaceMode
	be_t<u32> chunkInformation;
};

struct CellPngDecInParam
{
	vm::bptr<volatile s32> commandPtr; // CellPngDecCommand
	be_t<s32> outputMode; // CellPngDecOutputMode
	be_t<s32> outputColorSpace; // CellPngDecColorSpace
	be_t<u32> outputBitDepth;
	be_t<s32> outputPackFlag; // CellPngDecPackFlag
	be_t<s32> outputAlphaSelect; // CellPngDecAlphaSelect
	be_t<u32> outputColorAlpha;
};

struct CellPngDecOutParam
{
	be_t<u64> outputWidthByte;
	be_t<u32> outputWidth;
	be_t<u32> outputHeight;
	be_t<u32> outputComponents;
	be_t<u32> outputBitDepth;
	be_t<s32> outputMode; // CellPngDecOutputMode
	be_t<s32> outputColorSpace; // CellPngDecOutputMode
	be_t<u32> useMemorySpace;
};

struct CellPngDecDataCtrlParam
{
	be_t<u64> outputBytesPerLine;
};

struct CellPngDecDataOutInfo
{
	be_t<u32> chunkInformation;
	be_t<u32> numText;
	be_t<u32> numUnknownChunk;
	be_t<s32> status; // CellPngDecDecodeStatus
};

// Structs for decoding partial streams
struct CellPngDecStrmInfo
{
	be_t<u32> decodedStrmSize;
};

struct CellPngDecStrmParam
{
	vm::bptr<void> strmPtr;
	be_t<u32> strmSize;
};

struct CellPngDecDispInfo
{
	be_t<u64> outputFrameWidthByte;
	be_t<u32> outputFrameHeight;
	be_t<u64> outputStartXByte;
	be_t<u32> outputStartY;
	be_t<u64> outputWidthByte;
	be_t<u32> outputHeight;
	be_t<u32> outputBitDepth;
	be_t<u32> outputComponents;
	be_t<u32> nextOutputStartY;
	be_t<u32> scanPassCount;
	vm::bptr<void> outputImage;
};

struct CellPngDecDispParam
{
	vm::bptr<void> nextOutputImage;
};

struct CellPngDecOpnParam
{
	be_t<u32> selectChunk;
};

struct CellPngDecExtInfo
{
	be_t<u64> reserved;
};

struct CellPngDecExtInParam
{
	be_t<s32> bufferMode; // CellPngDecBufferMode
	be_t<u32> outputCounts;
	be_t<s32> spuMode; // CellPngDecSpuMode
};

struct CellPngDecExtOutParam
{
	be_t<u64> outputWidthByte;
	be_t<u32> outputHeight;
};

// Callbacks for decoding partial streams
using CellPngDecCbControlStream = s32(vm::ptr<CellPngDecStrmInfo> strmInfo, vm::ptr<CellPngDecStrmParam> strmParam, vm::ptr<void> cbCtrlStrmArg);
using CellPngDecCbControlDisp   = s32(vm::ptr<CellPngDecDispInfo> dispInfo, vm::ptr<CellPngDecDispParam> dispParam, vm::ptr<void> cbCtrlDispArg);

struct CellPngDecCbCtrlStrm
{
	vm::bptr<CellPngDecCbControlStream> cbCtrlStrmFunc;
	vm::bptr<void> cbCtrlStrmArg;
};

struct CellPngDecCbCtrlDisp
{
	vm::bptr<CellPngDecCbControlDisp> cbCtrlDispFunc;
	vm::bptr<void> cbCtrlDispArg;
};

// Custom structs

// Main handle, allocated with the user's malloc callback. The guest can modify it, the decoder only uses PngDecMainHost
struct PngHandle
{
	vm::bptr<CellPngDecCbControlMalloc> malloc_;
	vm::bptr<void> malloc_arg;
	vm::bptr<CellPngDecCbControlFree> free_;
	vm::bptr<void> free_arg;
	be_t<u32> host_id; // ID of the PngDecMainHost. Only used to look up the host object, which is then verified
};

// Sub handle, allocated with the user's malloc callback. The decoder state is located in PngDecStreamHost
struct PngStream
{
	be_t<u32> host_id; // ID of the PngDecStreamHost. Only used to look up the host object, which is then verified
	u8 reserved[0x7c];
};

// Second allocation of a sub handle, unused. It used to contain the state of the input buffer
struct PngBuffer
{
	u8 reserved[0x20];
};

class ppu_thread;

// HLE exclusive, state of a main handle
class PngDecMainHost
{
public:
	static constexpr u32 id_base = 1;
	static constexpr u32 id_step = 1;
	static constexpr u32 id_count = 1023;
	SAVESTATE_INIT_POS(46);

	PngDecMainHost(u32 handle_addr, vm::ptr<CellPngDecCbControlMalloc> malloc_func, vm::ptr<void> malloc_arg, vm::ptr<CellPngDecCbControlFree> free_func, vm::ptr<void> free_arg)
		: handle_addr(handle_addr), malloc_func(malloc_func), malloc_arg(malloc_arg), free_func(free_func), free_arg(free_arg)
	{
	}

	PngDecMainHost(utils::serial& ar);
	void save(utils::serial& ar);

	const u32 handle_addr; // Address of the PngHandle

	// The callbacks set when the handle was created
	const vm::ptr<CellPngDecCbControlMalloc> malloc_func;
	const vm::ptr<void> malloc_arg;
	const vm::ptr<CellPngDecCbControlFree> free_func;
	const vm::ptr<void> free_arg;
};

// HLE exclusive, state of a sub handle. libpng only accesses this object, never guest memory directly
class PngDecStreamHost
{
public:
	static constexpr u32 id_base = 1;
	static constexpr u32 id_step = 1;
	static constexpr u32 id_count = 1023;
	SAVESTATE_INIT_POS(47);

	PngDecStreamHost(u32 handle_addr, u32 stream_addr, const PngDecMainHost& main)
		: handle_addr(handle_addr), stream_addr(stream_addr), malloc_func(main.malloc_func), malloc_arg(main.malloc_arg), free_func(main.free_func), free_arg(main.free_arg)
	{
	}

	PngDecStreamHost(utils::serial& ar);
	PngDecStreamHost(const PngDecStreamHost&) = delete;
	PngDecStreamHost& operator=(const PngDecStreamHost&) = delete;
	~PngDecStreamHost();

	void save(utils::serial& ar);

	// Bookkeeping, saved in savestates
	const u32 handle_addr; // Address of the PngHandle the stream belongs to
	const u32 stream_addr; // Address of the PngStream
	u32 buffer_addr = 0;   // Address of the PngBuffer
	const vm::ptr<CellPngDecCbControlMalloc> malloc_func;
	const vm::ptr<void> malloc_arg;
	const vm::ptr<CellPngDecCbControlFree> free_func;
	const vm::ptr<void> free_arg;
	bool file = false;
	u32 fd = 0;

	// The state of libpng can't be saved. Streams that were open when a savestate was created can only be closed
	bool lost = false;

	// Set while libpng is used. The guest callbacks can call the decoder functions again, which is not allowed
	atomic_t<bool> busy = false;

	CellPngDecInfo info{};
	CellPngDecOutParam out_param{};
	CellPngDecSrc source{};

	// Partial decoding
	CellPngDecCbCtrlStrm cb_ctrl_stream{};
	CellPngDecCbCtrlDisp cb_ctrl_disp{};
	vm::ptr<CellPngDecDispInfo> disp_info{};
	vm::ptr<CellPngDecDispParam> disp_param{};
	ppu_thread* ppu = nullptr; // Only set while decoding
	bool progressive = false;
	u32 output_counts = 0;
	u32 next_row = 0;
	bool end_of_file = false;

	s32 packing = 0;
	u32 passes = 0;
	bool parameters_set = false; // The output parameters were set, the row buffer is allocated

	// Input buffer for decoding from memory
	u32 data_addr = 0;
	u32 length = 0;
	u32 cursor = 0;
	u32 push_size = 0;           // Size of the data pushed to libpng in progressive mode
	bool pushing_source = false; // The data pushed to libpng is located at data_addr + cursor

	// libpng never writes to guest memory directly, rows are decoded here and then copied
	std::vector<u8> row_buffer;

	png_structp png_ptr = nullptr;
	png_infop info_ptr = nullptr;
};

// Validation of guest-supplied sizes and addresses. These functions don't access guest memory
namespace png_dec_internal
{
	// Size of the output buffer needed for an image with these parameters, or std::nullopt if it doesn't fit in the guest address space
	constexpr std::optional<u32> get_output_size(u32 height, u64 bytes_per_line, u64 row_bytes)
	{
		if (height == 0)
		{
			return 0;
		}

		if (row_bytes > bytes_per_line || bytes_per_line > u32{umax})
		{
			return std::nullopt;
		}

		const u64 size = u64{height - 1} * bytes_per_line + row_bytes;

		if (size > u32{umax})
		{
			return std::nullopt;
		}

		return static_cast<u32>(size);
	}

	// Address of a row written by the progressive decoder, or std::nullopt if the output parameters set by the guest are invalid
	constexpr std::optional<u32> get_progressive_row_addr(u32 next_output_image, u32 row_num, u32 output_start_y, u64 frame_width_byte, u32 pass, u64 row_bytes)
	{
		u64 addr = next_output_image;

		if (pass == 0)
		{
			if (row_num < output_start_y || frame_width_byte > u32{umax})
			{
				return std::nullopt;
			}

			addr += u64{row_num - output_start_y} * frame_width_byte;
		}

		if (addr + row_bytes > u64{u32{umax}} + 1)
		{
			return std::nullopt;
		}

		return static_cast<u32>(addr);
	}

	// Whether size bytes can be read from an input buffer of the given length at the cursor
	constexpr bool can_read(u32 cursor, u32 length, u64 size)
	{
		return cursor <= length && size <= u64{length - cursor};
	}
}

// Converts libpng colour type to cellPngDec colour type
static s32 getPngDecColourType(u8 type)
{
	switch (type)
	{
	case PNG_COLOR_TYPE_RGB:        return CELL_PNGDEC_RGB;
	case PNG_COLOR_TYPE_RGBA:       return CELL_PNGDEC_RGBA; // We can't diffrentiate between ARGB and RGBA. Doesn't seem to be exactly important.
	case PNG_COLOR_TYPE_PALETTE:    return CELL_PNGDEC_PALETTE;
	case PNG_COLOR_TYPE_GRAY:       return CELL_PNGDEC_GRAYSCALE;
	case PNG_COLOR_TYPE_GRAY_ALPHA: return CELL_PNGDEC_GRAYSCALE_ALPHA;
	default: fmt::throw_exception("Unknown colour type: %d", type);
	}
}

static bool cellPngColorSpaceHasAlpha(u32 colorspace)
{
	switch (colorspace)
	{
	case CELL_PNGDEC_RGBA:
	case CELL_PNGDEC_ARGB:
	case CELL_PNGDEC_GRAYSCALE_ALPHA:
		return true;
	default:
		return false;
	}
}

// Custom exception for libPng errors

class LibPngCustomException : public std::runtime_error
{
public:
	LibPngCustomException(char const* const message) : runtime_error(message) {}
};
