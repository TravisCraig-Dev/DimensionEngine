#include "Dimension/Core/PlayerPrefs.h"
#include "Dimension/Core/Log.h"
#include "Dimension/Project/Project.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <mutex>

namespace Dimension
{
    namespace
    {
        fs::path s_Folder;
        std::mutex s_Mutex;
        nlohmann::json s_Prefs = nlohmann::json::object();
        fs::path s_LoadedFrom; // the file s_Prefs came from (read again when the folder changes)

        // None without a folder (no project open and none set): kept in memory only.
        fs::path PrefsFile()
        {
            fs::path folder = PlayerData::Folder();
            return folder.empty() ? fs::path() : folder / "PlayerPrefs.json";
        }

        // The prefs of the current folder (with s_Mutex held).
        nlohmann::json& Prefs()
        {
            fs::path file = PrefsFile();
            if (file != s_LoadedFrom)
            {
                s_LoadedFrom = file;
                s_Prefs = nlohmann::json::object();
                std::ifstream in(file);
                if (in)
                {
                    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
                    if (j.is_object()) s_Prefs = std::move(j);
                    else Log::Warn("Player prefs in {} could not be read; starting empty", file.string());
                }
            }
            return s_Prefs;
        }

        void Write()
        {
            fs::path file = PrefsFile();
            if (file.empty()) return;
            std::error_code ec;
            fs::create_directories(file.parent_path(), ec);
            // Written aside first, then swapped in: a crash mid-write keeps the old file.
            fs::path temp = file;
            temp += ".tmp";
            {
                std::ofstream out(temp);
                if (!out) return;
                out << s_Prefs.dump(2);
            }
            fs::rename(temp, file, ec);
            if (ec) Log::Warn("Could not save player prefs to {}: {}", file.string(), ec.message());
        }

        template <typename T>
        void Set(const std::string& key, const T& value)
        {
            std::lock_guard lock(s_Mutex);
            Prefs()[key] = value;
            Write();
        }
    }

    namespace PlayerData
    {
        void SetFolder(const fs::path& folder) { s_Folder = folder; }

        fs::path Folder()
        {
            if (!s_Folder.empty()) return s_Folder;
            return Project::IsOpen() ? Project::GetDirectory() / ".dimension" / "PlayerData" : fs::path();
        }

        std::string FolderName(const std::string& product)
        {
            std::string out;
            for (char c : product)
                if (std::string_view("<>:\"/\\|?*").find(c) == std::string_view::npos && (unsigned char)c >= 32) out += c;
            while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
            // Collapse the doubled spaces left by removed characters.
            std::string tidy;
            for (char c : out)
                if (!(c == ' ' && !tidy.empty() && tidy.back() == ' ')) tidy += c;
            return tidy.empty() ? "Game" : tidy;
        }
    }

    namespace PlayerPrefs
    {
        void SetInt(const std::string& key, int value) { Set(key, value); }
        void SetFloat(const std::string& key, float value) { Set(key, (double)value); }
        void SetString(const std::string& key, const std::string& value) { Set(key, value); }

        int GetInt(const std::string& key, int defaultValue)
        {
            std::lock_guard lock(s_Mutex);
            auto& p = Prefs();
            return p.contains(key) && p[key].is_number_integer() ? p[key].get<int>() : defaultValue;
        }

        float GetFloat(const std::string& key, float defaultValue)
        {
            std::lock_guard lock(s_Mutex);
            auto& p = Prefs();
            return p.contains(key) && p[key].is_number_float() ? p[key].get<float>() : defaultValue;
        }

        std::string GetString(const std::string& key, const std::string& defaultValue)
        {
            std::lock_guard lock(s_Mutex);
            auto& p = Prefs();
            return p.contains(key) && p[key].is_string() ? p[key].get<std::string>() : defaultValue;
        }

        bool HasKey(const std::string& key)
        {
            std::lock_guard lock(s_Mutex);
            return Prefs().contains(key);
        }

        void DeleteKey(const std::string& key)
        {
            std::lock_guard lock(s_Mutex);
            if (Prefs().erase(key)) Write();
        }

        void DeleteAll()
        {
            std::lock_guard lock(s_Mutex);
            Prefs() = nlohmann::json::object();
            Write();
        }

        void Save()
        {
            std::lock_guard lock(s_Mutex);
            Prefs();
            Write();
        }

        void Reload()
        {
            std::lock_guard lock(s_Mutex);
            s_LoadedFrom.clear();
            Prefs();
        }
    }
}
