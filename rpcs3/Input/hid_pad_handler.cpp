#include "hid_pad_handler.h"
#include "ds3_pad_handler.h"
#include "ds4_pad_handler.h"
#include "dualsense_pad_handler.h"
#include "skateboard_pad_handler.h"
#include "ps_move_handler.h"
#include "util/logs.hpp"
#include "Utilities/Timer.h"
#include "Emu/System.h"
#include "pad_thread.h"

#if defined(__APPLE__)
#include "3rdparty/hidapi/hidapi/mac/hidapi_darwin.h"
#endif

#include <algorithm>
#include <memory>

LOG_CHANNEL(hid_log, "HID");

#ifdef ANDROID
std::vector<android_usb_device> g_android_usb_devices;
std::mutex g_android_usb_devices_mutex;
#endif

struct hid_instance
{
public:
	hid_instance() = default;
	~hid_instance()
	{
		std::lock_guard lock(m_hid_mutex);

		// Only exit HIDAPI once on exit. HIDAPI uses a global state internally...
		if (m_initialized)
		{
			hid_log.notice("Exiting HIDAPI...");

			if (hid_exit() != 0)
			{
				hid_log.error("hid_exit failed!");
			}
		}
	}

	static hid_instance& get_instance()
	{
		static hid_instance instance {};
		return instance;
	}

	bool initialize()
	{
		std::lock_guard lock(m_hid_mutex);

		// Only init HIDAPI once. HIDAPI uses a global state internally...
		if (m_initialized)
		{
			return true;
		}

		hid_log.notice("Initializing HIDAPI ...");

		if (int errorCode = hid_init(); errorCode != 0)
		{
			hid_log.fatal("hid_init error %d: %s", errorCode, hid_error(nullptr));
			return false;
		}

		m_initialized = true;
		return true;
	}

private:
	bool m_initialized = false;
	std::mutex m_hid_mutex;
};

void HidDevice::close()
{
	if (hidDevice)
	{
		hid_close(hidDevice);
		hidDevice = nullptr;
	}
#ifdef _WIN32
	if (bt_device)
	{
		hid_close(bt_device);
		bt_device = nullptr;
	}
#endif
}

template <class Device>
hid_pad_handler<Device>::hid_pad_handler(pad_handler type, std::vector<id_pair> ids)
    : PadHandlerBase(type), m_ids(std::move(ids))
{
};

template <class Device>
hid_pad_handler<Device>::~hid_pad_handler()
{
	if (m_enumeration_thread)
	{
		auto& enumeration_thread = *m_enumeration_thread;
		enumeration_thread = thread_state::aborting;
		enumeration_thread();
	}

	for (auto& controller : m_controllers)
	{
		if (controller.second)
		{
			controller.second->close();
		}
	}
}

template <class Device>
bool hid_pad_handler<Device>::Init()
{
	if (m_is_init)
		return true;

	if (!hid_instance::get_instance().initialize())
		return false;

#if defined(__APPLE__)
	hid_darwin_set_open_exclusive(0);
#endif

	for (usz i = 1; i <= MAX_GAMEPADS; i++) // Controllers 1-n in GUI
	{
		m_controllers.emplace(m_name_string + std::to_string(i), std::make_shared<Device>());
	}

	enumerate_devices();
	update_devices();

	m_is_init = true;

	m_enumeration_thread = std::make_unique<named_thread<std::function<void()>>>(fmt::format("%s Enumerator", m_type), [this]()
	{
		while (thread_ctrl::state() != thread_state::aborting)
		{
			if (pad::g_enabled && Emu.IsRunning())
			{
				enumerate_devices();
			}

			thread_ctrl::wait_for(2'000'000);
		}
	});

	return true;
}

template <class Device>
void hid_pad_handler<Device>::process()
{
	update_devices();

	PadHandlerBase::process();
}

template <class Device>
std::vector<pad_list_entry> hid_pad_handler<Device>::list_devices()
{
	std::vector<pad_list_entry> pads_list;

	if (!Init())
		return pads_list;

	for (const auto& controller : m_controllers) // Controllers 1-n in GUI
	{
		pads_list.emplace_back(controller.first, false);
	}

	return pads_list;
}

template <class Device>
void hid_pad_handler<Device>::enumerate_devices()
{
	Timer timer;
	std::set<hid_enumerated_device_type> device_paths;
	std::map<hid_enumerated_device_type, std::wstring> serials;

#ifdef ANDROID
	{
		std::lock_guard lock(g_android_usb_devices_mutex);
		for (auto device : g_android_usb_devices)
		{
			auto filter = [&](id_pair id)
			{
				return id.m_vid == device.vendorId && id.m_pid == device.productId;
			};

			if (std::find_if(m_ids.begin(), m_ids.end(), filter) != m_ids.end())
			{
				device_paths.insert(device.fd);
			}
		}
	}
#else
	for (const auto& [vid, pid] : m_ids)
	{
		hid_device_info* dev_info = hid_enumerate(vid, pid);
		hid_device_info* head     = dev_info;
		while (dev_info)
		{
			if (!dev_info->path)
			{
				hid_log.error("Skipping enumeration of device with empty path.");
				continue;
			}

			const std::string path = dev_info->path;
			device_paths.insert(path);

#ifdef _WIN32
			// Only add serials for col01 ps move device
			if (m_type == pad_handler::move && path.find("&Col01#") != umax)
#endif
			{
				serials[path] = dev_info->serial_number ? std::wstring(dev_info->serial_number) : std::wstring();
			}

			dev_info = dev_info->next;
		}
		hid_free_enumeration(head);
	}
#endif
	hid_log.notice("%s enumeration found %d devices (%f ms)", m_type, device_paths.size(), timer.GetElapsedTimeInMilliSec());

	std::lock_guard lock(m_enumeration_mutex);
	m_new_enumerated_devices = device_paths;
	m_enumerated_serials = std::move(serials);

#ifdef _WIN32
	if (m_type == pad_handler::move)
	{
		// Windows enumerates 3 ps move devices: Col01, Col02, and Col03.
		// We use Col01 for data and Col02 for bluetooth.

		// Filter paths. We only want the Col01 paths.
		std::set<std::string> col01_paths;

		for (const std::string& path : m_new_enumerated_devices)
		{
			hid_log.trace("Found ps move device: %s", path);

			if (path.find("&Col01#") != umax)
			{
				col01_paths.insert(path);
			}
		}

		m_new_enumerated_devices = std::move(col01_paths);
	}
#endif
}

template <class Device>
void hid_pad_handler<Device>::update_devices()
{
	std::lock_guard lock(m_enumeration_mutex);

	if (m_last_enumerated_devices == m_new_enumerated_devices)
	{
		return;
	}

	m_last_enumerated_devices = m_new_enumerated_devices;

	// Scrap devices that are not in the new list
	for (auto& controller : m_controllers)
	{
		if (controller.second && controller.second->path != hid_enumerated_device_default && !m_new_enumerated_devices.contains(controller.second->path))
		{
			controller.second->close();
			cfg_pad* config = controller.second->config;
			controller.second.reset(new Device());
			controller.second->config = config;
		}
	}

	bool warn_about_drivers = false;

	// Find and add new devices
	for (const auto& path : m_new_enumerated_devices)
	{
		// Check if we have at least one virtual controller left
		if (std::none_of(m_controllers.cbegin(), m_controllers.cend(), [](const auto& c) { return !c.second || !c.second->hidDevice; }))
			break;

		// Check if we already have this controller
		if (std::any_of(m_controllers.cbegin(), m_controllers.cend(), [&path](const auto& c) { return c.second && c.second->path == path; }))
			continue;

#ifdef _WIN32
		if (m_type == pad_handler::move)
		{
			check_add_device(nullptr, path, m_enumerated_serials[path]);
			continue;
		}
#endif

#ifdef ANDROID
		if (hid_device* dev = hid_libusb_wrap_sys_device(path, -1))
#else
		if (hid_device* dev = hid_open_path(path.c_str()))
#endif
		{
			if (const hid_device_info* info = hid_get_device_info(dev))
			{
				hid_log.notice("%s adding device: vid=0x%x, pid=0x%x, path='%s'", m_type, info->vendor_id, info->product_id, path);
			}
			else
			{
				hid_log.warning("%s adding device: vid=N/A, pid=N/A, path='%s', error='%s'", m_type, path, hid_error(dev));
			}

			check_add_device(dev, path, m_enumerated_serials[path]);
		}
		else
		{
			hid_log.error("%s hid_open_path failed! error='%s', path='%s'", m_type, hid_error(dev), path);
			warn_about_drivers = true;
		}
	}

	if (warn_about_drivers)
	{
		hid_log.error("One or more %s pads were detected but couldn't be interacted with directly", m_type);
#if defined(_WIN32) || defined(__linux__)
		hid_log.error("Check https://wiki.rpcs3.net/index.php?title=Help:Controller_Configuration for instructions on how to solve this issue");
#endif
	}
	else
	{
		const usz count = std::count_if(m_controllers.cbegin(), m_controllers.cend(), [](const auto& c) { return c.second && c.second->hidDevice; });
		if (count > 0)
		{
			hid_log.success("%s Controllers found: %d", m_type, count);
		}
		else
		{
			hid_log.warning("No %s controllers found!", m_type);
		}
	}
}

template <class Device>
std::shared_ptr<Device> hid_pad_handler<Device>::get_hid_device(const std::string& padId)
{
	if (!Init())
		return nullptr;

	// Controllers 1-n in GUI
	if (auto it = m_controllers.find(padId); it != m_controllers.end())
	{
		return it->second;
	}

	return nullptr;
}

template <class Device>
std::shared_ptr<PadDevice> hid_pad_handler<Device>::get_device(const std::string& device)
{
	return get_hid_device(device);
}

template <class Device>
u32 hid_pad_handler<Device>::get_battery_color(u8 battery_level, u32 brightness)
{
	static constexpr std::array<u32, 12> battery_level_clr = {0xff00, 0xff33, 0xff66, 0xff99, 0xffcc, 0xffff, 0xccff, 0x99ff, 0x66ff, 0x33ff, 0x00ff, 0x00ff};

	const u32 combined_color = battery_level_clr[battery_level < battery_level_clr.size() ? battery_level : 0];

	const u32 red = (combined_color >> 8) * brightness / 100;
	const u32 green = (combined_color & 0xff) * brightness / 100;
	return ((red << 8) | green);
}

template class hid_pad_handler<ds3_device>;
template class hid_pad_handler<DS4Device>;
template class hid_pad_handler<DualSenseDevice>;
template class hid_pad_handler<skateboard_device>;
template class hid_pad_handler<ps_move_device>;
