//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================
#include "config_manager.h"

#include <monitoring/monitoring.h>
#include <sys/utsname.h>

#include <iostream>

#include "config_converter.h"
#include "config_logger_loader.h"
#include "config_private.h"
#include "items/items.h"

namespace cfg
{
	struct XmlWriter : pugi::xml_writer
	{
		ov::String result;

		void write(const void *data, size_t size) override
		{
			result.Append(static_cast<const char *>(data), size);
		}
	};

	ConfigManager::ConfigManager()
	{
		// Modify if supported xml version is added or changed

		// Current OME compatible with v8 & v9
		_supported_versions_map["Server"] = {8, 9};
		_supported_versions_map["Logger"] = {2};
	}

	ConfigManager::~ConfigManager()
	{
	}

	void ConfigManager::SetOmeVersion(const ov::String &version, const ov::String &git_extra)
	{
		_version = version;
		_git_extra = git_extra;
	}

	void ConfigManager::CheckLegacyConfigs(ov::String config_path)
	{
		// LastConfig was used <= 0.12.10, but later version changed to <API><Storage>.
		// Inform the user that LastConfig is no longer used and throws an exception so that OME can be terminated.
		bool is_last_config_found = ov::PathManager::IsFile(ov::PathManager::Combine(config_path, CFG_LAST_CONFIG_FILE_NAME));
		bool is_legacy_last_config_found = ov::PathManager::IsFile(ov::PathManager::Combine(config_path, CFG_LAST_CONFIG_FILE_NAME_LEGACY));

		if (is_last_config_found || is_legacy_last_config_found)
		{
			throw CreateConfigError("Legacy config file found. Please migrate '%s' manually or delete it and run OME again.", is_last_config_found ? CFG_LAST_CONFIG_FILE_NAME : CFG_LAST_CONFIG_FILE_NAME_LEGACY);
		}
	}

	void ConfigManager::LoadConfigs(ov::String config_path)
	{
		if (config_path.IsEmpty())
		{
			// Default: <OME_HOME>/conf
			config_path = ov::PathManager::GetAppPath("conf");
		}

		CheckLegacyConfigs(config_path);

		LoadLoggerConfig(config_path);
		LoadServerConfig(config_path);

		_config_path = config_path;

		LoadServerID(config_path);
		_server->SetID(_server_id);
	}

	void ConfigManager::ReloadConfigs()
	{
		LoadConfigs(_config_path);
	}

	Json::Value ConfigManager::GetCurrentConfigAsJson() const
	{
		auto lock_guard = std::lock_guard(_config_mutex);

		auto config = serdes::GetServerJsonFromConfig(_server, false);

		return config;
	}

	pugi::xml_document ConfigManager::GetCurrentConfigAsXml() const
	{
		auto lock_guard = std::lock_guard(_config_mutex);

		auto config = serdes::GetServerXmlFromConfig(_server, false);

		return config;
	}

	bool ConfigManager::SaveCurrentConfig(pugi::xml_document &config, const ov::String &last_config_path)
	{
		auto comment_node = config.prepend_child(pugi::node_comment);
		ov::String comment;

		utsname uts{};
		::uname(&uts);

#if DEBUG
		static constexpr const char *BUILD_MODE = " [debug]";
#else	// DEBUG
		static constexpr const char *BUILD_MODE = "";
#endif	// DEBUG

		comment.Format(
			"\n"
			"\tThis is an auto-generated configuration file through API call.\n"
			"\tOvenMediaEngine may not work if it is modified incorrectly.\n"
			"\tYou can use '-i' option to prevent loading this file when the OME launches.\n\n"
			"\tVersion: v%s%s%s\n"
			"\tCreated: %s\n"
			"\tHost: %s (%s %s - %s, %s)\n",
			_version.CStr(), _git_extra.CStr(), BUILD_MODE,
			ov::Time::MakeUtcMillisecond().CStr(), uts.nodename, uts.sysname, uts.machine, uts.release, uts.version);

		comment_node.set_value(comment);

		auto declaration = config.prepend_child(pugi::node_declaration);
		declaration.append_attribute("version") = "1.0";
		declaration.append_attribute("encoding") = "utf-8";

		XmlWriter writer;
		config.print(writer);

		auto file = ov::DumpToFile(last_config_path, writer.result.CStr(), writer.result.GetLength());

		if (file == nullptr)
		{
			logte("Could not write config to file: %s", last_config_path.CStr());
			return false;
		}

		logti("Current config is written to %s", last_config_path.CStr());

		return true;
	}

	bool ConfigManager::SaveCurrentConfig()
	{
		auto config = GetCurrentConfigAsXml();
		ov::String last_config_path = ov::PathManager::Combine(_config_path, CFG_LAST_CONFIG_FILE_NAME);

		return SaveCurrentConfig(config, last_config_path);
	}

	void ConfigManager::LoadServerID(const ov::String &config_path)
	{
		{
			auto [result, server_id] = LoadServerIDFromStorage(config_path);
			if (result == true)
			{
				_server_id = server_id;
				return;
			}
		}

		{
			auto [result, server_id] = GenerateServerID();
			if (result == true)
			{
				_server_id = server_id;
				StoreServerID(config_path, server_id);
				return;
			}
		}
	}

	std::tuple<bool, ov::String> ConfigManager::LoadServerIDFromStorage(const ov::String &config_path) const
	{
		// If node id is empty, try to load ID from file
		auto node_id_storage = ov::PathManager::Combine(config_path, SERVER_ID_STORAGE_FILE);

		std::ifstream fs(node_id_storage);
		if (!fs.is_open())
		{
			return {false, ""};
		}

		std::string line;
		std::getline(fs, line);
		fs.close();

		line.erase(std::remove(line.begin(), line.end(), '\n'), line.end());

		return {true, line.c_str()};
	}

	bool ConfigManager::StoreServerID(const ov::String &config_path, ov::String server_id)
	{
		// Store server_id to storage
		auto node_id_storage = ov::PathManager::Combine(config_path, SERVER_ID_STORAGE_FILE);

		std::ofstream fs(node_id_storage);
		if (!fs.is_open())
		{
			return false;
		}

		fs.write(server_id.CStr(), server_id.GetLength());
		fs.close();
		return true;
	}

	std::tuple<bool, ov::String> ConfigManager::GenerateServerID() const
	{
		return {true, ov::UUID::Generate()};
	}

	void ConfigManager::LoadLoggerConfig(const ov::String &config_path)
	{
		struct stat value = {0};

		ov::String logger_config_path = ov::PathManager::Combine(config_path, CFG_LOG_FILE_NAME);

		::memset(&_last_modified, 0, sizeof(_last_modified));
		if (::stat(logger_config_path, &value) == -1)
		{
			// There is no file or to open file error
			// OME will work with the default settings.
			logtw("There is no configuration file for logs : %s. OME will run with the default settings.", logger_config_path.CStr());
			return;
		}

		if (
#if defined(__APPLE__)
			(_last_modified.tv_sec == value.st_mtimespec.tv_sec) &&
			(_last_modified.tv_nsec == value.st_mtimespec.tv_nsec)
#else
			(_last_modified.tv_sec == value.st_mtim.tv_sec) &&
			(_last_modified.tv_nsec == value.st_mtim.tv_nsec)
#endif
		)
		{
			// File is not changed
			return;
		}

		::ov_log_reset_enable();

#if defined(__APPLE__)
		_last_modified = value.st_mtimespec;
#else
		_last_modified = value.st_mtim;
#endif

		auto logger_loader = std::make_shared<ConfigLoggerLoader>(logger_config_path);

		logger_loader->Parse();

		CheckValidVersion("Logger", ov::Converter::ToInt32(logger_loader->GetVersion()));

		auto log_path = logger_loader->GetLogPath();
		::ov_log_set_path(log_path.CStr());

		// For event logger
		MonitorInstance->SetLogPath(log_path.CStr());

		// Init stat log
		//TODO(Getroot): This is temporary code for testing. This will change to more elegant code in the future.
		::ov_stat_log_set_path(STAT_LOG_WEBRTC_EDGE_SESSION, log_path.CStr());
		::ov_stat_log_set_path(STAT_LOG_WEBRTC_EDGE_REQUEST, log_path.CStr());
		::ov_stat_log_set_path(STAT_LOG_WEBRTC_EDGE_VIEWERS, log_path.CStr());
		::ov_stat_log_set_path(STAT_LOG_HLS_EDGE_SESSION, log_path.CStr());
		::ov_stat_log_set_path(STAT_LOG_HLS_EDGE_REQUEST, log_path.CStr());
		::ov_stat_log_set_path(STAT_LOG_HLS_EDGE_VIEWERS, log_path.CStr());

		logti("Trying to set logfile in directory... (%s)", log_path.CStr());

		std::vector<std::shared_ptr<LoggerTagInfo>> tags = logger_loader->GetTags();

		for (auto iterator = tags.begin(); iterator != tags.end(); ++iterator)
		{
			auto name = (*iterator)->GetName();

			if (::ov_log_set_enable(name.CStr(), (*iterator)->GetLevel(), true) == false)
			{
				throw CreateConfigError("Could not set log level for tag: %s", name.CStr());
			}
		}

		logger_loader->Reset();
	}

	void ConfigManager::LoadServerConfig(const ov::String &config_path)
	{
		const char *XML_ROOT_NAME = "Server";
		ov::String server_config_path = ov::PathManager::Combine(config_path, CFG_MAIN_FILE_NAME);

		logti("Trying to load configurations... (%s)", server_config_path.CStr());
		DataSource data_source(DataType::Xml, config_path, CFG_MAIN_FILE_NAME, XML_ROOT_NAME);
		_server = std::make_shared<Server>();
		_server->FromDataSource(XML_ROOT_NAME, data_source);

		CheckValidVersion(XML_ROOT_NAME, ov::Converter::ToInt32(_server->GetVersion()));
	}

	void ConfigManager::CheckValidVersion(const ov::String &name, int version)
	{
		auto versions_iterator = _supported_versions_map.find(name);

		if (versions_iterator == _supported_versions_map.end())
		{
			throw CreateConfigError("Cannot find conf XML (%s.xml)", name.CStr());
		}

		auto supported_versions = versions_iterator->second;

		if (version == 0)
		{
			throw CreateConfigError(
				"Could not obtain version in your XML. If you have upgraded OME, see misc/conf_examples/%s.xml",
				name.CStr());
		}

		auto version_iterator = std::find(supported_versions.begin(), supported_versions.end(), version);

		if (version_iterator == supported_versions.end())
		{
			ov::String description;

			description.Format(
				"The version of %s.xml is outdated (Your XML version: %d, Latest version: %d).\n",
				name.CStr(), version, supported_versions);

			description.AppendFormat(
				"If you have upgraded OME, see misc/conf_examples/%s.xml\n",
				name.CStr());

			if (version <= 7)
			{
				description.AppendFormat("Major Changes (v7 -> v8):\n");
				description.AppendFormat(" - Added <Server>.<Bind>.<Managers>.<API> for setting API binding port\n");
				description.AppendFormat(" - Added <Server>.<API> for setting API server\n");
				description.AppendFormat(" - Added <Server>.<VirtualHosts>.<VirtualHost>.<Applications>.<Application>.<OutputProfiles>\n");
				description.AppendFormat(" - Changed <Server>.<VirtualHosts>.<VirtualHost>.<Domain> to <Host>\n");
				description.AppendFormat(" - Changed <CrossDomain> to <CrossDomains>\n");
				description.AppendFormat(" - Deleted <Server>.<VirtualHosts>.<VirtualHost>.<Applications>.<Application>.<Streams>\n");
				description.AppendFormat(" - Deleted <Server>.<VirtualHosts>.<VirtualHost>.<Applications>.<Application>.<Encodes>\n");
			}

			if (version <= 8)
			{
				description.AppendFormat("Major Changes (v8 -> v9):\n");
				description.AppendFormat(" - Added <Server>.<Bind>.<Managers>.<API>.<Storage> to store configs created using API\n");
			}

			throw CreateConfigError("%s", description.CStr());
		}
	}
}  // namespace cfg