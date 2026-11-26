using System;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Reflection.PortableExecutable;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Diagnostics;
using Microsoft.CodeAnalysis.Text;

namespace Dimension.CodeService
{
    // Code Mode's language service: the project's scripts as Roslyn compilations (one per script assembly: GameScripts
    // and each .asmdef folder), kept warm between calls, so the file being edited can be checked as it is typed. The
    // editor calls it from one worker thread of its own; every entry point takes and returns UTF-8 JSON.
    public static unsafe class Service
    {
        // ---- Entry points (native)

        // {"scriptsDir", "managedDir", "assembliesDir"}: the project to serve. 1 when it could start.
        [UnmanagedCallersOnly]
        public static int Open(byte* configUtf8)
        {
            try
            {
                using var doc = JsonDocument.Parse(Utf8(configUtf8));
                var root = doc.RootElement;
                s_Project = new ProjectModel(root.GetProperty("scriptsDir").GetString(), root.GetProperty("managedDir").GetString(),
                                             root.GetProperty("assembliesDir").GetString());
                return 1;
            }
            catch (Exception e)
            {
                s_LastError = e.ToString();
                return 0;
            }
        }

        // {"kind", "file", "overlays": [{"path", "text"}], "line", "col"}: a question about `file` (and the assembly it is
        // in), with the texts of files being edited (unsaved) in place of what is on disk. Lines are 0-based, columns
        // UTF-16. The kinds (see Queries.cs for the last three):
        //   "diagnostics" -> {"assembly", "files": [...every source of it...], "diagnostics": [{file, line, col, endLine,
        //                     endCol, severity, code, message}], "ms"}
        //   "completion"  -> {"items": [{label, kind, detail, insert?}], "ms"}
        //   "signature"   -> {"signatures": [{label, doc, params: [{start, length, name, doc}]}], "active", "param", "ms"}
        //   "hover"       -> {"text", "doc", "ms"}
        // or {"error"}.
        [UnmanagedCallersOnly]
        public static byte* Analyze(byte* requestUtf8)
        {
            string json;
            try
            {
                json = AnalyzeJson(Utf8(requestUtf8));
            }
            catch (Exception e)
            {
                json = JsonSerializer.Serialize(new Dictionary<string, string> { ["error"] = e.ToString() });
            }
            return Result(json);
        }

        [UnmanagedCallersOnly]
        public static void Free(byte* result)
        {
            if (result != null) Marshal.FreeHGlobal((IntPtr)result);
        }

        // ---- Work

        private static ProjectModel s_Project;
        private static string s_LastError;

        private static string AnalyzeJson(string request)
        {
            if (s_Project == null) throw new InvalidOperationException("Open was not called" + (s_LastError != null ? ": " + s_LastError : ""));
            using var doc = JsonDocument.Parse(request);
            var root = doc.RootElement;
            string file = root.GetProperty("file").GetString();
            var overlays = new Dictionary<string, string>(PathComparer);
            if (root.TryGetProperty("overlays", out var list))
                foreach (var o in list.EnumerateArray()) overlays[Path.GetFullPath(o.GetProperty("path").GetString())] = o.GetProperty("text").GetString();

            var watch = Stopwatch.StartNew();
            string kind = root.TryGetProperty("kind", out var k) ? k.GetString() : "diagnostics";
            var unit = s_Project.UnitOf(Path.GetFullPath(file));
            var compilation = unit.Compile(overlays);
            if (kind != "diagnostics")
            {
                int line = root.TryGetProperty("line", out var l) ? l.GetInt32() : 0;
                int col = root.TryGetProperty("col", out var c) ? c.GetInt32() : 0;
                var tree = unit.TreeOf(Path.GetFullPath(file));
                if (tree == null) return "{\"ms\":0}";
                var text = tree.GetText();
                line = Math.Clamp(line, 0, text.Lines.Count - 1);
                int position = Math.Min(text.Lines[line].Start + Math.Max(0, col), text.Lines[line].End);
                var model = compilation.GetSemanticModel(tree);
                string path = Path.GetFullPath(file);
                UnitWorkspace Space() => unit.Space(compilation);
                using var output = new MemoryStream();
                using (var w = new Utf8JsonWriter(output))
                {
                    w.WriteStartObject();
                    switch (kind)
                    {
                        case "completion": Queries.Completion(model, position, w); break;
                        case "signature": Queries.Signature(model, position, w); break;
                        case "hover": Queries.Hover(model, position, w); break;
                        case "definition": Navigation.Definition(model, position, w); break;
                        case "references": Navigation.References(Space(), path, position, w); break;
                        case "renameCheck":
                        case "rename":
                            Navigation.Rename(Space(), path, position, root.TryGetProperty("newName", out var n) ? n.GetString() : "", kind == "renameCheck", w);
                            break;
                        case "format":
                            Navigation.Format(Space(), path, line, root.TryGetProperty("endLine", out var e) ? e.GetInt32() : -1,
                                              root.TryGetProperty("tabs", out var t) && t.GetBoolean(), root.TryGetProperty("crlf", out var cr) && cr.GetBoolean(), w);
                            break;
                    }
                    w.WriteNumber("ms", watch.Elapsed.TotalMilliseconds);
                    w.WriteEndObject();
                }
                return Encoding.UTF8.GetString(output.ToArray());
            }
            ImmutableArray<Diagnostic> diagnostics;
            var analyzers = s_Project.Analyzers();
            if (analyzers.Length > 0)
            {
                var options = new CompilationWithAnalyzersOptions(AnalyzerOptions(), null, concurrentAnalysis: true, logAnalyzerExecutionTime: false);
                diagnostics = compilation.WithAnalyzers(analyzers, options).GetAllDiagnosticsAsync().GetAwaiter().GetResult();
            }
            else diagnostics = compilation.GetDiagnostics();

            using var stream = new MemoryStream();
            using (var w = new Utf8JsonWriter(stream))
            {
                w.WriteStartObject();
                w.WriteString("assembly", unit.Name);
                w.WriteStartArray("files");
                foreach (var f in unit.Files) w.WriteStringValue(f);
                w.WriteEndArray();
                w.WriteStartArray("diagnostics");
                foreach (var d in diagnostics)
                {
                    if (d.IsSuppressed || (d.Severity != DiagnosticSeverity.Error && d.Severity != DiagnosticSeverity.Warning)) continue;
                    if (!d.Location.IsInSource) continue; // (project-wide ones: the build reports those)
                    var span = d.Location.GetLineSpan();
                    if (!unit.Contains(span.Path)) continue;  // (the generated global usings)
                    w.WriteStartObject();
                    w.WriteString("file", span.Path);
                    w.WriteNumber("line", span.StartLinePosition.Line);
                    w.WriteNumber("col", span.StartLinePosition.Character);
                    w.WriteNumber("endLine", span.EndLinePosition.Line);
                    w.WriteNumber("endCol", span.EndLinePosition.Character);
                    w.WriteString("severity", d.Severity == DiagnosticSeverity.Error ? "error" : "warning");
                    w.WriteString("code", d.Id);
                    w.WriteString("message", d.GetMessage());
                    w.WriteEndObject();
                }
                w.WriteEndArray();
                w.WriteNumber("ms", watch.Elapsed.TotalMilliseconds);
                w.WriteEndObject();
            }
            return Encoding.UTF8.GetString(stream.ToArray());
        }

        private static AnalyzerOptions s_AnalyzerOptions;
        private static AnalyzerOptions AnalyzerOptions() => s_AnalyzerOptions ??= new AnalyzerOptions(ImmutableArray<AdditionalText>.Empty);

        internal static readonly StringComparer PathComparer = OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;

        private static string Utf8(byte* p) => p == null ? "" : Marshal.PtrToStringUTF8((IntPtr)p) ?? "";

        private static byte* Result(string s)
        {
            byte[] bytes = Encoding.UTF8.GetBytes(s);
            IntPtr mem = Marshal.AllocHGlobal(bytes.Length + 1);
            Marshal.Copy(bytes, 0, mem, bytes.Length);
            ((byte*)mem)[bytes.Length] = 0;
            return (byte*)mem;
        }
    }

    // The scripts as assemblies: GameScripts (the Scripts folder) and the .asmdef folders split off from it.
    internal sealed class ProjectModel
    {
        public readonly string ScriptsDir, ManagedDir, AssembliesDir;
        private readonly Dictionary<string, Unit> m_Units = new(StringComparer.OrdinalIgnoreCase);
        private readonly Dictionary<string, (DateTime Time, MetadataReference Ref)> m_References = new(Service.PathComparer);
        private ImmutableArray<DiagnosticAnalyzer>? m_Analyzers;

        public ProjectModel(string scriptsDir, string managedDir, string assembliesDir)
        {
            ScriptsDir = Path.GetFullPath(scriptsDir);
            ManagedDir = Path.GetFullPath(managedDir);
            AssembliesDir = Path.GetFullPath(assembliesDir);
        }

        // The assembly a file belongs to: the nearest folder above it with an .asmdef, else GameScripts.
        public Unit UnitOf(string file)
        {
            string name = "GameScripts", dir = ScriptsDir;
            for (string d = Path.GetDirectoryName(file); d != null && d.Length >= ScriptsDir.Length; d = Path.GetDirectoryName(d))
            {
                string asmdef = Directory.Exists(d) ? Directory.EnumerateFiles(d, "*.asmdef").FirstOrDefault() : null;
                if (asmdef != null)
                {
                    name = AsmdefName(asmdef);
                    dir = d;
                    break;
                }
                if (Service.PathComparer.Equals(d, ScriptsDir)) break;
            }
            if (!m_Units.TryGetValue(name, out var unit)) m_Units[name] = unit = new Unit(this, name, dir);
            return unit;
        }

        private static string AsmdefName(string asmdef)
        {
            try
            {
                using var doc = JsonDocument.Parse(File.ReadAllText(asmdef));
                if (doc.RootElement.TryGetProperty("name", out var n) && n.GetString() is { Length: > 0 } s) return s;
            }
            catch (Exception)
            {
            }
            return Path.GetFileNameWithoutExtension(asmdef);
        }

        // What an assembly compiles against: the .NET framework (the running one's assemblies: no SDK needed), the
        // engine's API, and the built assemblies beside the scripts (other script assemblies, NuGet packages' DLLs).
        public List<MetadataReference> References(string ownName)
        {
            var paths = new List<string>();
            if (AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES") is string tpa)
                paths.AddRange(tpa.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries));
            paths.Add(Path.Combine(ManagedDir, "Dimension.Core.dll"));
            if (Directory.Exists(AssembliesDir))
                foreach (string dll in Directory.EnumerateFiles(AssembliesDir, "*.dll"))
                    if (!string.Equals(Path.GetFileNameWithoutExtension(dll), ownName, StringComparison.OrdinalIgnoreCase)) paths.Add(dll);
            var refs = new List<MetadataReference>();
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (string p in paths)
            {
                if (!File.Exists(p) || !seen.Add(Path.GetFileName(p))) continue;
                DateTime time = File.GetLastWriteTimeUtc(p);
                if (!m_References.TryGetValue(p, out var cached) || cached.Time != time)
                {
                    string xml = Path.ChangeExtension(p, ".xml");
                    cached = (time, IsManaged(p) ? MetadataReference.CreateFromFile(p, documentation: File.Exists(xml) ? new XmlDocs(xml) : null) : null);
                    m_References[p] = cached;
                }
                if (cached.Ref != null) refs.Add(cached.Ref);
            }
            return refs;
        }

        private static bool IsManaged(string path)
        {
            try
            {
                using var stream = File.OpenRead(path);
                using var pe = new PEReader(stream);
                return pe.HasMetadata;
            }
            catch (Exception)
            {
                return false;
            }
        }

        // The engine's script checks (DIM001...), as the build runs them.
        public ImmutableArray<DiagnosticAnalyzer> Analyzers()
        {
            if (m_Analyzers.HasValue) return m_Analyzers.Value;
            var list = ImmutableArray.CreateBuilder<DiagnosticAnalyzer>();
            string path = Path.Combine(ManagedDir, "Dimension.Analyzers.dll");
            try
            {
                if (File.Exists(path))
                {
                    // (into this service's load context, so it binds to the Roslyn already loaded here)
                    var context = AssemblyLoadContext.GetLoadContext(typeof(ProjectModel).Assembly) ?? AssemblyLoadContext.Default;
                    Assembly assembly = context.LoadFromAssemblyPath(path);
                    foreach (Type t in assembly.GetTypes())
                        if (!t.IsAbstract && typeof(DiagnosticAnalyzer).IsAssignableFrom(t) && t.GetCustomAttributes(typeof(DiagnosticAnalyzerAttribute), false).Length > 0)
                            list.Add((DiagnosticAnalyzer)Activator.CreateInstance(t));
                }
            }
            catch (Exception)
            {
                // (without them: the compiler's diagnostics only)
            }
            m_Analyzers = list.ToImmutable();
            return m_Analyzers.Value;
        }
    }

    // One script assembly: its sources (parsed once each, again only when they change) and options from its csproj.
    internal sealed class Unit
    {
        public readonly string Name, Directory;
        public List<string> Files { get; private set; } = new();

        private readonly ProjectModel m_Project;
        private readonly Dictionary<string, (string Text, SyntaxTree Tree)> m_Trees = new(Service.PathComparer);
        private HashSet<string> m_FileSet = new(Service.PathComparer);
        private CSharpParseOptions m_Parse;
        private CSharpCompilationOptions m_Options;
        private SyntaxTree m_GlobalUsings;
        private DateTime m_ProjectTime;
        private CSharpCompilation m_Compilation;
        private List<MetadataReference> m_CompiledReferences;
        private List<SyntaxTree> m_CompiledTrees;

        public Unit(ProjectModel project, string name, string directory)
        {
            m_Project = project;
            Name = name;
            Directory = directory;
        }

        public bool Contains(string path) => path != null && m_FileSet.Contains(path);
        private UnitWorkspace m_Space;
        // The unit as a workspace (references, rename, format), up to date with `compilation` (the last Compile's).
        public UnitWorkspace Space(CSharpCompilation compilation)
        {
            m_Space ??= new UnitWorkspace();
            m_Space.Sync(this, compilation);
            return m_Space;
        }

        public SyntaxTree TreeOf(string path) => m_Trees.TryGetValue(path, out var t) ? t.Tree : null;

        public CSharpCompilation Compile(Dictionary<string, string> overlays)
        {
            ReadProject();
            // The sources: every .cs under the folder, but not build output or folders that are assemblies of their own.
            var files = new List<string>();
            Collect(Directory, files, true);
            files.Sort(StringComparer.OrdinalIgnoreCase);
            Files = files;
            m_FileSet = new HashSet<string>(files, Service.PathComparer);
            var trees = new List<SyntaxTree>(files.Count + 1);
            foreach (string f in files)
            {
                string text = overlays.TryGetValue(f, out var o) ? o : ReadFile(f);
                if (!m_Trees.TryGetValue(f, out var cached) || cached.Text != text)
                {
                    cached = (text, CSharpSyntaxTree.ParseText(SourceText.From(text, Encoding.UTF8), m_Parse, f));
                    m_Trees[f] = cached;
                }
                trees.Add(cached.Tree);
            }
            foreach (string gone in m_Trees.Keys.Where(k => !m_FileSet.Contains(k)).ToList()) m_Trees.Remove(gone);
            if (m_GlobalUsings != null) trees.Add(m_GlobalUsings);
            var references = m_Project.References(Name);
            bool sameReferences = m_CompiledReferences != null && m_CompiledReferences.Count == references.Count &&
                                  m_CompiledReferences.Zip(references).All(r => ReferenceEquals(r.First, r.Second));
            if (m_Compilation != null && sameReferences && m_CompiledTrees.Count == trees.Count &&
                m_CompiledTrees.Zip(trees).All(t => t.First.FilePath == t.Second.FilePath))
            {
                // (Roslyn keeps what it knows about the files that did not change)
                for (int i = 0; i < trees.Count; ++i)
                    if (!ReferenceEquals(m_CompiledTrees[i], trees[i])) m_Compilation = m_Compilation.ReplaceSyntaxTree(m_CompiledTrees[i], trees[i]);
            }
            else m_Compilation = CSharpCompilation.Create(Name, trees, references, m_Options);
            m_CompiledTrees = trees;
            m_CompiledReferences = references;
            return m_Compilation;
        }

        private static string ReadFile(string path)
        {
            try
            {
                return File.ReadAllText(path);
            }
            catch (IOException)
            {
                return "";
            }
        }

        private void Collect(string dir, List<string> files, bool top)
        {
            if (!top && System.IO.Directory.EnumerateFiles(dir, "*.asmdef").Any()) return;
            foreach (string f in System.IO.Directory.EnumerateFiles(dir, "*.cs")) files.Add(Path.GetFullPath(f));
            foreach (string d in System.IO.Directory.EnumerateDirectories(dir))
            {
                string n = Path.GetFileName(d);
                if (n == "bin" || n == "obj" || n.StartsWith('.')) continue;
                Collect(d, files, false);
            }
        }

        // The options the build uses: from GameScripts.csproj (or the .asmdef's generated project), as the SDK sets them.
        private void ReadProject()
        {
            string csproj = Name == "GameScripts"
                ? Path.Combine(m_Project.ScriptsDir, "GameScripts.csproj")
                : Path.Combine(Path.GetDirectoryName(m_Project.ScriptsDir) ?? "", ".dimension", "ScriptProjects", Name, Name + ".csproj");
            DateTime time = File.Exists(csproj) ? File.GetLastWriteTimeUtc(csproj) : DateTime.MinValue;
            if (m_Options != null && time == m_ProjectTime) return;
            m_ProjectTime = time;
            string text = File.Exists(csproj) ? File.ReadAllText(csproj) : "";
            string Prop(string name, string fallback)
            {
                var m = Regex.Match(text, "<" + name + ">\\s*([^<]*?)\\s*</" + name + ">");
                return m.Success ? m.Groups[1].Value : fallback;
            }
            bool On(string value) => string.Equals(value, "true", StringComparison.OrdinalIgnoreCase) || string.Equals(value, "enable", StringComparison.OrdinalIgnoreCase);

            var defines = new List<string> { "TRACE", "DEBUG", "NET", "NETCOREAPP" };
            for (int v = 5; v <= 10; ++v) defines.Add($"NET{v}_0_OR_GREATER");
            defines.Add("NET10_0");
            defines.AddRange(new[] { "NETCOREAPP3_1_OR_GREATER", "NETCOREAPP3_0_OR_GREATER", "NETCOREAPP2_1_OR_GREATER", "NETCOREAPP2_0_OR_GREATER", "NETCOREAPP1_1_OR_GREATER", "NETCOREAPP1_0_OR_GREATER" });
            foreach (string d in Prop("DefineConstants", "").Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
                if (d != "$(DefineConstants)") defines.Add(d);

            LanguageVersion lang = LanguageVersion.Default;
            string langText = Prop("LangVersion", "");
            if (langText.Length > 0 && LanguageVersionFacts.TryParse(langText, out var parsed)) lang = parsed;
            m_Parse = new CSharpParseOptions(lang, DocumentationMode.Parse, SourceCodeKind.Regular, defines);

            var nullable = Prop("Nullable", "disable").ToLowerInvariant() switch
            {
                "enable" => NullableContextOptions.Enable,
                "warnings" => NullableContextOptions.Warnings,
                "annotations" => NullableContextOptions.Annotations,
                _ => NullableContextOptions.Disable,
            };
            // (the warnings the build leaves out: NoWarn, and the SDK's defaults)
            var suppress = new Dictionary<string, ReportDiagnostic> { ["CS1701"] = ReportDiagnostic.Suppress, ["CS1702"] = ReportDiagnostic.Suppress };
            foreach (string id in Prop("NoWarn", "").Split(new[] { ';', ',' }, StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
                if (!id.StartsWith("$(")) suppress[id.All(char.IsDigit) ? "CS" + id.PadLeft(4, '0') : id] = ReportDiagnostic.Suppress;
            m_Options = new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, allowUnsafe: On(Prop("AllowUnsafeBlocks", "false")),
                                                     nullableContextOptions: nullable, warningLevel: 9999, specificDiagnosticOptions: suppress,
                                                     concurrentBuild: true);
            // ImplicitUsings: the global usings the SDK writes for Microsoft.NET.Sdk.
            m_GlobalUsings = On(Prop("ImplicitUsings", "disable"))
                ? CSharpSyntaxTree.ParseText("global using global::System;\nglobal using global::System.Collections.Generic;\nglobal using global::System.IO;\n" +
                                             "global using global::System.Linq;\nglobal using global::System.Net.Http;\nglobal using global::System.Threading;\n" +
                                             "global using global::System.Threading.Tasks;\n", m_Parse, "GlobalUsings.g.cs")
                : null;
            m_Trees.Clear(); // (parsed with the old options)
            m_Compilation = null;
        }
    }

    // A reference's XML documentation file (Dimension.Core.xml...), read once: member id -> its <member> element.
    internal sealed class XmlDocs : DocumentationProvider
    {
        private readonly string m_Path;
        private Dictionary<string, string> m_Members;
        private readonly object m_Lock = new();

        public XmlDocs(string path) => m_Path = path;

        protected override string GetDocumentationForSymbol(string documentationMemberID, System.Globalization.CultureInfo preferredCulture,
                                                            CancellationToken cancellationToken = default)
        {
            lock (m_Lock)
            if (m_Members == null)
            {
                var members = new Dictionary<string, string>();
                try
                {
                    var doc = System.Xml.Linq.XDocument.Load(m_Path);
                    foreach (var m in doc.Descendants("member"))
                        if (m.Attribute("name")?.Value is { } name) members[name] = m.ToString();
                }
                catch (Exception)
                {
                }
                m_Members = members;
            }
            return m_Members.TryGetValue(documentationMemberID, out var xml) ? xml : "";
        }

        public override bool Equals(object obj) => obj is XmlDocs other && other.m_Path == m_Path;
        public override int GetHashCode() => m_Path.GetHashCode();
    }
}
