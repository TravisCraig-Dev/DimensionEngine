#pragma once
#include "EditorContext.h"
#include <Dimension/Code/TextBuffer.h>
#include <Dimension/Code/CSharpLexer.h>
#include <imgui.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Editor
{
    // Code Mode: a full-screen script workspace (like an IDE's) in place of the editor's windows: the project's C#
    // scripts in a tree, open files in tabs, and a code editor (syntax colouring, auto-indent, brackets, undo, find and
    // replace). Saving a script rebuilds the scripts as an outside edit would (the editor's C# build).
    class CodeMode
    {
    public:
        explicit CodeMode(EditorContext& ctx) : m_Ctx(ctx) {}

        bool IsActive() const { return m_Active; }
        // Into Code Mode, with `file` open (else the files open last time); false when `file` cannot be read.
        bool Open(const fs::path& file = {});
        // Saves everything and goes back to the editor.
        void Close();
        bool SaveAll();
        // The whole window (call each frame while active, instead of the editor's windows).
        void OnImGui();

        // Set by the editor: build the scripts now, and how the last build went (0 none, 1 building, 2 ok, 3 failed).
        std::function<void()> BuildScripts;
        std::function<int()> BuildState;

        // An open file.
        struct Document
        {
            fs::path Path;
            Dimension::Code::TextBuffer Buffer;
            fs::file_time_type DiskTime{};
            bool ChangedOnDisk = false; // changed outside while it has unsaved edits here
            // Lexer states at each line's start, for the buffer version they were made for.
            std::vector<Dimension::Code::LexState> States;
            uint64_t StatesVersion = 0;
            int LongestLine = 0; // in columns
            bool ScrollToCursor = false;
            float ScrollY = 0.0f, ScrollX = 0.0f;
        };
        // Automation and tests: the file being edited (null with none open).
        Document* Current() { return m_Current >= 0 && m_Current < (int)m_Docs.size() ? m_Docs[(size_t)m_Current].get() : nullptr; }

    private:
        void DrawTopBar();
        void DrawExplorer(const ImVec2& size);
        void DrawTree(const fs::path& dir);
        void DrawDocuments(const ImVec2& size);
        void DrawCode(Document& doc, const ImVec2& size);
        void DrawFindBar(Document& doc);
        void DrawStatusBar();
        void DrawPopups();
        void HandleKeys(Document& doc, int pageLines);
        void TypeText(Document& doc, const std::string& text);
        void NewLine(Document& doc);
        void FindNext(Document& doc, bool backwards);
        void UpdateStates(Document& doc);

        bool OpenFile(const fs::path& file);
        bool Save(Document& doc);
        void CloseTab(int index);
        void ReloadFromDisk(Document& doc);
        void CheckDisk(float dt);
        void SaveSession();
        void LoadSession();
        fs::path ScriptsDir() const;

        EditorContext& m_Ctx;
        bool m_Active = false;
        std::vector<std::unique_ptr<Document>> m_Docs;
        int m_Current = -1;
        int m_SelectTab = -1;     // the tab to bring forward next frame
        bool m_FocusCode = false; // keyboard to the code view next frame
        float m_DiskTimer = 0.0f;
        double m_BlinkStart = 0.0;
        float m_Zoom = 1.0f;
        bool m_Dragging = false;
        bool m_DragLines = false; // dragging in the line numbers (whole lines)
        int m_DragFirstLine = 0;

        // Find and replace.
        bool m_FindOpen = false, m_ReplaceOpen = false, m_FocusFind = false;
        std::string m_Find, m_Replace;
        Dimension::Code::TextBuffer::FindOptions m_FindOptions;
        std::string m_FindMessage;

        // Popups: a tab's unsaved changes, new script/folder, rename, delete.
        int m_ConfirmClose = -1;
        enum class FileAction
        {
            None, NewScript, NewFolder, Rename, Delete
        };
        FileAction m_FileAction = FileAction::None;
        fs::path m_ActionPath; // the folder for new ones, the file or folder to rename or delete
        std::string m_ActionName;
        bool m_OpenActionPopup = false;
    };
}
