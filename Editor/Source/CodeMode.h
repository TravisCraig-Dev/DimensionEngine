#pragma once
#include "EditorContext.h"
#include <Dimension/Code/TextBuffer.h>
#include <Dimension/Code/CSharpLexer.h>
#include <Dimension/Code/BuildDiagnostics.h>
#include <Dimension/Scripting/LanguageService.h>
#include <imgui.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Editor
{
    // Code Mode: a full-screen script workspace (like an IDE's) in place of the editor's windows: the project's C#
    // scripts in a tree, open files in tabs, and a code editor (syntax colouring, auto-indent, brackets, undo, find and
    // replace). Saving a script rebuilds the scripts as an outside edit would (the editor's C# build); what the build
    // reports is underlined in the files and listed under them (Problems).
    class CodeMode
    {
    public:
        explicit CodeMode(EditorContext& ctx) : m_Ctx(ctx) {}
        ~CodeMode();

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
        // The last build's errors and warnings: listed, and underlined in the files (the list opens when it failed).
        void SetBuildResult(std::vector<Dimension::Code::Diagnostic> diagnostics, bool succeeded);

        struct Problem
        {
            Dimension::Code::Diagnostic D; // (its place is kept up to date from the file's mark while the file is open)
            fs::path Path;                 // the file (empty when it is about none)
            std::string Key;               // Path for comparing
            bool Live = false;             // from the language service (checked as typed), not a build
        };
        const std::vector<Problem>& Problems() const { return m_Problems; }
        // Automation and tests: to a problem's place (opens its file).
        void GoToProblem(int index);

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
            // Live checking: the version last seen (and when it changed), and the one last sent to be checked.
            uint64_t SeenVersion = 0, CheckedVersion = 0;
            double EditTime = 0.0;
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
        void DrawProblems(const ImVec2& size);
        void NextProblem(bool backwards);
        // The problems in a file as marks in its buffer (so they follow edits), and their places back when it closes.
        void AttachMarks(Document& doc);
        void DetachMarks(Document& doc);
        // After m_Problems changed: sorted, the explorer's error files, and the marks again (marks detached first).
        void RebuildProblems();
        // Live checking (the language service): asks for the file being edited once typing pauses, takes in results.
        void UpdateLive();
        void ApplyLive(Dimension::LanguageService::Result& result);
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

        // The last build's problems.
        std::vector<Problem> m_Problems;
        std::vector<std::string> m_ErrorFiles; // the keys of files with errors (red in the explorer)
        bool m_HaveBuildResult = false, m_LastBuildOk = false;
        bool m_ShowProblems = false, m_ShowErrors = true, m_ShowWarnings = true;
        int m_SelectedProblem = -1;

        // The language service (Roslyn in the editor's .NET): started with Code Mode when C# scripting runs.
        std::unique_ptr<Dimension::LanguageService> m_Language;
        bool m_LanguageTried = false;
        fs::path m_LivePath;     // the file of the request in flight
        double m_LiveMs = -1.0;  // how long the last check took
        std::string m_LiveError;

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
