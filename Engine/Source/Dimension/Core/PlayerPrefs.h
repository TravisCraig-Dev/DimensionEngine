#pragma once
#include "Dimension/Core/Base.h"
#include <string>

namespace Dimension
{
    // Where a game keeps the player's data (Unity's Application.persistentDataPath): a build uses
    // %APPDATA%/<product name> (the Player sets it); the editor, without a folder set, the open project's
    // .dimension/PlayerData, so trying a game in the editor never touches a build's saves.
    namespace PlayerData
    {
        void SetFolder(const fs::path& folder); // empty: back to the editor's default
        fs::path Folder();
        // A product name made safe as a folder name ("My Game: Deluxe" -> "My Game Deluxe").
        std::string FolderName(const std::string& product);
    }

    // Small settings the player keeps between runs (like Unity's PlayerPrefs): ints, floats and strings by key, in
    // PlayerData's folder (PlayerPrefs.json), written whenever one changes. Reading a key of another type, or one not
    // set, gives the default.
    namespace PlayerPrefs
    {
        void SetInt(const std::string& key, int value);
        void SetFloat(const std::string& key, float value);
        void SetString(const std::string& key, const std::string& value);
        int GetInt(const std::string& key, int defaultValue = 0);
        float GetFloat(const std::string& key, float defaultValue = 0.0f);
        std::string GetString(const std::string& key, const std::string& defaultValue = {});
        bool HasKey(const std::string& key);
        void DeleteKey(const std::string& key);
        void DeleteAll();
        // Written already on each change; Save writes again (and Reload reads the file again, e.g. after it changed).
        void Save();
        void Reload();
    }
}
