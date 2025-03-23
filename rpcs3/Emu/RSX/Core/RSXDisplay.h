#pragma once

#include <util/types.hpp>
#include <util/logs.hpp>
#include <deque>
#include <unordered_map>

template <typename T>
class named_thread;

namespace rsx
{
	enum class surface_antialiasing : u8;

	struct framebuffer_dimensions_t
	{
		u16 width;
		u16 height;
		u8 samples_x;
		u8 samples_y;

		inline u32 samples_total() const
		{
			return static_cast<u32>(width) * height * samples_x * samples_y;
		}

		inline bool operator > (const framebuffer_dimensions_t& that) const
		{
			return samples_total() > that.samples_total();
		}

		std::string to_string(bool skip_aa_suffix = false) const;

		static framebuffer_dimensions_t make(u16 width, u16 height, rsx::surface_antialiasing aa);
	};

	struct framebuffer_statistics_t
	{
		std::unordered_map<rsx::surface_antialiasing, framebuffer_dimensions_t> data;

		// Replace the existing data with this input if it is greater than what is already known
		void add(u16 width, u16 height, rsx::surface_antialiasing aa);

		// Returns a formatted string representing the statistics collected over the frame.
		std::string to_string(bool squash) const;
	};

	struct frame_statistics_t
	{
		u32 draw_calls;                      // Total number of draw groups requested by RSX
		u32 submit_count;                    // Total number of CPU -> GPU submits for recording APIs (e.g vulkan, d3d12)
		u32 instanced_draws;                 // Total number of draws that were compiled into instanced emits
		u32 instanced_groups;                // Number of instance groups encountered.

		s64 setup_time;                      // Time to prepare for a draw. Includes time used to load shaders and prepare render targets.
		s64 vertex_upload_time;              // Time spent uploading vertex and index data.
		s64 textures_upload_time;            // Time spend uploading textures
		s64 draw_exec_time;                  // Time spent in draw call execution logic. Includes emitting the draw command, setting up renderpasses, etc
		s64 flip_time;                       // Time spent in the flip logic

		u32 vertex_cache_request_count;      // Vertex cache lookups total.
		u32 vertex_cache_miss_count;         // Vertex cache misses.

		u32 program_cache_lookups_total;     // Number of times the program caches were queried for a shader. Counts vertex and fragment lookup separately and does not include queries to the linked program cache.
		u32 program_cache_lookups_ellided;   // Number of times a lookup was skipped because we already knew the previous shader was being reused. Higher number means better cache usage and leads to more performance.

		framebuffer_statistics_t framebuffer_stats; // Summarized framebuffer stats, used to guess the internal rendering resolution.
	};

	struct frame_time_t
	{
		u64 preempt_count;
		u64 timestamp;
		u64 tsc;
	};

	struct display_flip_info_t
	{
		std::deque<u32> buffer_queue;
		u32 buffer;
		bool skip_frame;
		bool emu_flip;
		bool in_progress;
		frame_statistics_t stats;

		inline void push(u32 _buffer)
		{
			buffer_queue.push_back(_buffer);
		}

		inline bool pop(u32 _buffer)
		{
			if (buffer_queue.empty())
			{
				return false;
			}

			do
			{
				const auto index = buffer_queue.front();
				buffer_queue.pop_front();

				if (index == _buffer)
				{
					buffer = _buffer;
					return true;
				}
			} while (!buffer_queue.empty());

			// Need to observe this happening in the wild
			rsx_log.error("Display queue was discarded while not empty!");
			return false;
		}
	};

	class vblank_thread
	{
		std::shared_ptr<named_thread<std::function<void()>>> m_thread;

	public:
		vblank_thread() = default;
		vblank_thread(const vblank_thread&) = delete;

		void set_thread(std::shared_ptr<named_thread<std::function<void()>>> thread);

		vblank_thread& operator=(thread_state);
		vblank_thread& operator=(const vblank_thread&) = delete;
	};
}
