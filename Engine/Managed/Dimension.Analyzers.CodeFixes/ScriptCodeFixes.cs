using System.Collections.Immutable;
using System.Composition;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CodeActions;
using Microsoft.CodeAnalysis.CodeFixes;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace Dimension.Analyzers
{
    /// <summary>Quick fixes for the script analyzers: DIM001 makes the method the callback it was meant to be (adds
    /// 'public override', renaming a misspelled one); DIM002 disposes the container where its scope ends ('using var').</summary>
    [ExportCodeFixProvider(LanguageNames.CSharp, Name = nameof(ScriptCodeFixes)), Shared]
    public sealed class ScriptCodeFixes : CodeFixProvider
    {
        public override ImmutableArray<string> FixableDiagnosticIds => ImmutableArray.Create(ScriptAnalyzer.Callback.Id, ScriptAnalyzer.NotDisposed.Id);

        public override FixAllProvider GetFixAllProvider() => WellKnownFixAllProviders.BatchFixer;

        public override async Task RegisterCodeFixesAsync(CodeFixContext context)
        {
            SyntaxNode root = await context.Document.GetSyntaxRootAsync(context.CancellationToken).ConfigureAwait(false);
            foreach (Diagnostic diagnostic in context.Diagnostics)
            {
                SyntaxNode node = root.FindNode(diagnostic.Location.SourceSpan);
                if (diagnostic.Id == ScriptAnalyzer.Callback.Id && node.FirstAncestorOrSelf<MethodDeclarationSyntax>() is { } method)
                {
                    string callback = diagnostic.Properties.TryGetValue("Callback", out string c) && !string.IsNullOrEmpty(c) ? c : method.Identifier.Text;
                    string title = callback == method.Identifier.Text ? $"Make it 'public override {callback}'" : $"Rename to the callback '{callback}' (public override)";
                    context.RegisterCodeFix(CodeAction.Create(title, ct => MakeCallback(context.Document, method, callback, ct), "DIM001"), diagnostic);
                }
                else if (diagnostic.Id == ScriptAnalyzer.NotDisposed.Id && node.FirstAncestorOrSelf<LocalDeclarationStatementSyntax>() is { } local)
                {
                    context.RegisterCodeFix(CodeAction.Create("Dispose it at the end of its scope ('using var')", ct => UseUsing(context.Document, local, ct), "DIM002"),
                                            diagnostic);
                }
            }
        }

        private static async Task<Document> MakeCallback(Document document, MethodDeclarationSyntax method, string callback, CancellationToken ct)
        {
            SyntaxNode root = await document.GetSyntaxRootAsync(ct).ConfigureAwait(false);
            // public override, keeping other modifiers (not private / protected / internal / new / virtual).
            var drop = new[] { SyntaxKind.PrivateKeyword, SyntaxKind.ProtectedKeyword, SyntaxKind.InternalKeyword, SyntaxKind.PublicKeyword,
                               SyntaxKind.NewKeyword, SyntaxKind.VirtualKeyword, SyntaxKind.OverrideKeyword };
            SyntaxTriviaList leading = method.Modifiers.Any() ? method.Modifiers.First().LeadingTrivia : method.ReturnType.GetLeadingTrivia();
            var kept = method.Modifiers.Where(m => !drop.Contains(m.Kind())).Select(m => m.WithLeadingTrivia(SyntaxFactory.Space).WithTrailingTrivia(SyntaxFactory.Space));
            SyntaxTokenList modifiers = SyntaxFactory.TokenList(new[]
            {
                SyntaxFactory.Token(SyntaxKind.PublicKeyword).WithLeadingTrivia(leading).WithTrailingTrivia(SyntaxFactory.Space),
                SyntaxFactory.Token(SyntaxKind.OverrideKeyword).WithTrailingTrivia(SyntaxFactory.Space),
            }.Concat(kept));
            MethodDeclarationSyntax fixedMethod = method.WithModifiers(modifiers).WithReturnType(method.ReturnType.WithLeadingTrivia())
                                                        .WithIdentifier(SyntaxFactory.Identifier(callback).WithTriviaFrom(method.Identifier));
            return document.WithSyntaxRoot(root.ReplaceNode(method, fixedMethod));
        }

        private static async Task<Document> UseUsing(Document document, LocalDeclarationStatementSyntax local, CancellationToken ct)
        {
            SyntaxNode root = await document.GetSyntaxRootAsync(ct).ConfigureAwait(false);
            LocalDeclarationStatementSyntax withUsing = local.WithoutLeadingTrivia()
                .WithUsingKeyword(SyntaxFactory.Token(SyntaxKind.UsingKeyword).WithLeadingTrivia(local.GetLeadingTrivia()).WithTrailingTrivia(SyntaxFactory.Space));
            return document.WithSyntaxRoot(root.ReplaceNode(local, withUsing));
        }
    }
}
