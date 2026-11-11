using System;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Diagnostics;

namespace Dimension.Analyzers
{
    /// <summary>Engine-specific checks of game scripts (like Unity's analyzers): callbacks the engine never calls,
    /// native containers never disposed, and engine objects used inside jobs.</summary>
    [DiagnosticAnalyzer(LanguageNames.CSharp)]
    public sealed class ScriptAnalyzer : DiagnosticAnalyzer
    {
        public static readonly DiagnosticDescriptor Callback = new(
            "DIM001", "Behaviour callback the engine does not call",
            "'{0}' is never called by the engine: {1}", "Dimension", DiagnosticSeverity.Warning, isEnabledByDefault: true,
            description: "Behaviour callbacks are virtual methods: a method with a callback's name needs 'override', and one a letter or two away from a callback's name is probably misspelled.");

        public static readonly DiagnosticDescriptor NotDisposed = new(
            "DIM002", "Native container not disposed",
            "'{0}' is a {1} made with Allocator.{2} and never disposed in this method: call Dispose() (or Dispose(handle) after its job), or use 'using'", "Dimension",
            DiagnosticSeverity.Warning, isEnabledByDefault: true,
            description: "NativeArray, NativeList, NativeHashMap and NativeQueue hold native memory until disposed.");

        public static readonly DiagnosticDescriptor EngineInJob = new(
            "DIM003", "Engine object used inside a job",
            "'{0}' is main-thread only and cannot be used inside a job's Execute: copy what the job needs into a NativeArray (or use IJobParallelForTransform to move objects)", "Dimension",
            DiagnosticSeverity.Warning, isEnabledByDefault: true,
            description: "Jobs run on worker threads; entities, components and engine services are not thread-safe.");

        public override ImmutableArray<DiagnosticDescriptor> SupportedDiagnostics => ImmutableArray.Create(Callback, NotDisposed, EngineInJob);

        private static readonly string[] s_Containers = { "NativeArray", "NativeList", "NativeHashMap", "NativeQueue" };
        private static readonly string[] s_JobInterfaces = { "IJob", "IJobParallelFor", "IJobFor", "IJobParallelForTransform" };
        // Engine types a job must not touch (structs and static services as well as components).
        private static readonly string[] s_MainThreadTypes =
        {
            "Entity", "Transform", "Component", "Behaviour", "Physics", "Physics2D", "Input", "SceneManager", "Time", "AudioMixer",
            "PlayerPrefs", "Camera", "Gizmos",
        };

        public override void Initialize(AnalysisContext context)
        {
            context.ConfigureGeneratedCodeAnalysis(GeneratedCodeAnalysisFlags.None);
            context.EnableConcurrentExecution();
            context.RegisterSymbolAction(CheckCallbacks, SymbolKind.Method);
            context.RegisterSyntaxNodeAction(CheckDisposal, SyntaxKind.LocalDeclarationStatement);
            context.RegisterSyntaxNodeAction(CheckJob, SyntaxKind.MethodDeclaration);
        }

        private static bool IsEngineType(ITypeSymbol type, string name) =>
            type != null && type.Name == name && type.ContainingNamespace?.ToDisplayString() == "Dimension";

        // Behaviour, when the type derives from it.
        private static INamedTypeSymbol BehaviourBase(INamedTypeSymbol type)
        {
            for (INamedTypeSymbol t = type?.BaseType; t != null; t = t.BaseType)
                if (IsEngineType(t, "Behaviour")) return t;
            return null;
        }

        // DIM001: a Behaviour's method named like a callback without override, or a near miss of one.
        private static void CheckCallbacks(SymbolAnalysisContext context)
        {
            var method = (IMethodSymbol)context.Symbol;
            if (method.MethodKind != MethodKind.Ordinary || method.IsOverride || method.IsStatic || method.IsImplicitlyDeclared) return;
            INamedTypeSymbol behaviour = BehaviourBase(method.ContainingType);
            if (behaviour == null) return;
            // The callbacks: Behaviour's virtual On... methods.
            string[] callbacks = behaviour.GetMembers().OfType<IMethodSymbol>()
                .Where(m => m.IsVirtual && m.Name.StartsWith("On", StringComparison.Ordinal)).Select(m => m.Name).Distinct().ToArray();
            string name = method.Name;
            string reason = null, callback = name;
            if (callbacks.Contains(name))
                reason = $"it hides Behaviour.{name}; declare it 'public override'";
            else if (name.StartsWith("On", StringComparison.Ordinal))
            {
                string near = callbacks.FirstOrDefault(c => string.Equals(c, name, StringComparison.OrdinalIgnoreCase) || Distance(c, name) <= 2);
                if (near != null) reason = $"did you mean the callback {near}?";
                callback = near;
            }
            // (the callback meant, for the quick fix)
            var properties = ImmutableDictionary<string, string>.Empty.Add("Callback", callback);
            if (reason != null) context.ReportDiagnostic(Diagnostic.Create(Callback, method.Locations[0], properties, name, reason));
        }

        private static int Distance(string a, string b)
        {
            if (Math.Abs(a.Length - b.Length) > 2) return 3;
            var d = new int[a.Length + 1, b.Length + 1];
            for (int i = 0; i <= a.Length; ++i) d[i, 0] = i;
            for (int j = 0; j <= b.Length; ++j) d[0, j] = j;
            for (int i = 1; i <= a.Length; ++i)
                for (int j = 1; j <= b.Length; ++j)
                    d[i, j] = Math.Min(Math.Min(d[i - 1, j] + 1, d[i, j - 1] + 1), d[i - 1, j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1));
            return d[a.Length, b.Length];
        }

        // DIM002: `var x = new NativeArray<T>(..., Allocator.Temp/TempJob)` with x never disposed, returned, stored or
        // passed on in its method.
        private static void CheckDisposal(SyntaxNodeAnalysisContext context)
        {
            var declaration = (LocalDeclarationStatementSyntax)context.Node;
            if (declaration.UsingKeyword != default) return;
            SyntaxNode body = declaration.FirstAncestorOrSelf<SyntaxNode>(n => n is BaseMethodDeclarationSyntax || n is LocalFunctionStatementSyntax ||
                                                                               n is AnonymousFunctionExpressionSyntax || n is AccessorDeclarationSyntax);
            if (body == null) return;
            foreach (VariableDeclaratorSyntax variable in declaration.Declaration.Variables)
            {
                if (!(variable.Initializer?.Value is ObjectCreationExpressionSyntax creation)) continue;
                ITypeSymbol type = context.SemanticModel.GetTypeInfo(creation, context.CancellationToken).Type;
                if (type == null || !s_Containers.Any(c => IsEngineType(type, c))) continue;
                string allocator = creation.ArgumentList?.Arguments
                    .Select(a => a.Expression).OfType<MemberAccessExpressionSyntax>()
                    .Where(m => m.Expression is IdentifierNameSyntax { Identifier.Text: "Allocator" })
                    .Select(m => m.Name.Identifier.Text).FirstOrDefault();
                if (allocator != "Temp" && allocator != "TempJob") continue; // (Persistent ones live on: fields, OnDestroy)
                ISymbol local = context.SemanticModel.GetDeclaredSymbol(variable, context.CancellationToken);
                if (local == null || Escapes(body, local, context.SemanticModel)) continue;
                context.ReportDiagnostic(Diagnostic.Create(NotDisposed, variable.Identifier.GetLocation(), variable.Identifier.Text, type.Name, allocator));
            }
        }

        // Whether the method disposes the local or hands it on (returns it, stores it, passes it to a call).
        private static bool Escapes(SyntaxNode body, ISymbol local, SemanticModel model)
        {
            foreach (IdentifierNameSyntax use in body.DescendantNodes().OfType<IdentifierNameSyntax>())
            {
                if (use.Identifier.Text != local.Name || !SymbolEqualityComparer.Default.Equals(model.GetSymbolInfo(use).Symbol, local)) continue;
                SyntaxNode parent = use.Parent;
                if (parent is MemberAccessExpressionSyntax access && access.Expression == use && access.Name.Identifier.Text == "Dispose") return true;
                if (parent is ReturnStatementSyntax || parent is ArrowExpressionClauseSyntax) return true;
                if (parent is AssignmentExpressionSyntax assign && assign.Right == use) return true;
                if (parent is ArgumentSyntax) return true;
                if (parent is UsingStatementSyntax) return true;
                if (parent is InitializerExpressionSyntax || parent is AssignmentExpressionSyntax { Parent: InitializerExpressionSyntax }) return true; // new Job { A = list }
                if (parent is EqualsValueClauseSyntax) return true; // var b = a;
            }
            return false;
        }

        // DIM003: engine objects in a job's Execute (a struct implementing a job interface).
        private static void CheckJob(SyntaxNodeAnalysisContext context)
        {
            var method = (MethodDeclarationSyntax)context.Node;
            if (method.Identifier.Text != "Execute" || !(method.Parent is StructDeclarationSyntax structDecl)) return;
            INamedTypeSymbol type = context.SemanticModel.GetDeclaredSymbol(structDecl, context.CancellationToken);
            if (type == null || !type.AllInterfaces.Any(i => s_JobInterfaces.Any(j => IsEngineType(i, j)))) return;
            var reported = new HashSet<string>();
            foreach (SyntaxNode node in method.DescendantNodes())
            {
                if (!(node is IdentifierNameSyntax || node is MemberAccessExpressionSyntax)) continue;
                // Once per use: the whole member access (Time.DeltaTime), not its parts; not a declaration's type.
                if (node.Parent is MemberAccessExpressionSyntax) continue;
                if (node.Parent is VariableDeclarationSyntax decl && decl.Type == node) continue;
                ITypeSymbol used = context.SemanticModel.GetTypeInfo(node, context.CancellationToken).Type;
                ISymbol symbol = context.SemanticModel.GetSymbolInfo(node, context.CancellationToken).Symbol;
                ITypeSymbol owner = symbol is { IsStatic: true } && !(symbol is ITypeSymbol) ? symbol.ContainingType : null;
                ITypeSymbol hit = new[] { used, owner }.FirstOrDefault(t => t != null && IsMainThreadType(t));
                if (hit == null) continue;
                string text = node.ToString();
                if (reported.Add(text)) context.ReportDiagnostic(Diagnostic.Create(EngineInJob, node.GetLocation(), text));
            }
        }

        private static bool IsMainThreadType(ITypeSymbol type)
        {
            for (ITypeSymbol t = type; t != null; t = t.BaseType)
                if (s_MainThreadTypes.Any(n => IsEngineType(t, n))) return true;
            return false;
        }
    }
}
