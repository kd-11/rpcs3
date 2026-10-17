#pragma once

#include "Emu/Memory/vm.h"
#include "Emu/VFS.h"
#include "Utilities/mutex.h"
#include "util/serialization.hpp"

#include <optional>

// Shared by the HLE GIF and JPG decoders, which both decode with stb_image
namespace img_dec_internal
{
	// Largest encoded image the decoders accept: stb_image takes the length as an int
	constexpr u32 MAX_SOURCE_SIZE = 0x7fffffff;

	// Placement of the decoded rows in the guest output buffer
	struct output_layout
	{
		u32 row_bytes; // Bytes written per row
		u32 stride;    // Distance between the starts of two rows
		u32 size;      // Extent of the whole output
	};

	// The guest's bytes per line only adds padding: a smaller value (including 0) packs the rows, as the decoders always did
	constexpr std::optional<output_layout> get_output_layout(u64 bytes_per_line, u32 width, u32 height, u32 components)
	{
		if (!width || !height || !components)
		{
			return std::nullopt;
		}

		const u64 row_bytes = u64{width} * components;
		const u64 stride = std::max(bytes_per_line, row_bytes);

		if (stride > u32{umax})
		{
			return std::nullopt;
		}

		// Can't overflow: both factors are below 2^32
		const u64 size = stride * (height - 1) + row_bytes;

		if (size > u32{umax})
		{
			return std::nullopt;
		}

		return output_layout{static_cast<u32>(row_bytes), static_cast<u32>(stride), static_cast<u32>(size)};
	}

	// Copies packed decoded rows to the output, which must hold layout.size bytes.
	// to_argb converts RGBA pixels to ARGB, flip writes the rows from bottom to top.
	inline void write_rows(u8* dst, const output_layout& layout, const u8* src, u32 height, bool to_argb, bool flip)
	{
		for (u32 i = 0; i < height; i++)
		{
			const u8* src_row = src + u64{layout.row_bytes} * (flip ? height - 1 - i : i);
			u8* dst_row = dst + u64{layout.stride} * i;

			if (!to_argb)
			{
				std::memcpy(dst_row, src_row, layout.row_bytes);
				continue;
			}

			for (u32 j = 0; j + 4 <= layout.row_bytes; j += 4)
			{
				dst_row[j + 0] = src_row[j + 3];
				dst_row[j + 1] = src_row[j + 0];
				dst_row[j + 2] = src_row[j + 1];
				dst_row[j + 3] = src_row[j + 2];
			}
		}
	}

	// Saves or loads a guest ABI struct as raw bytes
	template <typename T> requires std::is_trivially_copyable_v<T>
	void serialize_bytes(utils::serial& ar, T& obj)
	{
		std::array<u8, sizeof(T)> bytes{};

		if (ar.is_writing())
		{
			std::memcpy(bytes.data(), &obj, sizeof(T));
			ar(bytes);
		}
		else
		{
			ar(bytes);
			std::memcpy(&obj, bytes.data(), sizeof(T));
		}
	}

	// Host state of an open stream. The guest only holds its ID, so guest writes can't choose the source, its size or the file to close.
	class image_stream
	{
	public:
		const bool from_file;        // Otherwise the source is a guest buffer
		const u32 stream_addr;       // Buffer source
		const u32 stream_size;       // Size of the buffer or the file, at most MAX_SOURCE_SIZE
		const std::string file_name; // Guest path of the file source

		// Serializes the operations on one stream
		shared_mutex mutex;

		image_stream(u32 stream_addr, u32 stream_size)
			: from_file(false)
			, stream_addr(stream_addr)
			, stream_size(stream_size)
		{
		}

		image_stream(std::string file_name, fs::file&& file, u32 file_size)
			: from_file(true)
			, stream_addr(0)
			, stream_size(file_size)
			, file_name(std::move(file_name))
			, file(std::move(file))
		{
		}

		explicit image_stream(utils::serial& ar)
			: from_file(ar.pop<bool>())
			, stream_addr(ar.pop<u32>())
			, stream_size(ar.pop<u32>())
			, file_name(ar.pop<std::string>())
		{
			if (from_file)
			{
				// Reopen the file by its guest path. If it changed, reading fails and decoding returns an error.
				file = fs::file(vfs::get(file_name));

				if (file && file.size() != stream_size)
				{
					file.close();
				}
			}
		}

		void save(utils::serial& ar)
		{
			ar(from_file, stream_addr, stream_size, file_name);
		}

		bool can_read() const
		{
			return !from_file || !!file;
		}

		// Reads the first size bytes of the source. Fails if the source is shorter, the buffer is not readable
		// or the file could not be reopened after loading a savestate.
		bool read(u8* dst, u32 size) const
		{
			if (size > stream_size)
			{
				return false;
			}

			if (!size)
			{
				return true;
			}

			if (!from_file)
			{
				if (!vm::check_addr(stream_addr, vm::page_readable, size))
				{
					return false;
				}

				std::memcpy(dst, vm::base(stream_addr), size);
				return true;
			}

			return file && file.read_at(0, dst, size) == size;
		}

	private:
		fs::file file;
	};
}
