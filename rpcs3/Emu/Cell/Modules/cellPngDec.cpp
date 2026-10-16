#include "stdafx.h"
#include "Emu/VFS.h"
#include "Emu/IdManager.h"
#include "Emu/Cell/PPUModule.h"
#include "Emu/Cell/lv2/sys_fs.h"
#include "Emu/savestate_utils.hpp"
#include "png.h"
#include "cellPng.h"
#include "cellPngDec.h"

#if PNG_LIBPNG_VER_MAJOR >= 1 && (PNG_LIBPNG_VER_MINOR < 5 \
|| (PNG_LIBPNG_VER_MINOR == 5 && PNG_LIBPNG_VER_RELEASE < 7))
#define PNG_ERROR_ACTION_NONE 1
#define PNG_RGB_TO_GRAY_DEFAULT (-1)
#endif

#if PNG_LIBPNG_VER_MAJOR >= 1 && PNG_LIBPNG_VER_MINOR >= 5
typedef png_bytep iCCP_profile_type;
#else
typedef png_charp iCCP_profile_type;
#endif

LOG_CHANNEL(cellPngDec);

template <>
void fmt_class_string<CellPngDecError>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](CellPngDecError value)
	{
		switch (value)
		{
		STR_CASE(CELL_PNGDEC_ERROR_HEADER);
		STR_CASE(CELL_PNGDEC_ERROR_STREAM_FORMAT);
		STR_CASE(CELL_PNGDEC_ERROR_ARG);
		STR_CASE(CELL_PNGDEC_ERROR_SEQ);
		STR_CASE(CELL_PNGDEC_ERROR_BUSY);
		STR_CASE(CELL_PNGDEC_ERROR_FATAL);
		STR_CASE(CELL_PNGDEC_ERROR_OPEN_FILE);
		STR_CASE(CELL_PNGDEC_ERROR_SPU_UNSUPPORT);
		STR_CASE(CELL_PNGDEC_ERROR_SPU_ERROR);
		STR_CASE(CELL_PNGDEC_ERROR_CB_PARAM);
		}

		return unknown;
	});
}

// cellPngDec aliases to improve readability
using PPHandle           = vm::pptr<PngHandle>;
using PHandle            = vm::ptr<PngHandle>;
using PThreadInParam     = vm::cptr<CellPngDecThreadInParam>;
using PThreadOutParam    = vm::ptr<CellPngDecThreadOutParam>;
using PExtThreadInParam  = vm::cptr<CellPngDecExtThreadInParam>;
using PExtThreadOutParam = vm::ptr<CellPngDecExtThreadOutParam>;
using PPStream           = vm::pptr<PngStream>;
using PStream            = vm::ptr<PngStream>;
using PSrc               = vm::cptr<CellPngDecSrc>;
using POpenInfo          = vm::ptr<CellPngDecOpnInfo>;
using POpenParam         = vm::cptr<CellPngDecOpnParam>;
using PInfo              = vm::ptr<CellPngDecInfo>;
using PExtInfo           = vm::ptr<CellPngDecExtInfo>;
using PInParam           = vm::cptr<CellPngDecInParam>;
using POutParam          = vm::ptr<CellPngDecOutParam>;
using PExtInParam        = vm::cptr<CellPngDecExtInParam>;
using PExtOutParam       = vm::ptr<CellPngDecExtOutParam>;
using PDataControlParam  = vm::cptr<CellPngDecDataCtrlParam>;
using PDataOutInfo       = vm::ptr<CellPngDecDataOutInfo>;
using PCbControlDisp     = vm::cptr<CellPngDecCbCtrlDisp>;
using PCbControlStream   = vm::cptr<CellPngDecCbCtrlStrm>;
using PDispParam         = vm::ptr<CellPngDecDispParam>;

PngDecMainHost::PngDecMainHost(utils::serial& ar)
	: handle_addr(ar.pop<u32>())
	, malloc_func(vm::ptr<CellPngDecCbControlMalloc>::make(ar.pop<u32>()))
	, malloc_arg(vm::ptr<void>::make(ar.pop<u32>()))
	, free_func(vm::ptr<CellPngDecCbControlFree>::make(ar.pop<u32>()))
	, free_arg(vm::ptr<void>::make(ar.pop<u32>()))
{
}

void PngDecMainHost::save(utils::serial& ar)
{
	USING_SERIALIZATION_VERSION(cellPngDec);
	ar(handle_addr, malloc_func.addr(), malloc_arg.addr(), free_func.addr(), free_arg.addr());
}

PngDecStreamHost::PngDecStreamHost(utils::serial& ar)
	: handle_addr(ar.pop<u32>())
	, stream_addr(ar.pop<u32>())
	, buffer_addr(ar.pop<u32>())
	, malloc_func(vm::ptr<CellPngDecCbControlMalloc>::make(ar.pop<u32>()))
	, malloc_arg(vm::ptr<void>::make(ar.pop<u32>()))
	, free_func(vm::ptr<CellPngDecCbControlFree>::make(ar.pop<u32>()))
	, free_arg(vm::ptr<void>::make(ar.pop<u32>()))
	, file(ar.pop<bool>())
	, fd(ar.pop<u32>())
	, lost(true)
{
	cellPngDec.error("A PNG stream was open when the savestate was created (stream=*0x%x). The decoder state can't be restored, it can only be closed", stream_addr);
}

PngDecStreamHost::~PngDecStreamHost()
{
	if (png_ptr)
	{
		png_destroy_read_struct(&png_ptr, info_ptr ? &info_ptr : nullptr, nullptr);
	}
}

void PngDecStreamHost::save(utils::serial& ar)
{
	USING_SERIALIZATION_VERSION(cellPngDec);
	ar(handle_addr, stream_addr, buffer_addr, malloc_func.addr(), malloc_arg.addr(), free_func.addr(), free_arg.addr(), file, fd);
}

namespace png_dec_internal
{
	// Calls a function using libpng. libpng errors jump back here, so func must not have any objects with destructors alive while it calls libpng
	template <typename F>
	static bool png_try(png_structp png_ptr, F&& func)
	{
		if (setjmp(png_jmpbuf(png_ptr)))
		{
			return false;
		}

		func();
		return true;
	}

	// Prevents several operations from using the same stream at the same time. The guest callbacks can call the decoder functions again
	class busy_lock
	{
		PngDecStreamHost& m_stream;
		const bool m_locked;

	public:
		explicit busy_lock(PngDecStreamHost& stream) : m_stream(stream), m_locked(!stream.busy.exchange(true)) {}
		busy_lock(const busy_lock&) = delete;
		busy_lock& operator=(const busy_lock&) = delete;
		~busy_lock() { if (m_locked) m_stream.busy = false; }
		explicit operator bool() const { return m_locked; }
	};

	// Returns the host object of a main handle. The ID is located in guest memory, so it is also checked that the object belongs to the handle
	static std::pair<u32, stx::shared_ptr<PngDecMainHost>> get_main(PHandle handle)
	{
		if (!handle || !vm::check_addr(handle.addr(), vm::page_readable, sizeof(PngHandle)))
		{
			return {};
		}

		const u32 id = handle->host_id;

		if (auto main = idm::get_unlocked<PngDecMainHost>(id); main && main->handle_addr == handle.addr())
		{
			return { id, std::move(main) };
		}

		// The guest overwrote the ID
		u32 found_id = 0;

		auto found = idm::select<PngDecMainHost>([&](u32 id, PngDecMainHost& main) -> bool
		{
			if (main.handle_addr != handle.addr())
			{
				return false;
			}

			found_id = id;
			return true;
		});

		return { found_id, std::move(found.ptr) };
	}

	// Returns the host object of a sub handle, which has to belong to the main handle
	static std::pair<u32, stx::shared_ptr<PngDecStreamHost>> get_stream(PHandle handle, PStream stream)
	{
		if (!handle || !stream || !vm::check_addr(stream.addr(), vm::page_readable, sizeof(PngStream)))
		{
			return {};
		}

		const u32 id = stream->host_id;

		if (auto host = idm::get_unlocked<PngDecStreamHost>(id); host && host->stream_addr == stream.addr() && host->handle_addr == handle.addr())
		{
			return { id, std::move(host) };
		}

		// The guest overwrote the ID
		u32 found_id = 0;

		auto found = idm::select<PngDecStreamHost>([&](u32 id, PngDecStreamHost& host) -> bool
		{
			if (host.stream_addr != stream.addr() || host.handle_addr != handle.addr())
			{
				return false;
			}

			found_id = id;
			return true;
		});

		return { found_id, std::move(found.ptr) };
	}

	// Allocates guest memory with the user's malloc callback, returns 0 if it failed or the memory isn't usable
	static u32 guest_malloc(ppu_thread& ppu, vm::ptr<CellPngDecCbControlMalloc> malloc_func, vm::ptr<void> malloc_arg, u32 size)
	{
		const u32 addr = malloc_func(ppu, size, malloc_arg).addr();

		if (!addr || !vm::check_addr(addr, vm::page_readable | vm::page_writable, size))
		{
			cellPngDec.error("The malloc callback returned an invalid address (addr=0x%x, size=0x%x)", addr, size);
			return 0;
		}

		return addr;
	}

	// Reads the input for libpng, returns false if not enough data is available
	static bool read_input(PngDecStreamHost& stream, u8* out, usz length)
	{
		if (stream.file)
		{
			const auto file = idm::get_unlocked<lv2_fs_object, lv2_file>(stream.fd);
			return file && file->file.read(out, length) == length;
		}

		if (!can_read(stream.cursor, stream.length, length))
		{
			return false;
		}

		// The buffer range was checked when the stream was opened
		std::memcpy(out, vm::base(stream.data_addr + stream.cursor), length);
		stream.cursor += static_cast<u32>(length);
		return true;
	}

	// Custom read function for libpng, so we could decode images from a buffer
	static void read_callback(png_structp png_ptr, png_bytep out, png_size_t length)
	{
		auto* const stream = static_cast<PngDecStreamHost*>(png_get_io_ptr(png_ptr));

		if (!stream || !read_input(*stream, out, length))
		{
			png_error(png_ptr, "Not enough data");
		}
	}

	// Calls the display callback if needed and copies a row of the progressive decoder to the output buffer set by the user. Returns false if the output buffer is invalid
	static bool output_progressive_row(PngDecStreamHost& stream, png_structp png_ptr, png_bytep new_row, png_uint_32 row_num, int pass)
	{
		const auto disp_info = stream.disp_info;
		const auto disp_param = stream.disp_param;

		// we have to check this everytime as this func can be called multiple times per row, and/or only once per row
		if (stream.next_row + stream.output_counts == row_num)
			stream.next_row = row_num;

		if (stream.ppu && (stream.next_row == row_num || pass > 0))
		{
			if (pass > 0)
			{
				disp_info->scanPassCount = pass;
				disp_info->nextOutputStartY = row_num;
			}
			else
			{
				disp_info->scanPassCount = 0;
				disp_info->nextOutputStartY = 0;
			}

			disp_info->outputImage = disp_param->nextOutputImage;
			stream.cb_ctrl_disp.cbCtrlDispFunc(*stream.ppu, disp_info, disp_param, stream.cb_ctrl_disp.cbCtrlDispArg);
			disp_info->outputStartY = row_num;
		}

		// The output parameters can be modified by the guest callback
		const auto addr = get_progressive_row_addr(disp_param->nextOutputImage.addr(), row_num, disp_info->outputStartY, disp_info->outputFrameWidthByte, pass, stream.row_buffer.size());

		if (!addr || !vm::check_addr(*addr, vm::page_readable | vm::page_writable, static_cast<u32>(stream.row_buffer.size())))
		{
			cellPngDec.error("Invalid output buffer for row %d (nextOutputImage=*0x%x, outputStartY=%d, outputFrameWidthByte=0x%llx)",
				row_num, disp_param->nextOutputImage, disp_info->outputStartY, disp_info->outputFrameWidthByte);
			return false;
		}

		// Interlaced rows are combined with the previous contents of the output buffer
		std::memcpy(stream.row_buffer.data(), vm::base(*addr), stream.row_buffer.size());
		png_progressive_combine_row(png_ptr, stream.row_buffer.data(), new_row);
		std::memcpy(vm::base(*addr), stream.row_buffer.data(), stream.row_buffer.size());
		return true;
	}

	static void row_callback(png_structp png_ptr, png_bytep new_row, png_uint_32 row_num, int pass)
	{
		auto* const stream = static_cast<PngDecStreamHost*>(png_get_progressive_ptr(png_ptr));

		if (!stream || !output_progressive_row(*stream, png_ptr, new_row, row_num, pass))
		{
			png_error(png_ptr, "Invalid output buffer");
		}
	}

	static void info_callback(png_structp png_ptr, png_infop /*info*/)
	{
		auto* const stream = static_cast<PngDecStreamHost*>(png_get_progressive_ptr(png_ptr));

		if (!stream)
		{
			png_error(png_ptr, "Failed to obtain the stream in info_callback");
		}

		const usz remaining = png_process_data_pause(png_ptr, false);

		// Only the input buffer of the stream advances the cursor, not the data provided by the stream callback
		if (stream->pushing_source && remaining <= stream->push_size)
		{
			stream->cursor += stream->push_size - static_cast<u32>(remaining);
		}
	}

	static void end_callback(png_structp png_ptr, png_infop /*info*/)
	{
		auto* const stream = static_cast<PngDecStreamHost*>(png_get_progressive_ptr(png_ptr));

		if (!stream)
		{
			png_error(png_ptr, "Failed to obtain the stream in end_callback");
		}

		stream->end_of_file = true;
	}

	// Custom error handler for libpng, jumps back to png_try()
	[[noreturn]] static void error_callback(png_structp png_ptr, png_const_charp error_message)
	{
		cellPngDec.error("libpng error: %s", error_message);
		png_longjmp(png_ptr, 1);
	}

	// Custom warning handler for libpng
	static void warning_callback(png_structp /*png_ptr*/, png_const_charp error_message)
	{
		cellPngDec.warning("libpng warning: %s", error_message);
	}

	// Pushes data from the input buffer of the stream to the progressive decoder
	static bool push_source(PngDecStreamHost& stream)
	{
		if (stream.cursor >= stream.length)
		{
			return true;
		}

		// The buffer range was checked when the stream was opened
		stream.push_size = stream.length - stream.cursor;
		stream.pushing_source = true;

		const bool ok = png_try(stream.png_ptr, [&]()
		{
			png_process_data(stream.png_ptr, stream.info_ptr, static_cast<u8*>(vm::base(stream.data_addr + stream.cursor)), stream.push_size);
		});

		stream.pushing_source = false;
		return ok;
	}
}

using namespace png_dec_internal;

// Get the chunk information of the PNG file. IDAT is marked as existing, only after decoding or reading the header.
// Bits (if set indicates existence of the chunk):
// 0  - gAMA
// 1  - sBIT
// 2  - cHRM
// 3  - PLTE
// 4  - tRNS
// 5  - bKGD
// 6  - hIST
// 7  - pHYs
// 8  - oFFs
// 9  - tIME
// 10 - pCAL
// 11 - sRGB
// 12 - iCCP
// 13 - sPLT
// 14 - sCAL
// 15 - IDAT
// 16:30 - reserved
be_t<u32> pngDecGetChunkInformation(PngDecStreamHost& stream, bool IDAT = false)
{
	// The end result of the chunk information (bigger-endian)
	be_t<u32> chunk_information = 0;

	// Needed pointers for getting the chunk information
	f64 gamma;
	f64 red_x;
	f64 red_y;
	f64 green_x;
	f64 green_y;
	f64 blue_x;
	f64 blue_y;
	f64 white_x;
	f64 white_y;
	f64 width;
	f64 height;
	s32 intent;
	s32 num_trans;
	s32 num_palette;
	s32 unit_type;
	s32 type;
	s32 nparams;
	s32 compression_type;
	s32 unit;
	u16* hist;
	png_uint_32 proflen;
	iCCP_profile_type profile;
	png_bytep trans_alpha;
	png_charp units;
	png_charp name;
	png_charp purpose;
	png_charpp params;
	png_int_32 X0;
	png_int_32 X1;
	png_int_32 offset_x;
	png_int_32 offset_y;
	png_uint_32 res_x;
	png_uint_32 res_y;
	png_colorp palette;
	png_color_8p sig_bit;
	png_color_16p background;
	png_color_16p trans_color;
	png_sPLT_tp entries;
	png_timep mod_time;

	// Get chunk information and set the appropriate bits
	if (png_get_gAMA(stream.png_ptr, stream.info_ptr, &gamma))
	{
		chunk_information |= 1 << 0; // gAMA
	}

	if (png_get_sBIT(stream.png_ptr, stream.info_ptr, &sig_bit))
	{
		chunk_information |= 1 << 1; // sBIT
	}

	if (png_get_cHRM(stream.png_ptr, stream.info_ptr, &white_x, &white_y, &red_x, &red_y, &green_x, &green_y, &blue_x, &blue_y))
	{
		chunk_information |= 1 << 2; // cHRM
	}

	if (png_get_PLTE(stream.png_ptr, stream.info_ptr, &palette, &num_palette))
	{
		chunk_information |= 1 << 3; // PLTE
	}

	if (png_get_tRNS(stream.png_ptr, stream.info_ptr, &trans_alpha, &num_trans, &trans_color))
	{
		chunk_information |= 1 << 4; // tRNS
	}

	if (png_get_bKGD(stream.png_ptr, stream.info_ptr, &background))
	{
		chunk_information |= 1 << 5; // bKGD
	}

	if (png_get_hIST(stream.png_ptr, stream.info_ptr, &hist))
	{
		chunk_information |= 1 << 6; // hIST
	}

	if (png_get_pHYs(stream.png_ptr, stream.info_ptr, &res_x, &res_y, &unit_type))
	{
		chunk_information |= 1 << 7; // pHYs
	}

	if (png_get_oFFs(stream.png_ptr, stream.info_ptr, &offset_x, &offset_y, &unit_type))
	{
		chunk_information |= 1 << 8; // oFFs
	}

	if (png_get_tIME(stream.png_ptr, stream.info_ptr, &mod_time))
	{
		chunk_information |= 1 << 9; // tIME
	}

	if (png_get_pCAL(stream.png_ptr, stream.info_ptr, &purpose, &X0, &X1, &type, &nparams, &units, &params))
	{
		chunk_information |= 1 << 10; // pCAL
	}

	if (png_get_sRGB(stream.png_ptr, stream.info_ptr, &intent))
	{
		chunk_information |= 1 << 11; // sRGB
	}

	if (png_get_iCCP(stream.png_ptr, stream.info_ptr, &name, &compression_type, &profile, &proflen))
	{
		chunk_information |= 1 << 12; // iCCP
	}

	if (png_get_sPLT(stream.png_ptr, stream.info_ptr, &entries))
	{
		chunk_information |= 1 << 13; // sPLT
	}

	if (png_get_sCAL(stream.png_ptr, stream.info_ptr, &unit, &width, &height))
	{
		chunk_information |= 1 << 14; // sCAL
	}

	if (IDAT)
	{
		chunk_information |= 1 << 15; // IDAT
	}

	return chunk_information;
}

error_code pngDecCreate(ppu_thread& ppu, PPHandle png_handle, PThreadInParam thread_in_param, PThreadOutParam thread_out_param, PExtThreadInParam /*extra_thread_in_param*/ = vm::null, PExtThreadOutParam extra_thread_out_param = vm::null)
{
	// Block savestate creation during the malloc callback
	std::unique_lock savestate_lock{g_fxo->get<hle_locks_t>(), std::try_to_lock};

	if (!savestate_lock.owns_lock())
	{
		ppu.state += cpu_flag::again;
		return {};
	}

	// Check if partial image decoding is used
	if (extra_thread_out_param)
	{
		fmt::throw_exception("Partial image decoding is not supported.");
	}

	if (!png_handle || !thread_in_param || !thread_out_param)
	{
		return CELL_PNGDEC_ERROR_ARG;
	}

	// The parameters are located in guest memory, so they are only read once
	const vm::ptr<CellPngDecCbControlMalloc> malloc_func = thread_in_param->cbCtrlMallocFunc;
	const vm::ptr<void> malloc_arg = thread_in_param->cbCtrlMallocArg;
	const vm::ptr<CellPngDecCbControlFree> free_func = thread_in_param->cbCtrlFreeFunc;
	const vm::ptr<void> free_arg = thread_in_param->cbCtrlFreeArg;

	// Allocate memory for the decoder handle
	const u32 handle_addr = guest_malloc(ppu, malloc_func, malloc_arg, sizeof(PngHandle));

	if (!handle_addr)
	{
		cellPngDec.error("PNG decoder creation failed.");
		return CELL_PNGDEC_ERROR_FATAL;
	}

	// If the guest reused the memory of a handle without destroying it, the old host object is removed. Its sub handles keep their own copy of the callbacks
	if (const auto [old_id, old_main] = get_main(vm::ptr<PngHandle>::make(handle_addr)); old_main && old_main->handle_addr == handle_addr)
	{
		cellPngDec.warning("The PNG decoder at *0x%x was not destroyed", handle_addr);
		idm::remove_verify<PngDecMainHost>(old_id, old_main);
	}

	const u32 host_id = idm::make<PngDecMainHost>(handle_addr, malloc_func, malloc_arg, free_func, free_arg);

	if (!host_id)
	{
		free_func(ppu, vm::ptr<void>::make(handle_addr), free_arg);
		return CELL_PNGDEC_ERROR_FATAL;
	}

	const auto handle = vm::ptr<PngHandle>::make(handle_addr);
	handle->malloc_ = malloc_func;
	handle->malloc_arg = malloc_arg;
	handle->free_ = free_func;
	handle->free_arg = free_arg;
	handle->host_id = host_id;

	// Set handle pointer
	*png_handle = handle;

	// Set the version information
	thread_out_param->pngCodecVersion = PNGDEC_CODEC_VERSION;

	return CELL_OK;
}

error_code pngDecDestroy(ppu_thread& ppu, PHandle handle)
{
	// Block savestate creation during the free callback
	std::unique_lock savestate_lock{g_fxo->get<hle_locks_t>(), std::try_to_lock};

	if (!savestate_lock.owns_lock())
	{
		ppu.state += cpu_flag::again;
		return {};
	}

	const auto [main_id, main] = get_main(handle);

	if (!main)
	{
		return CELL_PNGDEC_ERROR_ARG;
	}

	// Deallocate the decoder handle memory, using the address and callback recorded when it was created
	if (main->free_func(ppu, vm::ptr<void>::make(main->handle_addr), main->free_arg) != 0)
	{
		cellPngDec.error("PNG decoder deallocation failed.");
		return CELL_PNGDEC_ERROR_FATAL;
	}

	idm::remove_verify<PngDecMainHost>(main_id, main);
	return CELL_OK;
}

error_code pngDecOpen(ppu_thread& ppu, PHandle handle, PPStream png_stream, PSrc source, POpenInfo open_info, PCbControlStream control_stream = vm::null, POpenParam open_param = vm::null)
{
	// Block savestate creation during the malloc and free callbacks
	std::unique_lock savestate_lock{g_fxo->get<hle_locks_t>(), std::try_to_lock};

	if (!savestate_lock.owns_lock())
	{
		ppu.state += cpu_flag::again;
		return {};
	}

	const auto main = get_main(handle).second;

	if (!main || !png_stream || !source || !open_info)
	{
		return CELL_PNGDEC_ERROR_ARG;
	}

	// The source is located in guest memory, so it's only read once
	const CellPngDecSrc src = *source;

	// partial decoding only supported with buffer type
	if (src.srcSelect != CELL_PNGDEC_BUFFER && control_stream)
	{
		cellPngDec.error("Attempted partial image decode with file.");
		return CELL_PNGDEC_ERROR_STREAM_FORMAT;
	}

	// Need to test it somewhere
	if (src.fileOffset != 0)
	{
		fmt::throw_exception("Non-0 file offset not supported.");
	}

	// Allocate memory for the stream structure
	const u32 stream_addr = guest_malloc(ppu, main->malloc_func, main->malloc_arg, sizeof(PngStream));

	if (!stream_addr)
	{
		cellPngDec.error("PNG stream creation failed.");
		return CELL_PNGDEC_ERROR_FATAL;
	}

	// If the guest reused the memory of a stream without closing it, the old host object is removed
	u32 old_stream_id = 0;

	if (const auto old_stream = idm::select<PngDecStreamHost>([&](u32 id, PngDecStreamHost& old) -> bool { old_stream_id = id; return old.stream_addr == stream_addr; }).ptr)
	{
		cellPngDec.warning("The PNG stream at *0x%x was not closed", stream_addr);

		if (old_stream->file)
		{
			idm::remove<lv2_fs_object, lv2_file>(old_stream->fd);
		}

		idm::remove_verify<PngDecStreamHost>(old_stream_id, old_stream);
	}

	const u32 stream_id = idm::make<PngDecStreamHost>(handle.addr(), stream_addr, *main);
	const auto stream = idm::get_unlocked<PngDecStreamHost>(stream_id);

	if (!stream)
	{
		main->free_func(ppu, vm::ptr<void>::make(stream_addr), main->free_arg);
		return CELL_PNGDEC_ERROR_FATAL;
	}

	// Frees everything that was allocated so far
	const auto fail = [&](error_code error) -> error_code
	{
		if (stream->file)
		{
			idm::remove<lv2_fs_object, lv2_file>(stream->fd);
		}

		if (stream->buffer_addr)
		{
			main->free_func(ppu, vm::ptr<void>::make(stream->buffer_addr), main->free_arg);
		}

		main->free_func(ppu, vm::ptr<void>::make(stream_addr), main->free_arg);
		idm::remove_verify<PngDecStreamHost>(stream_id, stream);
		return error;
	};

	const auto guest_stream = vm::ptr<PngStream>::make(stream_addr);
	std::memset(guest_stream.get_ptr(), 0, sizeof(PngStream));
	guest_stream->host_id = stream_id;

	stream->source = src;

	// Set memory info
	open_info->initSpaceAllocated = sizeof(PngStream);

	// Allocate memory for the second structure of the stream
	stream->buffer_addr = guest_malloc(ppu, main->malloc_func, main->malloc_arg, sizeof(PngBuffer));

	if (!stream->buffer_addr)
	{
		cellPngDec.error("Memory allocation for the PNG buffer structure failed.");
		return fail(CELL_PNGDEC_ERROR_FATAL);
	}

	std::memset(vm::base(stream->buffer_addr), 0, sizeof(PngBuffer));

	// Open the buffer/file and check the header
	u8 header[8];

	// Depending on the source type, get the first 8 bytes
	if (src.srcSelect == CELL_PNGDEC_FILE)
	{
		const auto real_path = vfs::get(src.fileName.get_ptr());

		// Open a file stream
		fs::file file_stream(real_path);

		// Check if opening of the PNG file failed
		if (!file_stream)
		{
			cellPngDec.error("Opening of PNG failed. (%s)", src.fileName.get_ptr());
			return fail(CELL_PNGDEC_ERROR_OPEN_FILE);
		}

		// Read the header
		if (file_stream.read(header, 8) != 8)
		{
			cellPngDec.error("PNG header is too small.");
			return fail(CELL_PNGDEC_ERROR_HEADER);
		}

		// Get the file descriptor
		stream->fd = idm::make<lv2_fs_object, lv2_file>(src.fileName.get_ptr(), std::move(file_stream), 0, 0, real_path);

		if (!stream->fd)
		{
			return fail(CELL_PNGDEC_ERROR_FATAL);
		}

		// Indicate that we need to read from a file stream
		stream->file = true;
	}
	else
	{
		// Not checked on LLE
		if (src.streamSize < 8u || !vm::check_addr(src.streamPtr.addr(), vm::page_readable, src.streamSize))
		{
			cellPngDec.error("Invalid PNG stream (streamPtr=*0x%x, streamSize=0x%x)", src.streamPtr, src.streamSize);
			return fail(CELL_PNGDEC_ERROR_ARG);
		}

		stream->data_addr = src.streamPtr.addr();
		stream->length = src.streamSize;
		stream->cursor = 8;

		// We can simply copy the first 8 bytes
		std::memcpy(header, src.streamPtr.get_ptr(), 8);
	}

	// Check if the header indicates a valid PNG file
	if (png_sig_cmp(header, 0, 8))
	{
		cellPngDec.error("PNG signature is invalid.");
		return fail(CELL_PNGDEC_ERROR_HEADER);
	}

	// Create a libpng structure, also pass our custom error/warning functions
	stream->png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, error_callback, warning_callback);

	// Check if the creation of the structure failed
	if (!stream->png_ptr)
	{
		cellPngDec.error("Creation of png_structp failed.");
		return fail(CELL_PNGDEC_ERROR_FATAL);
	}

	// Create a libpng info structure
	if (!png_try(stream->png_ptr, [&]() { stream->info_ptr = png_create_info_struct(stream->png_ptr); }) || !stream->info_ptr)
	{
		cellPngDec.error("Creation of png_infop failed.");
		return fail(CELL_PNGDEC_ERROR_FATAL);
	}

	// We must indicate, that we allocated more memory
	open_info->initSpaceAllocated += u32{sizeof(PngBuffer)};

	// The libpng callbacks only access the host object
	if (control_stream)
	{
		if (open_param && open_param->selectChunk != 0u)
			fmt::throw_exception("Partial Decoding with selectChunk not supported yet.");

		stream->cb_ctrl_stream = *control_stream;
		stream->progressive = true;

		png_set_progressive_read_fn(stream->png_ptr, stream.get(), info_callback, row_callback, end_callback);

		// push header tag to libpng to keep us in sync
		if (!png_try(stream->png_ptr, [&]() { png_process_data(stream->png_ptr, stream->info_ptr, header, 8); }))
		{
			return fail(CELL_PNGDEC_ERROR_HEADER);
		}
	}
	else
	{
		png_set_read_fn(stream->png_ptr, stream.get(), read_callback);

		// We need to tell libpng, that we already read 8 bytes
		png_set_sig_bytes(stream->png_ptr, 8);
	}

	// Use virtual memory address as a handle
	*png_stream = guest_stream;

	return CELL_OK;
}

error_code pngDecClose(ppu_thread& ppu, PHandle handle, PStream stream)
{
	// Block savestate creation during the free callbacks
	std::unique_lock savestate_lock{g_fxo->get<hle_locks_t>(), std::try_to_lock};

	if (!savestate_lock.owns_lock())
	{
		ppu.state += cpu_flag::again;
		return {};
	}

	const auto [stream_id, host] = get_stream(handle, stream);

	if (!host)
	{
		return CELL_PNGDEC_ERROR_ARG;
	}

	const busy_lock lock(*host);

	if (!lock)
	{
		return CELL_PNGDEC_ERROR_BUSY;
	}

	// Remove the file descriptor, if a file descriptor was used for decoding
	if (host->file)
	{
		idm::remove<lv2_fs_object, lv2_file>(host->fd);
	}

	// libpng is freed together with the host object
	idm::remove_verify<PngDecStreamHost>(stream_id, host);

	// Deallocate the memory, using the addresses and callback recorded when the stream was opened
	if (host->buffer_addr && host->free_func(ppu, vm::ptr<void>::make(host->buffer_addr), host->free_arg) != 0)
	{
		cellPngDec.error("PNG buffer decoding structure deallocation failed.");
		return CELL_PNGDEC_ERROR_FATAL;
	}

	if (host->free_func(ppu, vm::ptr<void>::make(host->stream_addr), host->free_arg) != 0)
	{
		cellPngDec.error("PNG stream deallocation failed.");
		return CELL_PNGDEC_ERROR_FATAL;
	}

	return CELL_OK;
}

void pngSetHeader(PngDecStreamHost& stream)
{
	stream.info.imageWidth = png_get_image_width(stream.png_ptr, stream.info_ptr);
	stream.info.imageHeight = png_get_image_height(stream.png_ptr, stream.info_ptr);
	stream.info.numComponents = png_get_channels(stream.png_ptr, stream.info_ptr);
	stream.info.colorSpace = getPngDecColourType(png_get_color_type(stream.png_ptr, stream.info_ptr));
	stream.info.bitDepth = png_get_bit_depth(stream.png_ptr, stream.info_ptr);
	stream.info.interlaceMethod = png_get_interlace_type(stream.png_ptr, stream.info_ptr);
	stream.info.chunkInformation = pngDecGetChunkInformation(stream);
}

namespace png_dec_internal
{
	static error_code read_header(PHandle handle, PStream stream, PInfo info, PExtInfo extra_info = vm::null, bool ext = false)
	{
		const auto [stream_id, host] = get_stream(handle, stream);

		if (!host)
		{
			return CELL_PNGDEC_ERROR_ARG;
		}

		const busy_lock lock(*host);

		if (!lock)
		{
			return CELL_PNGDEC_ERROR_BUSY;
		}

		if (host->lost)
		{
			return CELL_PNGDEC_ERROR_FATAL;
		}

		if (!info)
		{
			return CELL_PNGDEC_ERROR_ARG;
		}

		// Set the reserved value to 0, if passed to the function. (Should this be arg error if they dont pass?)
		if (ext && extra_info)
		{
			extra_info->reserved = 0;
		}

		PngDecStreamHost& s = *host;

		if (s.progressive)
		{
			// lets push what we have so far
			if (!push_source(s))
			{
				return CELL_PNGDEC_ERROR_HEADER;
			}
		}
		else
		{
			// Read the header info
			if (!png_try(s.png_ptr, [&]() { png_read_info(s.png_ptr, s.info_ptr); }))
			{
				return CELL_PNGDEC_ERROR_HEADER;
			}
		}

		pngSetHeader(s);

		// png doesnt allow empty image, so quick check for 0 verifys if we got the header
		// not sure exactly what should happen if we dont have header, ask for more data with callback?
		if (s.info.imageWidth == 0u)
		{
			cellPngDec.error("Invalid or not enough data sent to get header");
			return CELL_PNGDEC_ERROR_HEADER;
		}

		// Set the pointer to stream info
		*info = s.info;
		return CELL_OK;
	}
}

error_code pngDecSetParameter(PHandle handle, PStream stream, PInParam _in_param, POutParam out_param, PExtInParam _extra_in_param = vm::null, PExtOutParam extra_out_param = vm::null)
{
	const auto [stream_id, host] = get_stream(handle, stream);

	if (!host)
	{
		return CELL_PNGDEC_ERROR_ARG;
	}

	const busy_lock lock(*host);

	if (!lock)
	{
		return CELL_PNGDEC_ERROR_BUSY;
	}

	if (host->lost)
	{
		return CELL_PNGDEC_ERROR_FATAL;
	}

	if (!_in_param || !out_param)
	{
		return CELL_PNGDEC_ERROR_ARG;
	}

	// The parameters are located in guest memory, so they are only read once
	const CellPngDecInParam in_param = *_in_param;
	const std::optional<CellPngDecExtInParam> extra_in_param = _extra_in_param ? std::optional(*_extra_in_param) : std::nullopt;

	if (in_param.outputPackFlag == CELL_PNGDEC_1BYTE_PER_NPIXEL)
	{
		fmt::throw_exception("Packing not supported! (%d)", in_param.outputPackFlag);
	}

	if (extra_in_param && extra_in_param->bufferMode != CELL_PNGDEC_LINE_MODE)
	{
		cellPngDec.error("Invalid Buffermode specified.");
		return CELL_PNGDEC_ERROR_ARG;
	}

	PngDecStreamHost& s = *host;
	bool unsupported_conversion = false;

	const bool ok = png_try(s.png_ptr, [&]()
	{
		// flag to keep unknown chunks
		png_set_keep_unknown_chunks(s.png_ptr, PNG_HANDLE_CHUNK_IF_SAFE, nullptr, 0);

		// Scale 16 bit depth down to 8 bit depth.
		if (s.info.bitDepth == 16u && in_param.outputBitDepth == 8u)
		{
			// PS3 uses png_set_strip_16, since png_set_scale_16 wasn't available back then.
			png_set_strip_16(s.png_ptr);
		}

		if (s.info.colorSpace != in_param.outputColorSpace)
		{
			// check if we need to set alpha
			const bool inputHasAlpha = cellPngColorSpaceHasAlpha(s.info.colorSpace);
			const bool outputWantsAlpha = cellPngColorSpaceHasAlpha(in_param.outputColorSpace);

			if (outputWantsAlpha && !inputHasAlpha)
			{
				if (in_param.outputAlphaSelect == CELL_PNGDEC_FIX_ALPHA)
					png_set_add_alpha(s.png_ptr, in_param.outputColorAlpha, in_param.outputColorSpace == CELL_PNGDEC_ARGB ? PNG_FILLER_BEFORE : PNG_FILLER_AFTER);
				else
				{
					// Check if we can steal the alpha from a trns block
					if (png_get_valid(s.png_ptr, s.info_ptr, PNG_INFO_tRNS))
						png_set_tRNS_to_alpha(s.png_ptr);
					// if not, just set default of 0xff
					else
						png_set_add_alpha(s.png_ptr, 0xff, in_param.outputColorSpace == CELL_PNGDEC_ARGB ? PNG_FILLER_BEFORE : PNG_FILLER_AFTER);
				}
			}
			else if (inputHasAlpha && !outputWantsAlpha)
				png_set_strip_alpha(s.png_ptr);
			else if (in_param.outputColorSpace == CELL_PNGDEC_ARGB && s.info.colorSpace == CELL_PNGDEC_RGBA)
				png_set_swap_alpha(s.png_ptr);

			// Handle gray<->rgb colorspace conversions
			// rgb output
			if (in_param.outputColorSpace == CELL_PNGDEC_ARGB
				|| in_param.outputColorSpace == CELL_PNGDEC_RGBA
				|| in_param.outputColorSpace == CELL_PNGDEC_RGB)
			{

				if (s.info.colorSpace == CELL_PNGDEC_PALETTE)
					png_set_palette_to_rgb(s.png_ptr);
				if ((s.info.colorSpace == CELL_PNGDEC_GRAYSCALE || s.info.colorSpace == CELL_PNGDEC_GRAYSCALE_ALPHA)
					&& s.info.bitDepth < 8)
					png_set_expand_gray_1_2_4_to_8(s.png_ptr);
			}
			// grayscale output
			else if (s.info.colorSpace == CELL_PNGDEC_ARGB
				|| s.info.colorSpace == CELL_PNGDEC_RGBA
				|| s.info.colorSpace == CELL_PNGDEC_RGB)
			{
				png_set_rgb_to_gray(s.png_ptr, PNG_ERROR_ACTION_NONE, PNG_RGB_TO_GRAY_DEFAULT, PNG_RGB_TO_GRAY_DEFAULT);
			}
			else
			{
				unsupported_conversion = true;
			}
		}

		s.passes = png_set_interlace_handling(s.png_ptr);

		// Update the info structure
		png_read_update_info(s.png_ptr, s.info_ptr);
	});

	if (!ok)
	{
		return CELL_PNGDEC_ERROR_STREAM_FORMAT;
	}

	// This shouldnt ever happen, but not sure what to do if it does, just want it logged for now
	if (s.info.bitDepth != 16u && in_param.outputBitDepth == 16u)
		cellPngDec.error("Output depth of 16 with non input depth of 16 specified!");
	if (in_param.commandPtr)
		cellPngDec.warning("Ignoring CommandPtr.");
	if (unsupported_conversion)
	{
		// not sure what to do here
		cellPngDec.error("Grayscale / Palette to Grayscale / Palette conversion currently unsupported.");
	}

	s.out_param.outputWidth = s.info.imageWidth;
	s.out_param.outputHeight = s.info.imageHeight;
	s.out_param.outputBitDepth = in_param.outputBitDepth;
	s.out_param.outputColorSpace = in_param.outputColorSpace;
	s.out_param.outputMode = in_param.outputMode;

	s.out_param.outputWidthByte = png_get_rowbytes(s.png_ptr, s.info_ptr);
	s.out_param.outputComponents = png_get_channels(s.png_ptr, s.info_ptr);

	s.packing = in_param.outputPackFlag;

	// Set the memory usage. We currently don't actually allocate memory for libpng through the callbacks, due to libpng needing a lot more memory compared to PS3 variant.
	s.out_param.useMemorySpace = 0;

	// libpng decodes rows into this buffer, which is then copied to guest memory
	s.row_buffer.resize(s.out_param.outputWidthByte);
	s.parameters_set = true;

	if (extra_in_param)
	{
		if (s.passes > 1)
		{
			s.output_counts = 1;
		}
		else
			s.output_counts = extra_in_param->outputCounts;

		if (extra_out_param)
		{
			if (s.output_counts == 0)
				extra_out_param->outputHeight = s.out_param.outputHeight;
			else
				extra_out_param->outputHeight = std::min(s.output_counts, s.out_param.outputHeight.value());
			extra_out_param->outputWidthByte = s.out_param.outputWidthByte;
		}
	}

	*out_param = s.out_param;

	return CELL_OK;
}

error_code pngDecodeData(ppu_thread& ppu, PHandle handle, PStream stream, vm::ptr<u8> data, PDataControlParam data_control_param, PDataOutInfo data_out_info, PCbControlDisp cb_control_disp = vm::null, PDispParam disp_param = vm::null)
{
	// Block savestate creation during the malloc, free and progressive decoding callbacks
	std::unique_lock savestate_lock{g_fxo->get<hle_locks_t>(), std::try_to_lock};

	if (!savestate_lock.owns_lock())
	{
		ppu.state += cpu_flag::again;
		return {};
	}

	const auto [stream_id, host] = get_stream(handle, stream);

	if (!host)
	{
		return CELL_PNGDEC_ERROR_ARG;
	}

	const busy_lock lock(*host);

	if (!lock)
	{
		return CELL_PNGDEC_ERROR_BUSY;
	}

	if (host->lost)
	{
		return CELL_PNGDEC_ERROR_FATAL;
	}

	if (!data_control_param || !data_out_info)
	{
		return CELL_PNGDEC_ERROR_ARG;
	}

	PngDecStreamHost& s = *host;

	// Indicate, that the PNG decoding is stopped/failed. This is incase, we return an error code in the middle of decoding
	data_out_info->status = CELL_PNGDEC_DEC_STATUS_STOP;

	// The row buffer is only allocated once the output parameters are set
	if (!s.parameters_set)
	{
		cellPngDec.error("Attempted to decode before setting the parameters");
		return CELL_PNGDEC_ERROR_SEQ;
	}

	const u64 bytes_per_line = data_control_param->outputBytesPerLine;
	const u64 row_bytes = s.row_buffer.size();

	if (bytes_per_line < s.out_param.outputWidthByte || row_bytes != s.out_param.outputWidthByte)
	{
		cellPngDec.error("Bytes per line less than expected output! Got: %d, expected: %d", bytes_per_line, s.out_param.outputWidthByte);
		return CELL_PNGDEC_ERROR_ARG;
	}

	// partial decoding
	if (cb_control_disp && s.output_counts > 0)
	{
		if (!disp_param || bytes_per_line > u32{umax})
		{
			return CELL_PNGDEC_ERROR_ARG;
		}

		// get data from cb
		const auto stream_info = vm::ptr<CellPngDecStrmInfo>::make(guest_malloc(ppu, s.malloc_func, s.malloc_arg, sizeof(CellPngDecStrmInfo)));
		const auto stream_param = vm::ptr<CellPngDecStrmParam>::make(guest_malloc(ppu, s.malloc_func, s.malloc_arg, sizeof(CellPngDecStrmParam)));
		s.disp_info = vm::ptr<CellPngDecDispInfo>::make(guest_malloc(ppu, s.malloc_func, s.malloc_arg, sizeof(CellPngDecDispInfo)));
		s.disp_param = vm::ptr<CellPngDecDispParam>::make(guest_malloc(ppu, s.malloc_func, s.malloc_arg, sizeof(CellPngDecDispParam)));

		const auto free_mem = [&]()
		{
			for (const u32 addr : { stream_info.addr(), stream_param.addr(), s.disp_info.addr(), s.disp_param.addr() })
			{
				if (addr)
				{
					s.free_func(ppu, vm::ptr<void>::make(addr), s.free_arg);
				}
			}

			s.disp_info = vm::null;
			s.disp_param = vm::null;
			s.ppu = nullptr;
		};

		if (!stream_info || !stream_param || !s.disp_info || !s.disp_param)
		{
			free_mem();
			return CELL_PNGDEC_ERROR_FATAL;
		}

		// set things that won't change between callbacks
		s.disp_info->outputFrameWidthByte = bytes_per_line;
		s.disp_info->outputFrameHeight = s.out_param.outputHeight;
		s.disp_info->outputWidthByte = s.out_param.outputWidthByte;
		s.disp_info->outputBitDepth = s.out_param.outputBitDepth;
		s.disp_info->outputComponents = s.out_param.outputComponents;
		s.disp_info->outputHeight = s.output_counts;
		s.disp_info->outputStartXByte = 0;
		s.disp_info->outputStartY = 0;
		s.disp_info->scanPassCount = 0;
		s.disp_info->nextOutputStartY = 0;

		s.ppu = &ppu;
		s.next_row = s.output_counts;
		s.cb_ctrl_disp = *cb_control_disp;

		s.disp_param->nextOutputImage = disp_param->nextOutputImage;

		stream_info->decodedStrmSize = s.cursor;

		// push the rest of the buffer we have
		if (!push_source(s))
		{
			free_mem();
			return CELL_PNGDEC_ERROR_STREAM_FORMAT;
		}

		stream_info->decodedStrmSize = s.length;

		// todo: commandPtr
		// then just loop until the end, the callbacks should take care of the rest
		while (!s.end_of_file)
		{
			s.cb_ctrl_stream.cbCtrlStrmFunc(ppu, stream_info, stream_param, s.cb_ctrl_stream.cbCtrlStrmArg);

			// The stream parameters are set by the guest callback
			const u32 strm_addr = stream_param->strmPtr.addr();
			const u32 strm_size = stream_param->strmSize;

			if (!vm::check_addr(strm_addr, vm::page_readable, strm_size))
			{
				cellPngDec.error("Invalid stream data (strmPtr=*0x%x, strmSize=0x%x)", strm_addr, strm_size);
				free_mem();
				return CELL_PNGDEC_ERROR_STREAM_FORMAT;
			}

			stream_info->decodedStrmSize += strm_size;

			if (!png_try(s.png_ptr, [&]() { png_process_data(s.png_ptr, s.info_ptr, static_cast<u8*>(vm::base(strm_addr)), strm_size); }))
			{
				free_mem();
				return CELL_PNGDEC_ERROR_STREAM_FORMAT;
			}
		}

		free_mem();
	}
	else
	{
		if (!data)
		{
			return CELL_PNGDEC_ERROR_ARG;
		}

		// The whole output range has to be writable
		const u32 height = s.out_param.outputHeight;
		const auto output_size = get_output_size(height, bytes_per_line, row_bytes);

		if (!output_size || !vm::check_addr(data.addr(), vm::page_readable | vm::page_writable, *output_size))
		{
			cellPngDec.error("Invalid output buffer (data=*0x%x, height=%d, bytes_per_line=0x%llx, row_bytes=0x%llx)", data, height, bytes_per_line, row_bytes);
			return CELL_PNGDEC_ERROR_ARG;
		}

		// Check if the image needs to be flipped
		const bool flip = s.out_param.outputMode == CELL_PNGDEC_BOTTOM_TO_TOP;

		// Decode the image
		// todo: commandptr
		for (u32 j = 0; j < s.passes; j++)
		{
			for (u32 i = 0; i < height; ++i)
			{
				const u32 line = flip ? height - i - 1 : i;
				u8* const row = static_cast<u8*>(vm::base(data.addr() + static_cast<u32>(line * bytes_per_line)));

				// Interlaced rows are combined with the previous contents of the output buffer
				std::memcpy(s.row_buffer.data(), row, row_bytes);

				if (!png_try(s.png_ptr, [&]() { png_read_row(s.png_ptr, s.row_buffer.data(), nullptr); }))
				{
					return CELL_PNGDEC_ERROR_STREAM_FORMAT;
				}

				std::memcpy(row, s.row_buffer.data(), row_bytes);
			}
		}

		if (!png_try(s.png_ptr, [&]() { png_read_end(s.png_ptr, s.info_ptr); }))
		{
			return CELL_PNGDEC_ERROR_STREAM_FORMAT;
		}
	}

	// Get the number of iTXt, tEXt and zTXt chunks
	const s32 text_chunks = png_get_text(s.png_ptr, s.info_ptr, nullptr, nullptr);

	// Set the chunk information and the previously obtained number of text chunks
	data_out_info->numText = static_cast<u32>(text_chunks);
	data_out_info->chunkInformation = pngDecGetChunkInformation(s, true);
	png_unknown_chunkp unknowns;
	const int num_unknowns = png_get_unknown_chunks(s.png_ptr, s.info_ptr, &unknowns);
	data_out_info->numUnknownChunk = num_unknowns;

	// Indicate that the decoding succeeded
	data_out_info->status = CELL_PNGDEC_DEC_STATUS_FINISH;

	return CELL_OK;
}

error_code cellPngDecCreate(ppu_thread& ppu, PPHandle handle, PThreadInParam threadInParam, PThreadOutParam threadOutParam)
{
	cellPngDec.warning("cellPngDecCreate(handle=**0x%x, threadInParam=*0x%x, threadOutParam=*0x%x)", handle, threadInParam, threadOutParam);
	return pngDecCreate(ppu, handle, threadInParam, threadOutParam);
}

error_code cellPngDecExtCreate(ppu_thread& ppu, PPHandle handle, PThreadInParam threadInParam, PThreadOutParam threadOutParam, PExtThreadInParam extThreadInParam, PExtThreadOutParam extThreadOutParam)
{
	cellPngDec.warning("cellPngDecExtCreate(mainHandle=**0x%x, threadInParam=*0x%x, threadOutParam=*0x%x, extThreadInParam=*0x%x, extThreadOutParam=*0x%x)", handle, threadInParam, threadOutParam, extThreadInParam, extThreadOutParam);
	return pngDecCreate(ppu, handle, threadInParam, threadOutParam, extThreadInParam, extThreadOutParam);
}

error_code cellPngDecDestroy(ppu_thread& ppu, PHandle handle)
{
	cellPngDec.warning("cellPngDecDestroy(mainHandle=*0x%x)", handle);
	return pngDecDestroy(ppu, handle);
}

error_code cellPngDecOpen(ppu_thread& ppu, PHandle handle, PPStream stream, PSrc src, POpenInfo openInfo)
{
	cellPngDec.warning("cellPngDecOpen(handle=*0x%x, stream=**0x%x, src=*0x%x, openInfo=*0x%x)", handle, stream, src, openInfo);
	return pngDecOpen(ppu, handle, stream, src, openInfo);
}

error_code cellPngDecExtOpen(ppu_thread& ppu, PHandle handle, PPStream stream, PSrc src, POpenInfo openInfo, PCbControlStream cbCtrlStrm, POpenParam opnParam)
{
	cellPngDec.warning("cellPngDecExtOpen(handle=*0x%x, stream=**0x%x, src=*0x%x, openInfo=*0x%x, cbCtrlStrm=*0x%x, opnParam=*0x%x)", handle, stream, src, openInfo, cbCtrlStrm, opnParam);
	return pngDecOpen(ppu, handle, stream, src, openInfo, cbCtrlStrm, opnParam);
}

error_code cellPngDecClose(ppu_thread& ppu, PHandle handle, PStream stream)
{
	cellPngDec.warning("cellPngDecClose(handle=*0x%x, stream=*0x%x)", handle, stream);
	return pngDecClose(ppu, handle, stream);
}

error_code cellPngDecReadHeader(PHandle handle, PStream stream, PInfo info)
{
	cellPngDec.warning("cellPngDecReadHeader(handle=*0x%x, stream=*0x%x, info=*0x%x)", handle, stream, info);
	return read_header(handle, stream, info);
}

error_code cellPngDecExtReadHeader(PHandle handle, PStream stream, PInfo info, PExtInfo extInfo)
{
	cellPngDec.warning("cellPngDecExtReadHeader(handle=*0x%x, stream=*0x%x, info=*0x%x, extInfo=*0x%x)", handle, stream, info, extInfo);
	return read_header(handle, stream, info, extInfo, true);
}

error_code cellPngDecSetParameter(PHandle handle, PStream stream, PInParam inParam, POutParam outParam)
{
	cellPngDec.warning("cellPngDecSetParameter(handle=*0x%x, stream=*0x%x, inParam=*0x%x, outParam=*0x%x)", handle, stream, inParam, outParam);
	return pngDecSetParameter(handle, stream, inParam, outParam);
}

error_code cellPngDecExtSetParameter(PHandle handle, PStream stream, PInParam inParam, POutParam outParam, PExtInParam extInParam, PExtOutParam extOutParam)
{
	cellPngDec.warning("cellPngDecExtSetParameter(handle=*0x%x, stream=*0x%x, inParam=*0x%x, outParam=*0x%x, extInParam=*0x%x, extOutParam=*0x%x", handle, stream, inParam, outParam, extInParam, extOutParam);
	return pngDecSetParameter(handle, stream, inParam, outParam, extInParam, extOutParam);
}

error_code cellPngDecDecodeData(ppu_thread& ppu, PHandle handle, PStream stream, vm::ptr<u8> data, PDataControlParam dataCtrlParam, PDataOutInfo dataOutInfo)
{
	cellPngDec.warning("cellPngDecDecodeData(handle=*0x%x, stream=*0x%x, data=*0x%x, dataCtrlParam=*0x%x, dataOutInfo=*0x%x)", handle, stream, data, dataCtrlParam, dataOutInfo);
	return pngDecodeData(ppu, handle, stream, data, dataCtrlParam, dataOutInfo);
}

error_code cellPngDecExtDecodeData(ppu_thread& ppu, PHandle handle, PStream stream, vm::ptr<u8> data, PDataControlParam dataCtrlParam, PDataOutInfo dataOutInfo, PCbControlDisp cbCtrlDisp, PDispParam dispParam)
{
	cellPngDec.warning("cellPngDecExtDecodeData(handle=*0x%x, stream=*0x%x, data=*0x%x, dataCtrlParam=*0x%x, dataOutInfo=*0x%x, cbCtrlDisp=*0x%x, dispParam=*0x%x)", handle, stream, data, dataCtrlParam, dataOutInfo, cbCtrlDisp, dispParam);
	return pngDecodeData(ppu, handle, stream, data, dataCtrlParam, dataOutInfo, cbCtrlDisp, dispParam);
}

error_code cellPngDecGetUnknownChunks(PHandle handle, PStream stream, vm::pptr<CellPngUnknownChunk> unknownChunk, vm::ptr<u32> unknownChunkNumber)
{
	cellPngDec.todo("cellPngDecGetUnknownChunks(handle=*0x%x, stream=*0x%x, unknownChunk=*0x%x, unknownChunkNumber=*0x%x)", handle, stream, unknownChunk, unknownChunkNumber);
	return CELL_OK;
}

error_code cellPngDecGetpCAL(PHandle handle, PStream stream, vm::ptr<CellPngPCAL> pcal)
{
	cellPngDec.todo("cellPngDecGetpCAL(handle=*0x%x, stream=*0x%x, pcal=*0x%x)", handle, stream, pcal);
	return CELL_OK;
}

error_code cellPngDecGetcHRM(PHandle handle, PStream stream, vm::ptr<CellPngCHRM> chrm)
{
	cellPngDec.todo("cellPngDecGetcHRM(handle=*0x%x, stream=*0x%x, chrm=*0x%x)", handle, stream, chrm);
	return CELL_OK;
}

error_code cellPngDecGetsCAL(PHandle handle, PStream stream, vm::ptr<CellPngSCAL> scal)
{
	cellPngDec.todo("cellPngDecGetsCAL(handle=*0x%x, stream=*0x%x, scal=*0x%x)", handle, stream, scal);
	return CELL_OK;
}

error_code cellPngDecGetpHYs(PHandle handle, PStream stream, vm::ptr<CellPngPHYS> phys)
{
	cellPngDec.todo("cellPngDecGetpHYs(handle=*0x%x, stream=*0x%x, phys=*0x%x)", handle, stream, phys);
	return CELL_OK;
}

error_code cellPngDecGetoFFs(PHandle handle, PStream stream, vm::ptr<CellPngOFFS> offs)
{
	cellPngDec.todo("cellPngDecGetoFFs(handle=*0x%x, stream=*0x%x, offs=*0x%x)", handle, stream, offs);
	return CELL_OK;
}

error_code cellPngDecGetsPLT(PHandle handle, PStream stream, vm::ptr<CellPngSPLT> splt)
{
	cellPngDec.todo("cellPngDecGetsPLT(handle=*0x%x, stream=*0x%x, splt=*0x%x)", handle, stream, splt);
	return CELL_OK;
}

error_code cellPngDecGetbKGD(PHandle handle, PStream stream, vm::ptr<CellPngBKGD> bkgd)
{
	cellPngDec.todo("cellPngDecGetbKGD(handle=*0x%x, stream=*0x%x, bkgd=*0x%x)", handle, stream, bkgd);
	return CELL_OK;
}

error_code cellPngDecGettIME(PHandle handle, PStream stream, vm::ptr<CellPngTIME> time)
{
	cellPngDec.todo("cellPngDecGettIME(handle=*0x%x, stream=*0x%x, time=*0x%x)", handle, stream, time);
	return CELL_OK;
}

error_code cellPngDecGethIST(PHandle handle, PStream stream, vm::ptr<CellPngHIST> hist)
{
	cellPngDec.todo("cellPngDecGethIST(handle=*0x%x, stream=*0x%x, hist=*0x%x)", handle, stream, hist);
	return CELL_OK;
}

error_code cellPngDecGettRNS(PHandle handle, PStream stream, vm::ptr<CellPngTRNS> trns)
{
	cellPngDec.todo("cellPngDecGettRNS(handle=*0x%x, stream=*0x%x, trns=*0x%x)", handle, stream, trns);
	return CELL_OK;
}

error_code cellPngDecGetsBIT(PHandle handle, PStream stream, vm::ptr<CellPngSBIT> sbit)
{
	cellPngDec.todo("cellPngDecGetsBIT(handle=*0x%x, stream=*0x%x, sbit=*0x%x)", handle, stream, sbit);
	return CELL_OK;
}

error_code cellPngDecGetiCCP(PHandle handle, PStream stream, vm::ptr<CellPngICCP> iccp)
{
	cellPngDec.todo("cellPngDecGetiCCP(handle=*0x%x, stream=*0x%x, iccp=*0x%x)", handle, stream, iccp);
	return CELL_OK;
}

error_code cellPngDecGetsRGB(PHandle handle, PStream stream, vm::ptr<CellPngSRGB> srgb)
{
	cellPngDec.todo("cellPngDecGetsRGB(handle=*0x%x, stream=*0x%x, srgb=*0x%x)", handle, stream, srgb);
	return CELL_OK;
}

error_code cellPngDecGetgAMA(PHandle handle, PStream stream, vm::ptr<CellPngGAMA> gama)
{
	cellPngDec.todo("cellPngDecGetgAMA(handle=*0x%x, stream=*0x%x, gama=*0x%x)", handle, stream, gama);
	return CELL_OK;
}

error_code cellPngDecGetPLTE(PHandle handle, PStream stream, vm::ptr<CellPngPLTE> plte)
{
	cellPngDec.todo("cellPngDecGetPLTE(handle=*0x%x, stream=*0x%x, plte=*0x%x)", handle, stream, plte);
	return CELL_OK;
}

error_code cellPngDecGetTextChunk(PHandle handle, PStream stream, vm::ptr<u32> textInfoNum, vm::pptr<CellPngTextInfo> textInfo)
{
	cellPngDec.todo("cellPngDecGetTextChunk(handle=*0x%x, stream=*0x%x, textInfoNum=*0x%x, textInfo=*0x%x)", handle, stream, textInfoNum, textInfo);
	return CELL_OK;
}

DECLARE(ppu_module_manager::cellPngDec)("cellPngDec", []()
{
	REG_FUNC(cellPngDec, cellPngDecGetUnknownChunks);
	REG_FUNC(cellPngDec, cellPngDecClose);
	REG_FUNC(cellPngDec, cellPngDecGetpCAL);
	REG_FUNC(cellPngDec, cellPngDecGetcHRM);
	REG_FUNC(cellPngDec, cellPngDecGetsCAL);
	REG_FUNC(cellPngDec, cellPngDecGetpHYs);
	REG_FUNC(cellPngDec, cellPngDecGetoFFs);
	REG_FUNC(cellPngDec, cellPngDecGetsPLT);
	REG_FUNC(cellPngDec, cellPngDecGetbKGD);
	REG_FUNC(cellPngDec, cellPngDecGettIME);
	REG_FUNC(cellPngDec, cellPngDecGethIST);
	REG_FUNC(cellPngDec, cellPngDecGettRNS);
	REG_FUNC(cellPngDec, cellPngDecGetsBIT);
	REG_FUNC(cellPngDec, cellPngDecGetiCCP);
	REG_FUNC(cellPngDec, cellPngDecGetsRGB);
	REG_FUNC(cellPngDec, cellPngDecGetgAMA);
	REG_FUNC(cellPngDec, cellPngDecGetPLTE);
	REG_FUNC(cellPngDec, cellPngDecGetTextChunk);
	REG_FUNC(cellPngDec, cellPngDecDestroy);
	REG_FUNC(cellPngDec, cellPngDecCreate);
	REG_FUNC(cellPngDec, cellPngDecExtCreate);
	REG_FUNC(cellPngDec, cellPngDecExtSetParameter);
	REG_FUNC(cellPngDec, cellPngDecSetParameter);
	REG_FUNC(cellPngDec, cellPngDecExtReadHeader);
	REG_FUNC(cellPngDec, cellPngDecReadHeader);
	REG_FUNC(cellPngDec, cellPngDecExtOpen);
	REG_FUNC(cellPngDec, cellPngDecOpen);
	REG_FUNC(cellPngDec, cellPngDecExtDecodeData);
	REG_FUNC(cellPngDec, cellPngDecDecodeData);
});
