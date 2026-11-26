using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Xml.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace Dimension.CodeService
{
    // What Code Mode asks about a place in a file: what can be typed there (completion), the call it is in (signature
    // help) and what the name under the mouse is (hover). Answered from the compilation's semantic model.
    internal static class Queries
    {
        // ---- Completion

        public static void Completion(SemanticModel model, int position, Utf8JsonWriter w)
        {
            var tree = model.SyntaxTree;
            var root = tree.GetRoot();
            var text = tree.GetText();
            // The word being typed starts here (completion replaces it).
            int start = position;
            while (start > 0 && IsIdentifierChar(text[start - 1])) --start;
            w.WriteStartArray("items");
            if (InCommentOrString(root, start, position))
            {
                w.WriteEndArray();
                return;
            }
            // Naming something new ("int spe|", "class Fo|"): nothing to offer.
            var word = root.FindToken(start);
            if (start < position && word.IsKind(SyntaxKind.IdentifierToken) && IsDeclaredName(word))
            {
                w.WriteEndArray();
                return;
            }
            var items = new Dictionary<string, Item>();
            int before = start - 1;
            while (before >= 0 && (text[before] == ' ' || text[before] == '\t')) --before;
            var prev = start > 0 ? root.FindToken(Math.Max(0, before)) : default;

            if (before >= 0 && text[before] == '.' && prev.IsKind(SyntaxKind.DotToken))
                MemberItems(model, prev, start, items);
            else if (prev.IsKind(SyntaxKind.OverrideKeyword))
                OverrideItems(model, prev, items);
            else
            {
                foreach (var symbol in model.LookupSymbols(start))
                    Add(items, symbol, model, start);
                foreach (string keyword in Keywords)
                    if (!items.ContainsKey(keyword)) items[keyword] = new Item { Label = keyword, Kind = "keyword" };
            }
            foreach (var item in items.Values.OrderBy(i => i.Label, StringComparer.OrdinalIgnoreCase))
            {
                w.WriteStartObject();
                w.WriteString("label", item.Label);
                w.WriteString("kind", item.Kind);
                if (item.Symbol != null)
                {
                    string detail = Display(item.Symbol, model, start);
                    if (item.Overloads > 1) detail += $" (+{item.Overloads - 1} overload{(item.Overloads > 2 ? "s" : "")})";
                    w.WriteString("detail", detail);
                    string doc = Summary(item.Symbol);
                    if (doc.Length > 0) w.WriteString("doc", doc);
                }
                if (item.Insert != null) w.WriteString("insert", item.Insert);
                w.WriteEndObject();
            }
            w.WriteEndArray();
        }

        private sealed class Item
        {
            public string Label, Kind, Insert;
            public ISymbol Symbol;
            public int Overloads;
        }

        // After "x.": x's members (a namespace's types, a type's static members, a value's instance members and the
        // extension methods that apply).
        private static void MemberItems(SemanticModel model, SyntaxToken dot, int position, Dictionary<string, Item> items)
        {
            ExpressionSyntax left = dot.Parent switch
            {
                MemberAccessExpressionSyntax m => m.Expression,
                QualifiedNameSyntax q => q.Left,
                _ => null,
            };
            if (left == null) return;
            var info = model.GetSymbolInfo(left);
            var symbol = info.Symbol ?? info.CandidateSymbols.FirstOrDefault();
            if (symbol is INamespaceSymbol ns)
            {
                foreach (var s in model.LookupNamespacesAndTypes(position, ns)) Add(items, s, model, position);
                return;
            }
            var type = model.GetTypeInfo(left).Type;
            if (symbol is INamedTypeSymbol named && (type == null || !SymbolEqualityComparer.Default.Equals(named, type) || IsTypeReference(left, model)))
            {
                foreach (var s in model.LookupStaticMembers(position, named)) Add(items, s, model, position);
                foreach (var s in named.GetTypeMembers()) Add(items, s, model, position);
                return;
            }
            if (type == null || type.TypeKind == TypeKind.Error) return;
            foreach (var s in model.LookupSymbols(position, type, includeReducedExtensionMethods: true))
                if (!s.IsStatic || s is IMethodSymbol { MethodKind: MethodKind.ReducedExtension }) Add(items, s, model, position);
        }

        // "Color.Red" (the type) or "Color.r" (a field named after its type) - the name binds to the type itself.
        private static bool IsTypeReference(ExpressionSyntax left, SemanticModel model)
        {
            var symbol = model.GetSymbolInfo(left).Symbol;
            return symbol is ITypeSymbol;
        }

        // After "override": the base classes' members still to override, with their whole declaration.
        private static void OverrideItems(SemanticModel model, SyntaxToken overrideToken, Dictionary<string, Item> items)
        {
            var typeDecl = overrideToken.Parent?.AncestorsAndSelf().OfType<TypeDeclarationSyntax>().FirstOrDefault();
            if (typeDecl == null || model.GetDeclaredSymbol(typeDecl) is not INamedTypeSymbol type) return;
            var done = new HashSet<string>(type.GetMembers().Where(m => m.IsOverride).Select(m => Signature(m)));
            for (var b = type.BaseType; b != null; b = b.BaseType)
                foreach (var m in b.GetMembers())
                {
                    if (!(m.IsVirtual || m.IsAbstract || m.IsOverride) || m.IsSealed || m.IsStatic) continue;
                    if (m is IMethodSymbol { MethodKind: not MethodKind.Ordinary }) continue;
                    if (m.DeclaredAccessibility is Accessibility.Private) continue;
                    if (!done.Add(Signature(m))) continue;
                    string insert = OverrideText(m, model, overrideToken.SpanStart);
                    if (insert == null) continue;
                    string label = m is IMethodSymbol method ? m.Name + "(" + string.Join(", ", method.Parameters.Select(p => p.Type.ToMinimalDisplayString(model, overrideToken.SpanStart))) + ")" : m.Name;
                    items[label] = new Item { Label = label, Kind = KindOf(m), Symbol = m, Overloads = 1, Insert = insert };
                }
        }

        private static string Signature(ISymbol m) =>
            m is IMethodSymbol method ? m.Name + "(" + string.Join(",", method.Parameters.Select(p => p.Type.ToDisplayString())) + ")" : m.Name;

        // "void OnUpdate(float dt)\n{\n    $0\n}" ($0: where the cursor goes; four spaces: one indent).
        private static string OverrideText(ISymbol m, SemanticModel model, int position)
        {
            string Type(ITypeSymbol t) => t.ToMinimalDisplayString(model, position);
            switch (m)
            {
                case IMethodSymbol method:
                {
                    string parameters = string.Join(", ", method.Parameters.Select(p => (p.RefKind switch
                    {
                        RefKind.Ref => "ref ",
                        RefKind.Out => "out ",
                        RefKind.In => "in ",
                        _ => "",
                    }) + Type(p.Type) + " " + p.Name));
                    string call = method.Name + "(" + string.Join(", ", method.Parameters.Select(p => (p.RefKind == RefKind.Ref ? "ref " : p.RefKind == RefKind.Out ? "out " : "") + p.Name)) + ")";
                    string body = method.IsAbstract ? "$0" : method.ReturnsVoid ? "base." + call + ";$0" : "return base." + call + ";$0";
                    return (method.ReturnsVoid ? "void" : Type(method.ReturnType)) + " " + method.Name + "(" + parameters + ")\n{\n    " + body + "\n}";
                }
                case IPropertySymbol property:
                {
                    string accessors = (property.GetMethod != null ? "get => " + (property.IsAbstract ? "throw new System.NotImplementedException()" : "base." + property.Name) + "; " : "") +
                                       (property.SetMethod != null ? "set => " + (property.IsAbstract ? "throw new System.NotImplementedException()" : "base." + property.Name + " = value") + "; " : "");
                    return Type(property.Type) + " " + property.Name + " { " + accessors + "}$0";
                }
                default:
                    return null;
            }
        }

        private static void Add(Dictionary<string, Item> items, ISymbol symbol, SemanticModel model, int position)
        {
            if (!symbol.CanBeReferencedByName || symbol.IsImplicitlyDeclared && symbol is not IParameterSymbol) return;
            if (symbol is IMethodSymbol m && m.MethodKind is not (MethodKind.Ordinary or MethodKind.ReducedExtension or MethodKind.LocalFunction or MethodKind.DelegateInvoke))
                return;
            if (symbol.GetAttributes().Any(a => a.AttributeClass?.Name == "ObsoleteAttribute" && a.ConstructorArguments.Length > 1 && a.ConstructorArguments[1].Value is true))
                return; // (obsolete as an error)
            string label = symbol.Name;
            if (items.TryGetValue(label, out var existing))
            {
                if (existing.Symbol != null && existing.Kind == KindOf(symbol)) existing.Overloads++;
                return;
            }
            items[label] = new Item { Label = label, Kind = KindOf(symbol), Symbol = symbol, Overloads = 1 };
        }

        private static string KindOf(ISymbol s) => s switch
        {
            INamespaceSymbol => "namespace",
            INamedTypeSymbol t => t.TypeKind switch
            {
                TypeKind.Interface => "interface",
                TypeKind.Struct => "struct",
                TypeKind.Enum => "enum",
                TypeKind.Delegate => "delegate",
                _ => "class",
            },
            IMethodSymbol => "method",
            IPropertySymbol => "property",
            IFieldSymbol f => f.ContainingType?.TypeKind == TypeKind.Enum ? "enumMember" : f.IsConst ? "constant" : "field",
            IEventSymbol => "event",
            ILocalSymbol => "local",
            IParameterSymbol => "parameter",
            ITypeParameterSymbol => "typeParameter",
            _ => "text",
        };

        private static bool IsDeclaredName(SyntaxToken t) => t.Parent switch
        {
            VariableDeclaratorSyntax v => v.Identifier == t,
            ParameterSyntax p => p.Identifier == t && p.Type != null,
            BaseTypeDeclarationSyntax d => d.Identifier == t,
            MethodDeclarationSyntax m => m.Identifier == t,
            PropertyDeclarationSyntax p => p.Identifier == t,
            EventDeclarationSyntax e => e.Identifier == t,
            ForEachStatementSyntax f => f.Identifier == t,
            SingleVariableDesignationSyntax => true,
            LocalFunctionStatementSyntax l => l.Identifier == t,
            EnumMemberDeclarationSyntax e => e.Identifier == t,
            TypeParameterSyntax => true,
            CatchDeclarationSyntax c => c.Identifier == t,
            DelegateDeclarationSyntax d => d.Identifier == t,
            _ => false,
        };

        private static bool IsIdentifierChar(char c) => char.IsLetterOrDigit(c) || c == '_';

        private static bool InCommentOrString(SyntaxNode root, int start, int position)
        {
            int at = Math.Max(0, start - 1);
            var trivia = root.FindTrivia(at);
            if (trivia.IsKind(SyntaxKind.SingleLineCommentTrivia) || trivia.IsKind(SyntaxKind.MultiLineCommentTrivia) ||
                trivia.IsKind(SyntaxKind.SingleLineDocumentationCommentTrivia) || trivia.IsKind(SyntaxKind.MultiLineDocumentationCommentTrivia) ||
                trivia.IsKind(SyntaxKind.DisabledTextTrivia))
                return trivia.Span.Contains(at) || trivia.Span.End == position && !trivia.IsKind(SyntaxKind.MultiLineCommentTrivia);
            var token = root.FindToken(at);
            if (token.IsKind(SyntaxKind.StringLiteralToken) || token.IsKind(SyntaxKind.CharacterLiteralToken) ||
                token.IsKind(SyntaxKind.InterpolatedStringTextToken) || token.IsKind(SyntaxKind.SingleLineRawStringLiteralToken) ||
                token.IsKind(SyntaxKind.MultiLineRawStringLiteralToken))
                return token.Span.Contains(at) || (token.Span.End == position && !token.Text.EndsWith('"') && !token.Text.EndsWith('\''));
            return false;
        }

        private static readonly string[] Keywords =
        {
            "abstract", "as", "async", "await", "base", "bool", "break", "byte", "case", "catch", "char", "class", "const", "continue",
            "decimal", "default", "delegate", "do", "double", "else", "enum", "event", "false", "finally", "fixed", "float", "for",
            "foreach", "get", "if", "in", "int", "interface", "internal", "is", "long", "nameof", "namespace", "new", "null", "object",
            "out", "override", "params", "private", "protected", "public", "readonly", "record", "ref", "return", "sbyte", "sealed",
            "set", "short", "sizeof", "static", "string", "struct", "switch", "this", "throw", "true", "try", "typeof", "uint",
            "ulong", "unsafe", "ushort", "using", "var", "virtual", "void", "volatile", "when", "where", "while", "yield",
        };

        // ---- Signature help

        public static void Signature(SemanticModel model, int position, Utf8JsonWriter w)
        {
            var root = model.SyntaxTree.GetRoot();
            var token = root.FindToken(Math.Max(0, position - 1));
            // The innermost argument list the place is inside (after its "(", up to its ")").
            BaseArgumentListSyntax list = null;
            for (var node = token.Parent; node != null; node = node.Parent)
                if (node is ArgumentListSyntax a && position > a.OpenParenToken.SpanStart &&
                    (a.CloseParenToken.IsMissing || position <= a.CloseParenToken.SpanStart))
                {
                    list = a;
                    break;
                }
            w.WriteStartArray("signatures");
            if (list == null)
            {
                w.WriteEndArray();
                return;
            }
            var methods = new List<IMethodSymbol>();
            ISymbol chosen = null;
            switch (list.Parent)
            {
                case InvocationExpressionSyntax invocation:
                    methods.AddRange(model.GetMemberGroup(invocation.Expression).OfType<IMethodSymbol>());
                    chosen = model.GetSymbolInfo(invocation).Symbol;
                    break;
                case BaseObjectCreationExpressionSyntax creation:
                    if (model.GetTypeInfo(creation).Type is INamedTypeSymbol created)
                        methods.AddRange(created.InstanceConstructors.Where(c => model.IsAccessible(position, c)));
                    chosen = model.GetSymbolInfo(creation).Symbol;
                    break;
            }
            // The argument the place is in: the commas before it.
            int param = list.Arguments.GetSeparators().Count(s => s.SpanStart < position);
            int active = 0;
            if (chosen != null) active = Math.Max(0, methods.FindIndex(m => SymbolEqualityComparer.Default.Equals(m, chosen) || SymbolEqualityComparer.Default.Equals(m.ReducedFrom, chosen)));
            else active = Math.Max(0, methods.FindIndex(m => m.Parameters.Length > param || m.Parameters.LastOrDefault()?.IsParams == true));
            int at = list.SpanStart;
            foreach (var method in methods)
            {
                var label = new StringBuilder();
                if (method.MethodKind == MethodKind.Constructor) label.Append(method.ContainingType.ToMinimalDisplayString(model, at));
                else label.Append(method.ReturnsVoid ? "void" : method.ReturnType.ToMinimalDisplayString(model, at)).Append(' ')
                          .Append(method.ContainingType.ToMinimalDisplayString(model, at)).Append('.').Append(method.Name);
                label.Append('(');
                var spans = new List<(int Start, int Length, IParameterSymbol P)>();
                for (int i = 0; i < method.Parameters.Length; ++i)
                {
                    if (i > 0) label.Append(", ");
                    var p = method.Parameters[i];
                    string shown = p.ToMinimalDisplayString(model, at, ParameterFormat);
                    spans.Add((label.Length, shown.Length, p));
                    label.Append(shown);
                }
                label.Append(')');
                var docs = ParamDocs(method);
                w.WriteStartObject();
                w.WriteString("label", label.ToString());
                w.WriteString("doc", Summary(method));
                w.WriteStartArray("params");
                foreach (var (start, length, p) in spans)
                {
                    w.WriteStartObject();
                    w.WriteNumber("start", start);
                    w.WriteNumber("length", length);
                    w.WriteString("name", p.Name);
                    w.WriteString("doc", docs.TryGetValue(p.Name, out var d) ? d : "");
                    w.WriteEndObject();
                }
                w.WriteEndArray();
                w.WriteEndObject();
            }
            w.WriteEndArray();
            w.WriteNumber("active", active);
            w.WriteNumber("param", param);
        }

        private static readonly SymbolDisplayFormat ParameterFormat = new(
            globalNamespaceStyle: SymbolDisplayGlobalNamespaceStyle.Omitted,
            typeQualificationStyle: SymbolDisplayTypeQualificationStyle.NameAndContainingTypes,
            genericsOptions: SymbolDisplayGenericsOptions.IncludeTypeParameters,
            parameterOptions: SymbolDisplayParameterOptions.IncludeType | SymbolDisplayParameterOptions.IncludeName |
                              SymbolDisplayParameterOptions.IncludeDefaultValue | SymbolDisplayParameterOptions.IncludeModifiers,
            miscellaneousOptions: SymbolDisplayMiscellaneousOptions.UseSpecialTypes | SymbolDisplayMiscellaneousOptions.EscapeKeywordIdentifiers);

        // ---- Hover

        public static void Hover(SemanticModel model, int position, Utf8JsonWriter w)
        {
            var root = model.SyntaxTree.GetRoot();
            var token = root.FindToken(position);
            ISymbol symbol = null;
            if (token.Span.Contains(position) && (token.IsKind(SyntaxKind.IdentifierToken) || SyntaxFacts.IsPredefinedType(token.Kind()) ||
                                                  token.IsKind(SyntaxKind.BaseKeyword) || token.IsKind(SyntaxKind.ThisKeyword)))
            {
                // The name's own node: what it refers to, or what it declares (a method's name, a variable's...).
                var node = token.Parent;
                var info = model.GetSymbolInfo(node);
                symbol = info.Symbol ?? info.CandidateSymbols.FirstOrDefault() ?? model.GetDeclaredSymbol(node);
                if (symbol == null && node.Parent != null && node.Parent is not StatementSyntax)
                {
                    info = model.GetSymbolInfo(node.Parent);
                    symbol = info.Symbol ?? info.CandidateSymbols.FirstOrDefault();
                }
            }
            if (symbol == null) return;
            if (symbol is IMethodSymbol { ReducedFrom: not null } reduced) symbol = reduced;
            w.WriteString("text", Prefix(symbol) + Display(symbol, model, position));
            w.WriteString("doc", Summary(symbol));
        }

        private static string Prefix(ISymbol s) => s switch
        {
            ILocalSymbol l => l.IsConst ? "(local constant) " : "(local variable) ",
            IParameterSymbol => "(parameter) ",
            IFieldSymbol f => f.IsConst ? "(constant) " : "(field) ",
            IPropertySymbol => "(property) ",
            IEventSymbol => "(event) ",
            IMethodSymbol { MethodKind: MethodKind.ReducedExtension } or IMethodSymbol { IsExtensionMethod: true } => "(extension) ",
            IMethodSymbol { MethodKind: MethodKind.LocalFunction } => "(local function) ",
            ITypeParameterSymbol => "(type parameter) ",
            _ => "",
        };

        // ---- Shown text

        private static readonly SymbolDisplayFormat DisplayFormat = new(
            globalNamespaceStyle: SymbolDisplayGlobalNamespaceStyle.Omitted,
            typeQualificationStyle: SymbolDisplayTypeQualificationStyle.NameAndContainingTypes,
            genericsOptions: SymbolDisplayGenericsOptions.IncludeTypeParameters | SymbolDisplayGenericsOptions.IncludeVariance,
            memberOptions: SymbolDisplayMemberOptions.IncludeParameters | SymbolDisplayMemberOptions.IncludeType | SymbolDisplayMemberOptions.IncludeContainingType |
                           SymbolDisplayMemberOptions.IncludeRef | SymbolDisplayMemberOptions.IncludeConstantValue,
            kindOptions: SymbolDisplayKindOptions.IncludeTypeKeyword | SymbolDisplayKindOptions.IncludeNamespaceKeyword,
            parameterOptions: SymbolDisplayParameterOptions.IncludeType | SymbolDisplayParameterOptions.IncludeName |
                              SymbolDisplayParameterOptions.IncludeDefaultValue | SymbolDisplayParameterOptions.IncludeModifiers,
            propertyStyle: SymbolDisplayPropertyStyle.ShowReadWriteDescriptor,
            localOptions: SymbolDisplayLocalOptions.IncludeType | SymbolDisplayLocalOptions.IncludeConstantValue,
            miscellaneousOptions: SymbolDisplayMiscellaneousOptions.UseSpecialTypes | SymbolDisplayMiscellaneousOptions.EscapeKeywordIdentifiers |
                                  SymbolDisplayMiscellaneousOptions.UseErrorTypeSymbolName);

        private static string Display(ISymbol symbol, SemanticModel model, int position)
        {
            try
            {
                return symbol.ToMinimalDisplayString(model, position, DisplayFormat);
            }
            catch (Exception)
            {
                return symbol.ToDisplayString(DisplayFormat);
            }
        }

        // The <summary> of a symbol's XML documentation as plain text ("" without any).
        public static string Summary(ISymbol symbol)
        {
            var doc = Documentation(symbol);
            var summary = doc?.Element("summary");
            return summary == null ? "" : PlainText(summary);
        }

        private static Dictionary<string, string> ParamDocs(ISymbol symbol)
        {
            var result = new Dictionary<string, string>();
            var doc = Documentation(symbol);
            if (doc != null)
                foreach (var p in doc.Elements("param"))
                    if (p.Attribute("name")?.Value is { } name) result[name] = PlainText(p);
            return result;
        }

        private static XElement Documentation(ISymbol symbol)
        {
            if (symbol is IMethodSymbol { ReducedFrom: { } from }) symbol = from;
            string xml = symbol.GetDocumentationCommentXml(expandIncludes: false);
            if (string.IsNullOrWhiteSpace(xml)) return null;
            try
            {
                return XElement.Parse(xml.TrimStart().StartsWith("<member") || xml.TrimStart().StartsWith("<doc") ? xml : "<member>" + xml + "</member>");
            }
            catch (Exception)
            {
                return null;
            }
        }

        private static string PlainText(XElement e)
        {
            var sb = new StringBuilder();
            foreach (var node in e.Nodes())
            {
                if (node is XText t) sb.Append(t.Value);
                else if (node is XElement x)
                {
                    string cref = x.Attribute("cref")?.Value ?? x.Attribute("langword")?.Value ?? x.Attribute("name")?.Value;
                    if (x.Name.LocalName is "see" or "seealso" or "paramref" or "typeparamref" && cref != null && x.IsEmpty)
                    {
                        // "T:Dimension.Transform" -> Transform, "M:Dimension.Transform.Translate(...)" -> Translate
                        string name = cref.Contains(':') ? cref[(cref.IndexOf(':') + 1)..] : cref;
                        int paren = name.IndexOf('(');
                        if (paren >= 0) name = name[..paren];
                        int dot = name.LastIndexOf('.');
                        sb.Append(dot >= 0 ? name[(dot + 1)..] : name);
                    }
                    else if (x.Name.LocalName == "para") sb.Append('\n').Append(PlainText(x)).Append('\n');
                    else sb.Append(PlainText(x));
                }
            }
            // Whitespace as one space (the XML's line breaks and indentation), paragraphs kept.
            var lines = sb.ToString().Split('\n').Select(l => string.Join(' ', l.Split((char[])null, StringSplitOptions.RemoveEmptyEntries)));
            return string.Join("\n", lines.Where(l => l.Length > 0)).Trim();
        }
    }
}
