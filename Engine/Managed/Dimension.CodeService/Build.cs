using System;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Diagnostics;
using Microsoft.CodeAnalysis.Emit;

namespace Dimension.CodeService
{
    // Building the scripts in the editor (in place of `dotnet build`, so no .NET SDK is needed): every script assembly
    // (GameScripts and the .asmdef folders), each after those it uses, compiled with the engine's analyzers and
    // written beside the scripts as <Name>.dll and .pdb (a Debug build, as `dotnet build -c Debug` makes).
    internal static class Builder
    {
        private sealed class Assembly
        {
            public string Name, Directory;
            public List<string> References = new();
            public bool EditorOnly, AutoReferenced = true;
        }

        // -> {"succeeded", "built": [names], "diagnostics": [{file, line, col, endLine, endCol, severity, code, message}], "ms"}
        public static string Build(ProjectModel project, AnalyzerOptions analyzerOptions)
        {
            var watch = Stopwatch.StartNew();
            var assemblies = Discover(project);
            var names = new HashSet<string>(assemblies.Select(a => a.Name), StringComparer.OrdinalIgnoreCase);
            var built = new Dictionary<string, MetadataReference>(StringComparer.OrdinalIgnoreCase);
            var failed = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            var builtNames = new List<string>();
            var diagnostics = new List<Diagnostic>();
            var other = new List<(string Unit, Diagnostic D)>(); // (about no file)
            var analyzers = project.Analyzers();
            Directory.CreateDirectory(project.AssembliesDir);

            foreach (var a in Order(assemblies))
            {
                // What it uses: the assemblies it names (GameScripts: every auto-referenced, non-editor one).
                var uses = a.Name == "GameScripts"
                    ? assemblies.Where(x => x.Name != "GameScripts" && x.AutoReferenced && !x.EditorOnly).Select(x => x.Name).ToList()
                    : a.References.Where(names.Contains).ToList();
                if (uses.Any(failed.Contains))
                {
                    failed.Add(a.Name); // (its errors would only repeat those of what it uses)
                    continue;
                }
                var unit = project.UnitNamed(a.Name, a.Directory);
                var compilation = unit.Compile(new Dictionary<string, string>(), uses.Select(u => built[u]).ToList(), names)
                                      .WithOptions(unit.Options.WithOptimizationLevel(OptimizationLevel.Debug));
                var all = new List<Diagnostic>();
                if (analyzers.Length > 0)
                {
                    var options = new CompilationWithAnalyzersOptions(analyzerOptions, null, concurrentAnalysis: true, logAnalyzerExecutionTime: false);
                    all.AddRange(compilation.WithAnalyzers(analyzers, options).GetAnalyzerDiagnosticsAsync().GetAwaiter().GetResult());
                }
                using var dll = new MemoryStream();
                using var pdb = new MemoryStream();
                var emit = compilation.Emit(dll, pdb, options: new EmitOptions(debugInformationFormat: DebugInformationFormat.PortablePdb,
                                                                               pdbFilePath: Path.Combine(project.AssembliesDir, a.Name + ".pdb")));
                all.AddRange(emit.Diagnostics);
                foreach (var d in all)
                {
                    if (d.IsSuppressed || (d.Severity != DiagnosticSeverity.Error && d.Severity != DiagnosticSeverity.Warning)) continue;
                    if (d.Location.IsInSource && unit.Contains(d.Location.GetLineSpan().Path)) diagnostics.Add(d);
                    else other.Add((a.Name, d));
                }
                if (!emit.Success)
                {
                    failed.Add(a.Name);
                    continue;
                }
                Write(Path.Combine(project.AssembliesDir, a.Name + ".dll"), dll);
                Write(Path.Combine(project.AssembliesDir, a.Name + ".pdb"), pdb);
                built[a.Name] = compilation.ToMetadataReference();
                builtNames.Add(a.Name);
            }

            using var stream = new MemoryStream();
            using (var w = new Utf8JsonWriter(stream))
            {
                w.WriteStartObject();
                w.WriteBoolean("succeeded", failed.Count == 0);
                w.WriteStartArray("built");
                foreach (string n in builtNames) w.WriteStringValue(n);
                w.WriteEndArray();
                w.WriteStartArray("diagnostics");
                foreach (var d in diagnostics.Distinct())
                {
                    var span = d.Location.GetLineSpan();
                    WriteDiagnostic(w, span.Path, span.StartLinePosition.Line, span.StartLinePosition.Character, span.EndLinePosition.Line,
                                    span.EndLinePosition.Character, d);
                }
                foreach (var (unitName, d) in other) WriteDiagnostic(w, unitName, -1, 0, -1, -1, d);
                w.WriteEndArray();
                w.WriteNumber("ms", watch.Elapsed.TotalMilliseconds);
                w.WriteEndObject();
            }
            return Encoding.UTF8.GetString(stream.ToArray());
        }

        private static void WriteDiagnostic(Utf8JsonWriter w, string file, int line, int col, int endLine, int endCol, Diagnostic d)
        {
            w.WriteStartObject();
            w.WriteString("file", file);
            w.WriteNumber("line", line);
            w.WriteNumber("col", col);
            w.WriteNumber("endLine", endLine);
            w.WriteNumber("endCol", endCol);
            w.WriteString("severity", d.Severity == DiagnosticSeverity.Error ? "error" : "warning");
            w.WriteString("code", d.Id);
            w.WriteString("message", d.GetMessage());
            w.WriteEndObject();
        }

        // Written whole or not at all (a half-written assembly is never loaded): to a temporary file, then moved over.
        private static void Write(string path, MemoryStream data)
        {
            string temp = path + ".tmp";
            File.WriteAllBytes(temp, data.ToArray());
            File.Move(temp, path, overwrite: true);
        }

        // GameScripts and the project's .asmdef assemblies (as the editor's CSharpProject::FindAssemblies reads them).
        private static List<Assembly> Discover(ProjectModel project)
        {
            var list = new List<Assembly> { new() { Name = "GameScripts", Directory = project.ScriptsDir } };
            if (!Directory.Exists(project.ScriptsDir)) return list;
            foreach (string file in Directory.EnumerateFiles(project.ScriptsDir, "*.asmdef", SearchOption.AllDirectories))
            {
                string dir = Path.GetDirectoryName(file);
                string rel = Path.GetRelativePath(project.ScriptsDir, dir);
                if (rel.Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar).Any(p => p is "bin" or "obj" || p.StartsWith('.'))) continue;
                var a = new Assembly { Name = ProjectModel.AsmdefName(file), Directory = Path.GetFullPath(dir) };
                try
                {
                    using var doc = JsonDocument.Parse(File.ReadAllText(file));
                    var root = doc.RootElement;
                    if (root.TryGetProperty("references", out var refs) && refs.ValueKind == JsonValueKind.Array)
                        foreach (var r in refs.EnumerateArray())
                            if (r.GetString() is { Length: > 0 } name) a.References.Add(name);
                    if (root.TryGetProperty("editorOnly", out var e) && e.ValueKind == JsonValueKind.True) a.EditorOnly = true;
                    if (root.TryGetProperty("includePlatforms", out var plats) && plats.ValueKind == JsonValueKind.Array &&
                        plats.EnumerateArray().Any(p => p.GetString() == "Editor"))
                        a.EditorOnly = true;
                    if (root.TryGetProperty("autoReferenced", out var auto) && auto.ValueKind == JsonValueKind.False) a.AutoReferenced = false;
                }
                catch (Exception)
                {
                }
                if (list.All(x => !string.Equals(x.Name, a.Name, StringComparison.OrdinalIgnoreCase))) list.Add(a);
            }
            return list;
        }

        // Each after what it uses (GameScripts after the auto-referenced ones; editor-only ones last, they may use it).
        private static List<Assembly> Order(List<Assembly> assemblies)
        {
            var byName = assemblies.ToDictionary(a => a.Name, StringComparer.OrdinalIgnoreCase);
            var done = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            var visiting = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            var order = new List<Assembly>();
            IEnumerable<string> Uses(Assembly a) => a.Name == "GameScripts"
                ? assemblies.Where(x => x.Name != "GameScripts" && x.AutoReferenced && !x.EditorOnly).Select(x => x.Name)
                : a.References.Where(byName.ContainsKey);
            void Visit(Assembly a)
            {
                if (done.Contains(a.Name) || !visiting.Add(a.Name)) return; // (a cycle: the editor reports those)
                foreach (string u in Uses(a)) Visit(byName[u]);
                visiting.Remove(a.Name);
                done.Add(a.Name);
                order.Add(a);
            }
            foreach (var a in assemblies) Visit(a);
            return order;
        }
    }
}
