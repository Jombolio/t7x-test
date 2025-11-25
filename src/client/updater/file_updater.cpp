#include <std_include.hpp>

#include "updater.hpp"
#include "updater_ui.hpp"
#include "file_updater.hpp"

#include <utils/cryptography.hpp>
#include <utils/flags.hpp>
#include <utils/http.hpp>
#include <utils/io.hpp>
#include <utils/compression.hpp>

#define UPDATE_SERVER "https://jombo.uk/t7x/"

#define UPDATE_FILE_MAIN UPDATE_SERVER "files.json"
#define UPDATE_FOLDER_MAIN UPDATE_SERVER

#define UPDATE_HOST_BINARY "boiii.exe"

namespace updater
{
	namespace
	{
		std::string get_update_file()
		{
			return UPDATE_FILE_MAIN;
		}

		std::string get_update_folder()
		{
			return UPDATE_FOLDER_MAIN;
		}

		std::string get_cache_buster()
		{
			return "?" + std::to_string(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::system_clock::now().time_since_epoch()).count());
		}

		std::vector<file_info> get_file_infos()
		{
			const auto data = utils::http::get_data(get_update_file() + get_cache_buster());
			if (!data)
			{
				return {};
			}

			std::string hash = *data;
			// Trim whitespace
			hash.erase(hash.find_last_not_of(" \n\r\t") + 1);
			hash.erase(0, hash.find_first_not_of(" \n\r\t"));

			if (hash.empty())
			{
				return {};
			}

			file_info info{};
			info.name = UPDATE_HOST_BINARY;
			info.size = 0; // Size check skipped for simplified update
			info.hash = hash;

			return { info };
		}

		std::string get_hash(const std::string& data)
		{
			return utils::cryptography::md5::compute(data, true);
		}

		const file_info* find_host_file_info(const std::vector<file_info>& outdated_files)
		{
			for (const auto& file : outdated_files)
			{
				if (file.name == UPDATE_HOST_BINARY)
				{
					return &file;
				}
			}

			return nullptr;
		}

		size_t get_optimal_concurrent_download_count(const size_t file_count)
		{
			size_t cores = std::thread::hardware_concurrency();
			cores = (cores * 2) / 3;
			return std::max(1ull, std::min(cores, file_count));
		}

		bool is_inside_folder(const std::filesystem::path& file, const std::filesystem::path& folder)
		{
			const auto relative = std::filesystem::relative(file, folder);
			const auto start = relative.begin();
			return start != relative.end() && start->string() != "..";
		}
	}

	file_updater::file_updater(progress_listener& listener, std::filesystem::path base,
		std::filesystem::path process_file)
		: listener_(listener)
		, base_(std::move(base))
		, process_file_(std::move(process_file))
		, dead_process_file_(process_file_)
	{
		this->dead_process_file_.replace_extension(".exe.old");
		this->delete_old_process_file();
	}

	void file_updater::create_config_file_if_not_exists() const //ugly fix for t7x ext dll - creates a empty file
	{
		TCHAR appDataPath[MAX_PATH];
		DWORD result = GetEnvironmentVariable(TEXT("LOCALAPPDATA"), appDataPath, MAX_PATH);

		if (result > 0 && result < MAX_PATH)
		{
			const std::filesystem::path configFilePath = std::filesystem::path(appDataPath) / "Activision" / "CoD" / "config.ini";

			if (!std::filesystem::exists(configFilePath))
			{
				std::filesystem::create_directories(configFilePath.parent_path());
				std::ofstream configFile(configFilePath);
			}
		}
	}

	void file_updater::run() const
	{
		this->create_config_file_if_not_exists();

		// Ensure main.html exists
		const auto main_html_path = this->base_ / "data/launcher/main.html";
		if (!utils::io::file_exists(main_html_path))
		{
			file_info main_html_info{};
			main_html_info.name = "data/launcher/main.html";
			main_html_info.size = 0;
			main_html_info.hash = ""; // Skip hash check

			try
			{
				this->update_file(main_html_info);
			}
			catch (...)
			{
				// Ignore failure to download main.html, main.cpp will handle the error if it's missing
			}
		}

		const auto files = get_file_infos();
		if (files.empty())
		{
			return;
		}

		const auto outdated_files = this->get_outdated_files(files);
		if (outdated_files.empty())
		{
			return;
		}

		this->update_host_binary(outdated_files);
	}

	void file_updater::update_file(const file_info& file) const
	{
		// For main.html, we don't have a hash query param
		std::string url = get_update_folder() + file.name;
		if (!file.hash.empty())
		{
			url += "?" + file.hash;
		}

		const auto data = utils::http::get_data(url, {}, [&](const size_t progress)
			{
				this->listener_.file_progress(file, progress);
			});

		if (!data)
		{
			throw std::runtime_error("Failed to download: " + url);
		}

		if (!file.hash.empty())
		{
			if (file.size != 0 && data->size() != file.size)
			{
				throw std::runtime_error("Size mismatch: " + url);
			}

			if (get_hash(*data) != file.hash)
			{
				throw std::runtime_error("Hash mismatch: " + url);
			}
		}

		const auto out_file = this->get_drive_filename(file);
		if (!utils::io::write_file(out_file, *data, false))
		{
			throw std::runtime_error("Failed to write: " + file.name);
		}
	}

	std::vector<file_info> file_updater::get_outdated_files(const std::vector<file_info>& files) const
	{
		std::vector<file_info> outdated_files{};

		for (const auto& info : files)
		{
			if (this->is_outdated_file(info))
			{
				outdated_files.emplace_back(info);
			}
		}

		return outdated_files;
	}

	void file_updater::update_host_binary(const std::vector<file_info>& outdated_files) const
	{
		const auto* host_file = find_host_file_info(outdated_files);
		if (!host_file)
		{
			return;
		}

		try
		{
			this->move_current_process_file();
			this->update_files({ *host_file });
		}
		catch (...)
		{
			this->restore_current_process_file();
			throw;
		}

		if (!utils::flags::has_flag("norelaunch"))
		{
			utils::nt::relaunch_self();
		}

		throw update_cancelled();
	}

	void file_updater::update_files(const std::vector<file_info>& outdated_files) const
	{
		this->listener_.update_files(outdated_files);

		const auto thread_count = get_optimal_concurrent_download_count(outdated_files.size());

		std::vector<std::thread> threads{};
		std::atomic<size_t> current_index{ 0 };

		utils::concurrency::container<std::exception_ptr> exception{};

		for (size_t i = 0; i < thread_count; ++i)
		{
			threads.emplace_back([&]()
				{
					while (!exception.access<bool>([](const std::exception_ptr& ptr)
						{
							return static_cast<bool>(ptr);
						}))
					{
						const auto index = current_index++;
						if (index >= outdated_files.size())
						{
							break;
						}

						try
						{
							const auto& file = outdated_files[index];
							this->listener_.begin_file(file);
							this->update_file(file);
							this->listener_.end_file(file);
						}
						catch (...)
						{
							exception.access([](std::exception_ptr& ptr)
								{
									ptr = std::current_exception();
								});

							return;
						}
					}
				});
		}

		for (auto& thread : threads)
		{
			if (thread.joinable())
			{
				thread.join();
			}
		}

		exception.access([](const std::exception_ptr& ptr)
			{
				if (ptr)
				{
					std::rethrow_exception(ptr);
				}
			});

		this->listener_.done_update();
	}

	bool file_updater::is_outdated_file(const file_info& file) const
	{
#if !defined(NDEBUG) || !defined(CI)
		if (file.name == UPDATE_HOST_BINARY && !utils::flags::has_flag("update"))
		{
			return false;
		}
#endif

		std::string data{};
		const auto drive_name = this->get_drive_filename(file);
		if (!utils::io::read_file(drive_name, &data))
		{
			return true;
		}

		if (file.size != 0 && data.size() != file.size)
		{
			return true;
		}

		const auto hash = get_hash(data);
		return hash != file.hash;
	}

	std::filesystem::path file_updater::get_drive_filename(const file_info& file) const
	{
		if (file.name == UPDATE_HOST_BINARY)
		{
			return this->process_file_;
		}

		return this->base_ / file.name;
	}

	void file_updater::move_current_process_file() const
	{
		utils::io::move_file(this->process_file_, this->dead_process_file_);
	}

	void file_updater::restore_current_process_file() const
	{
		utils::io::move_file(this->dead_process_file_, this->process_file_);
	}

	void file_updater::delete_old_process_file() const
	{
		// Wait for other process to die
		for (auto i = 0; i < 4; ++i)
		{
			utils::io::remove_file(this->dead_process_file_);
			if (!utils::io::file_exists(this->dead_process_file_))
			{
				break;
			}

			std::this_thread::sleep_for(2s);
		}
	}

	void file_updater::cleanup_directories(const std::vector<file_info>& files) const
	{
		// Cleanup disabled for simplified update
	}

	void file_updater::cleanup_root_directory(const std::vector<file_info>& files) const
	{
	}

	void file_updater::cleanup_data_directory(const std::vector<file_info>& files) const
	{
	}
}
