using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Threading.Tasks;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.FindSymbols;
using Microsoft.CodeAnalysis.Formatting;
using Microsoft.CodeAnalysis.Host.Mef;
using Microsoft.CodeAnalysis.Rename;
using Microsoft.CodeAnalysis.Text;

namespace Dimension.CodeService
{
    // A script assembly as a Roslyn workspace (a solution of one project), for what needs one: finding references,
    // renaming and formatting. Kept beside the Unit's own compilation and brought up to date with its texts.
    internal sealed class UnitWorkspace
    {
        private static MefHostServices s_Host;
        private AdhocWorkspace m_Workspace;
        private ProjectId m_Project;
        private readonly Dictionary<string, (DocumentId Id, string Text)> m_Documents = new(Service.PathComparer);
        private object m_OptionsKey, m_ReferencesKey;

        public Solution Solution => m_Workspace.CurrentSolution;

        // Up to date with the unit's files and texts (made again when its options or references changed).
        public void Sync(Unit unit, CSharpCompilation compilation)
        {
            var options = compilation.Options;
            var references = compilation.References.ToList();
            bool sameReferences = m_ReferencesKey is List<MetadataReference> old && old.Count == references.Count && old.Zip(references).All(r => ReferenceEquals(r.First, r.Second));
            if (m_Workspace == null || !ReferenceEquals(m_OptionsKey, options) || !sameReferences)
            {
                s_Host ??= MefHostServices.Create(MefHostServices.DefaultAssemblies);
                m_Workspace?.Dispose();
                m_Workspace = new AdhocWorkspace(s_Host);
                m_Project = ProjectId.CreateNewId(unit.Name);
                var parse = (CSharpParseOptions)compilation.SyntaxTrees.First().Options;
                m_Workspace.AddProject(ProjectInfo.Create(m_Project, VersionStamp.Create(), unit.Name, unit.Name, LanguageNames.CSharp,
                                                          compilationOptions: options, parseOptions: parse, metadataReferences: references));
                m_Documents.Clear();
                m_OptionsKey = options;
                m_ReferencesKey = references;
            }
            var solution = m_Workspace.CurrentSolution;
            var seen = new HashSet<string>(Service.PathComparer);
            foreach (var tree in compilation.SyntaxTrees)
            {
                string path = tree.FilePath;
                seen.Add(path);
                string text = tree.GetText().ToString();
                if (m_Documents.TryGetValue(path, out var doc))
                {
                    if (doc.Text != text) solution = solution.WithDocumentText(doc.Id, SourceText.From(text));
                    m_Documents[path] = (doc.Id, text);
                }
                else
                {
                    var id = DocumentId.CreateNewId(m_Project, path);
                    solution = solution.AddDocument(DocumentInfo.Create(id, Path.GetFileName(path), filePath: path,
                                                                        loader: TextLoader.From(TextAndVersion.Create(SourceText.From(text), VersionStamp.Create()))));
                    m_Documents[path] = (id, text);
                }
            }
            foreach (var gone in m_Documents.Keys.Where(k => !seen.Contains(k)).ToList())
            {
                solution = solution.RemoveDocument(m_Documents[gone].Id);
                m_Documents.Remove(gone);
            }
            m_Workspace.TryApplyChanges(solution);
        }

        public Document DocumentOf(string path) => m_Documents.TryGetValue(path, out var d) ? m_Workspace.CurrentSolution.GetDocument(d.Id) : null;
        public Workspace Workspace => m_Workspace;
    }

    // Go to definition, find references, rename and format.
    internal static class Navigation
    {
        // -> {"locations": [{file, line, col, endLine, endCol}], "name"} or {"message"} (defined outside the scripts).
        public static void Definition(SemanticModel model, int position, Utf8JsonWriter w)
        {
            var symbol = Queries.SymbolAt(model, position);
            if (symbol == null)
            {
                w.WriteString("message", "Nothing to go to here");
                return;
            }
            if (symbol is IMethodSymbol { MethodKind: MethodKind.Constructor, IsImplicitlyDeclared: true } ctor) symbol = ctor.ContainingType;
            var places = symbol.Locations.Where(l => l.IsInSource).ToList();
            w.WriteString("name", symbol.Name);
            if (places.Count == 0)
            {
                string from = symbol.ContainingAssembly?.Name ?? "a library";
                w.WriteString("message", $"{Queries.Display(symbol, model, position)} is defined in {from} (no source here)");
                return;
            }
            w.WriteStartArray("locations");
            foreach (var l in places) WriteLocation(w, l.GetLineSpan(), null, true);
            w.WriteEndArray();
        }

        // -> {"name", "locations": [{file, line, col, endLine, endCol, text, definition}]} or {"message"}.
        public static void References(UnitWorkspace space, string file, int position, Utf8JsonWriter w)
        {
            var (symbol, document) = SymbolIn(space, file, position);
            if (symbol == null)
            {
                w.WriteString("message", "No symbol here to find");
                return;
            }
            var found = SymbolFinder.FindReferencesAsync(symbol, space.Solution).GetAwaiter().GetResult();
            var places = new List<(FileLinePositionSpan Span, bool Definition)>();
            foreach (var r in found)
            {
                foreach (var l in r.Definition.Locations.Where(l => l.IsInSource)) places.Add((l.GetLineSpan(), true));
                foreach (var loc in r.Locations)
                    if (!loc.IsImplicit && loc.Location.IsInSource) places.Add((loc.Location.GetLineSpan(), false));
            }
            places = places.GroupBy(p => (p.Span.Path, p.Span.StartLinePosition)).Select(g => g.OrderByDescending(p => p.Definition).First())
                           .OrderBy(p => p.Span.Path, StringComparer.OrdinalIgnoreCase).ThenBy(p => p.Span.StartLinePosition).ToList();
            w.WriteString("name", symbol.Name);
            w.WriteStartArray("locations");
            foreach (var (span, definition) in places) WriteLocation(w, span, LineText(space, span), definition);
            w.WriteEndArray();
        }

        // Rename: -> {"edits": [{file, line, col, endLine, endCol, text}], "renameFile": {from, to}?} or {"message"}.
        // With "check" only: {"name"} (what is being renamed) or {"message"} (why it can't be).
        public static void Rename(UnitWorkspace space, string file, int position, string newName, bool check, Utf8JsonWriter w)
        {
            var (symbol, document) = SymbolIn(space, file, position);
            if (symbol is IMethodSymbol { MethodKind: MethodKind.Constructor or MethodKind.Destructor } ctor) symbol = ctor.ContainingType;
            if (symbol == null || symbol.Kind is SymbolKind.Namespace || symbol.IsImplicitlyDeclared && symbol is not IParameterSymbol)
            {
                w.WriteString("message", "Nothing here to rename");
                return;
            }
            if (!symbol.Locations.Any(l => l.IsInSource))
            {
                w.WriteString("message", $"{symbol.Name} is defined in {symbol.ContainingAssembly?.Name ?? "a library"}: it can't be renamed here");
                return;
            }
            // An override of the engine's (OnUpdate...) keeps the engine's name.
            for (ISymbol over = Overridden(symbol); over != null; over = Overridden(over))
                if (!over.Locations.Any(l => l.IsInSource))
                {
                    w.WriteString("message", $"{symbol.Name} overrides {over.ContainingType?.Name}.{over.Name}: it keeps that name");
                    return;
                }
            if (check)
            {
                w.WriteString("name", symbol.Name);
                if (symbol is INamedTypeSymbol t && IsScriptType(t))
                    w.WriteString("warning", "Scenes and prefabs refer to scripts by class name: objects using this script lose it.");
                return;
            }
            if (!SyntaxFacts.IsValidIdentifier(newName) || SyntaxFacts.GetKeywordKind(newName) != SyntaxKind.None)
            {
                w.WriteString("message", $"'{newName}' is not a name C# allows");
                return;
            }
            var solution = space.Solution;
            var renamed = Renamer.RenameSymbolAsync(solution, symbol, new SymbolRenameOptions(RenameOverloads: false, RenameInStrings: false, RenameInComments: false, RenameFile: false),
                                                    newName).GetAwaiter().GetResult();
            w.WriteStartArray("edits");
            foreach (var id in renamed.GetChanges(solution).GetProjectChanges().SelectMany(p => p.GetChangedDocuments()))
            {
                var before = solution.GetDocument(id);
                var after = renamed.GetDocument(id);
                var oldText = before.GetTextAsync().GetAwaiter().GetResult();
                foreach (var change in after.GetTextChangesAsync(before).GetAwaiter().GetResult())
                    WriteEdit(w, before.FilePath, oldText, change);
            }
            w.WriteEndArray();
            // A type in a file of its name: the file too (Mover.cs -> Runner.cs).
            if (symbol is INamedTypeSymbol)
                foreach (var l in symbol.Locations.Where(l => l.IsInSource))
                {
                    string path = l.SourceTree.FilePath;
                    if (string.Equals(Path.GetFileNameWithoutExtension(path), symbol.Name, StringComparison.Ordinal))
                    {
                        string to = Path.Combine(Path.GetDirectoryName(path) ?? "", newName + Path.GetExtension(path));
                        if (!File.Exists(to))
                        {
                            w.WriteStartObject("renameFile");
                            w.WriteString("from", path);
                            w.WriteString("to", to);
                            w.WriteEndObject();
                        }
                        break;
                    }
                }
        }

        private static ISymbol Overridden(ISymbol s) => s switch
        {
            IMethodSymbol m => m.OverriddenMethod,
            IPropertySymbol p => p.OverriddenProperty,
            IEventSymbol e => e.OverriddenEvent,
            _ => null,
        };

        private static bool IsScriptType(INamedTypeSymbol t)
        {
            for (var b = t.BaseType; b != null; b = b.BaseType)
                if (b.Name == "Behaviour" && b.ContainingNamespace?.Name == "Dimension") return true;
            return false;
        }

        // Format: the document, or the lines from `line` to `endLine` (-1: all) -> {"edits": [...]}.
        public static void Format(UnitWorkspace space, string file, int line, int endLine, bool tabs, bool crlf, Utf8JsonWriter w)
        {
            var document = space.DocumentOf(file);
            w.WriteStartArray("edits");
            if (document != null)
            {
                var text = document.GetTextAsync().GetAwaiter().GetResult();
                var options = space.Solution.Options.WithChangedOption(FormattingOptions.UseTabs, LanguageNames.CSharp, tabs)
                                   .WithChangedOption(FormattingOptions.NewLine, LanguageNames.CSharp, "\n");
                TextSpan? span = null;
                if (endLine >= 0)
                {
                    int a = Math.Clamp(line, 0, text.Lines.Count - 1), b = Math.Clamp(endLine, a, text.Lines.Count - 1);
                    span = TextSpan.FromBounds(text.Lines[a].Start, text.Lines[b].End);
                }
#pragma warning disable CS0618 // (the OptionSet overloads: what an ad hoc workspace has)
                var formatted = span.HasValue ? Formatter.FormatAsync(document, span.Value, options).GetAwaiter().GetResult()
                                              : Formatter.FormatAsync(document, options).GetAwaiter().GetResult();
#pragma warning restore CS0618
                foreach (var change in formatted.GetTextChangesAsync(document).GetAwaiter().GetResult())
                    WriteEdit(w, file, text, change);
            }
            w.WriteEndArray();
        }

        // ---- Helpers

        private static (ISymbol, Document) SymbolIn(UnitWorkspace space, string file, int position)
        {
            var document = space.DocumentOf(file);
            if (document == null) return (null, null);
            var model = document.GetSemanticModelAsync().GetAwaiter().GetResult();
            return (Queries.SymbolAt(model, position), document);
        }

        private static string LineText(UnitWorkspace space, FileLinePositionSpan span)
        {
            var doc = space.DocumentOf(span.Path);
            if (doc == null) return "";
            var text = doc.GetTextAsync().GetAwaiter().GetResult();
            int line = span.StartLinePosition.Line;
            return line < text.Lines.Count ? text.Lines[line].ToString() : "";
        }

        private static void WriteLocation(Utf8JsonWriter w, FileLinePositionSpan span, string lineText, bool definition)
        {
            w.WriteStartObject();
            w.WriteString("file", span.Path);
            w.WriteNumber("line", span.StartLinePosition.Line);
            w.WriteNumber("col", span.StartLinePosition.Character);
            w.WriteNumber("endLine", span.EndLinePosition.Line);
            w.WriteNumber("endCol", span.EndLinePosition.Character);
            if (lineText != null) w.WriteString("text", lineText);
            w.WriteBoolean("definition", definition);
            w.WriteEndObject();
        }

        private static void WriteEdit(Utf8JsonWriter w, string file, SourceText text, TextChange change)
        {
            var a = text.Lines.GetLinePosition(change.Span.Start);
            var b = text.Lines.GetLinePosition(change.Span.End);
            w.WriteStartObject();
            w.WriteString("file", file);
            w.WriteNumber("line", a.Line);
            w.WriteNumber("col", a.Character);
            w.WriteNumber("endLine", b.Line);
            w.WriteNumber("endCol", b.Character);
            w.WriteString("text", change.NewText ?? "");
            w.WriteEndObject();
        }
    }
}
