#include "stdafx.h"
#include "Utilities/Config.h"
#include "Emu/System.h"

#include "VFS.h"

#include <regex>
#include "Utilities/StrUtil.h"

cfg::string_entry g_cfg_vfs_emulator_dir(cfg::root.vfs, "$(EmulatorDir)"); // Default (empty): taken from fs::get_executable_dir()
cfg::string_entry g_cfg_vfs_dev_hdd0(cfg::root.vfs, "/dev_hdd0/", "$(EmulatorDir)dev_hdd0/");
cfg::string_entry g_cfg_vfs_dev_hdd1(cfg::root.vfs, "/dev_hdd1/", "$(EmulatorDir)dev_hdd1/");
cfg::string_entry g_cfg_vfs_dev_flash(cfg::root.vfs, "/dev_flash/", "$(EmulatorDir)dev_flash/");
cfg::string_entry g_cfg_vfs_dev_usb000(cfg::root.vfs, "/dev_usb000/", "$(EmulatorDir)dev_usb000/");
cfg::string_entry g_cfg_vfs_dev_bdvd(cfg::root.vfs, "/dev_bdvd/"); // Not mounted
cfg::string_entry g_cfg_vfs_app_home(cfg::root.vfs, "/app_home/"); // Not mounted

cfg::bool_entry g_cfg_vfs_allow_host_root(cfg::root.vfs, "Enable /host_root/", true);

const std::regex s_regex_host_root("^/+host_root/(.*)", std::regex::optimize);
const std::regex s_regex_dev_hdd0("^/+dev_hdd0(?:$|/+)(.*)", std::regex::optimize);
const std::regex s_regex_dev_hdd1("^/+dev_hdd1(?:$|/+)(.*)", std::regex::optimize);
const std::regex s_regex_dev_flash("^/+dev_flash(?:$|/+)(.*)", std::regex::optimize);
const std::regex s_regex_dev_usb000("^/+dev_usb000(?:$|/+)(.*)", std::regex::optimize);
const std::regex s_regex_dev_usb("^/+dev_usb(?:$|/+)(.*)", std::regex::optimize);
const std::regex s_regex_dev_bdvd("^/+dev_bdvd(?:$|/+)(.*)", std::regex::optimize);
const std::regex s_regex_app_home("^/+app_home(?:$|/+)(.*)", std::regex::optimize);

const std::regex s_regex_emu_dir("\\$\\(EmulatorDir\\)");

void vfs::dump()
{
	LOG_NOTICE(LOADER, "Mount info:");
	LOG_NOTICE(LOADER, "/dev_hdd0/ -> %s", g_cfg_vfs_dev_hdd0.get());
	LOG_NOTICE(LOADER, "/dev_hdd1/ -> %s", g_cfg_vfs_dev_hdd1.get());
	LOG_NOTICE(LOADER, "/dev_flash/ -> %s", g_cfg_vfs_dev_flash.get());
	LOG_NOTICE(LOADER, "/dev_usb/ -> %s", g_cfg_vfs_dev_usb000.get());
	LOG_NOTICE(LOADER, "/dev_usb000/ -> %s", g_cfg_vfs_dev_usb000.get());
	if (g_cfg_vfs_dev_bdvd.size()) LOG_NOTICE(LOADER, "/dev_bdvd/ -> %s", g_cfg_vfs_dev_bdvd.get());
	if (g_cfg_vfs_app_home.size()) LOG_NOTICE(LOADER, "/app_home/ -> %s", g_cfg_vfs_app_home.get());
	if (g_cfg_vfs_allow_host_root) LOG_NOTICE(LOADER, "/host_root/ -> .");
	LOG_NOTICE(LOADER, "");
}

std::string vfs::get(const std::string& vpath)
{
	std::smatch match;
	const cfg::string_entry* vdir = nullptr;

	// Accessing host FS directly
	if (g_cfg_vfs_allow_host_root && std::regex_match(vpath, match, s_regex_host_root))
	{
		return match[1];
	}

	else if (std::regex_match(vpath, match, s_regex_dev_bdvd))
		vdir = &g_cfg_vfs_dev_bdvd;
	else if (std::regex_match(vpath, match, s_regex_dev_hdd0))
		vdir = &g_cfg_vfs_dev_hdd0;
	else if (std::regex_match(vpath, match, s_regex_dev_hdd1))
		vdir = &g_cfg_vfs_dev_hdd1;
	else if (std::regex_match(vpath, match, s_regex_app_home))
		vdir = &g_cfg_vfs_app_home;
	else if (std::regex_match(vpath, match, s_regex_dev_flash))
		vdir = &g_cfg_vfs_dev_flash;
	else if (std::regex_match(vpath, match, s_regex_dev_usb000))
		vdir = &g_cfg_vfs_dev_usb000;
	else if (std::regex_match(vpath, match, s_regex_dev_usb))
		vdir = &g_cfg_vfs_dev_usb000;

	// Return empty path if not mounted
	else
	{
		LOG_WARNING(GENERAL, "vfs::get() failed for %s", vpath);
		return{};
	}

	// Replace $(EmulatorDir), concatenate
	return fmt::replace_all(*vdir, "$(EmulatorDir)", g_cfg_vfs_emulator_dir.size() == 0 ? fs::get_executable_dir() : g_cfg_vfs_emulator_dir) + match.str(1);
}
