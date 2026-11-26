#pragma once
#include "EditorContext.h"
#include <Dimension/Code/TextBuffer.h>
#include <Dimension/Code/CSharpLexer.h>
#include <Dimension/Code/BuildDiagnostics.h>
#include <Dimension/Scripting/LanguageService.h>
#include <Dimension/Debugger/DebugSession.h>
#include <map>
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
        // The editor's language service (null when C# scripting is off).
        std::function<Dimension::LanguageService*()> Language;
        // The editor's C# debugger, and starting it (saves, builds if needed, runs the scene in a player).
        std::function<Dimension::DebugSession*()> Debugger;
        std::function<void()> StartDebugging;
        // A passing message in the status bar.
        void Note(std::string message);
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
            bool CenterOnCursor = false; // (the debugger's place: in the middle of the view)
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
        // Completion, parameter hints (signature help) and hover info, from the language service.
        std::vector<Dimension::LanguageService::Overlay> OverlaysFor(Document& doc);
        void PollAssist();
        void TypedAssist(Document& doc, const std::string& typed);
        void RequestCompletion(Document& doc, bool explicitly);
        void RequestSignature(Document& doc);
        void FilterCompletion(Document& doc);
        void AcceptCompletion(Document& doc);
        bool AssistKeys(Document& doc); // the keys the open list or hints take (true: one was taken)
        void DrawAssist(Document& doc, ImVec2 text, float charW, float lineH, ImFont* font, float fontSize);
        // Go to definition (F12, Ctrl+click), find references (Shift+F12), rename (F2), format (Shift+Alt+F), and back /
        // forward through the places gone to (Alt+Left / Alt+Right).
        void AskAtCursor(Document& doc, Dimension::LanguageService::Query kind);
        void FormatCode(Document& doc);
        void PollNavigation();
        // Changes the service worked out (rename, format) made: in open files' buffers (one undo step each), else on disk.
        // False when an open file changed since it was asked about.
        bool ApplyEdits(const std::vector<Dimension::LanguageService::Edit>& edits, int* files = nullptr);
        void NavigateTo(const fs::path& file, int line, int col16, int endLine, int endCol16, bool remember = true);
        void GoBack(bool forward);
        void DrawReferences();
        void DrawRenamePopup();
        void DrawCodeMenu(Document& doc);

        // Breakpoints: marks in open files (they follow edits), lines for the others; the debugger always has them.
        static constexpr int kBreakpointMark = -2;
        std::vector<int> BreakpointLines(Document& doc) const;
        void ToggleBreakpoint(Document& doc, int line);
        void AttachBreakpoints(Document& doc);
        void SyncBreakpoints(Document& doc); // its lines to m_Breakpoints and the debugger
        // Debugging: going to where it stopped, the debug controls and keys, the Debug and Output tabs.
        void UpdateDebug();
        bool DebugKeys(Document& doc);
        void DrawDebugButtons();
        void DrawDebugPanel();
        void DrawOutputPanel();
        void DrawVariable(Dimension::DebugSession::Variable& v, Dimension::DebugSession& session);
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
        Dimension::LanguageService* m_Language = nullptr;
        fs::path m_LivePath;     // the file of the request in flight
        double m_LiveMs = -1.0;  // how long the last check took
        std::string m_LiveError;
        uint64_t m_NextTag = 1;

        struct CompletionState
        {
            bool Open = false, Waiting = false, Explicit = false;
            Document* Doc = nullptr;
            int Line = 0, StartCol = 0; // where the word being completed starts (bytes)
            uint64_t Tag = 0;
            std::vector<Dimension::LanguageService::CompletionItem> Items;
            std::vector<int> Shown;     // the items matching the word, best first
            std::vector<int> Scores;    // (0 prefix with its case, 1 prefix, 2 the capitals, 3 inside)
            int Selected = 0;
            bool ScrollToSelected = false;
            uint64_t FilteredVersion = 0;
            Dimension::Code::TextPos FilteredCursor{ -1, -1 };
        } m_Completion;
        struct SignatureState
        {
            bool Open = false;
            Document* Doc = nullptr;
            uint64_t Tag = 0, AskedVersion = 0;
            Dimension::Code::TextPos AskedCursor{ -1, -1 };
            std::vector<Dimension::LanguageService::Signature> Signatures;
            int Active = 0, Param = 0;
        } m_Signature;
        struct HoverState
        {
            Document* Doc = nullptr;
            int Line = -1, Col = -1;    // the word's start under the mouse
            double Since = 0.0;
            bool Asked = false, Have = false;
            uint64_t Tag = 0;
            std::string Text, Info;
            // While stopped in the debugger: the expression's value.
            bool DebugAsked = false;
            std::string DebugText;
        } m_Hover;

        // Navigation, references, rename, format.
        uint64_t m_NavTag[(int)Dimension::LanguageService::Query::Count] = {};
        std::vector<std::pair<Document*, uint64_t>> m_AskedVersions; // the open files' versions when rename/format was asked
        std::vector<Dimension::LanguageService::Location> m_References;
        std::string m_ReferencesName;
        bool m_ReferencesWaiting = false;
        int m_BottomTab = 0, m_SelectedReference = -1; // (0 problems, 1 references)
        struct NavPoint
        {
            fs::path Path;
            Dimension::Code::TextPos Pos;
        };
        std::vector<NavPoint> m_Back, m_Forward;
        bool m_RenameOpen = false, m_FocusRename = false;
        std::string m_RenameName, m_RenameOld, m_RenameWarning;
        Document* m_RenameDoc = nullptr;
        Dimension::Code::TextPos m_RenamePos;
        std::string m_Note;
        double m_NoteTime = 0.0;
        bool m_OpenCodeMenu = false;

        std::map<std::string, std::pair<fs::path, std::vector<int>>> m_Breakpoints; // by PathKey
        uint64_t m_SeenStop = 0;
        int m_SeenFrame = -1;
        struct Watch
        {
            std::string Expression, Value;
            bool Ok = true;
            uint64_t For = 0; // (the stop and frame its value is for)
        };
        std::vector<Watch> m_Watches;
        std::string m_NewWatch;
        uint64_t m_OutputSeen = 0;

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
