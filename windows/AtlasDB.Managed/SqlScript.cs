using System.Text;

namespace AtlasDB.Managed;

/// <summary>Split scripts outside quoted text/identifiers and SQL comments.</summary>
public static class SqlScript
{
    public static IReadOnlyList<string> Split(string script, bool includeTail = true)
    {
        ArgumentNullException.ThrowIfNull(script);
        List<string> statements = [];
        int start = 0;
        char quote = '\0';
        bool line = false, block = false;
        for (int i = 0; i < script.Length; i++)
        {
            char ch = script[i], next = i + 1 < script.Length ? script[i + 1] : '\0';
            if (line) { if (ch == '\n') line = false; continue; }
            if (block) { if (ch == '*' && next == '/') { block = false; i++; } continue; }
            if (quote != '\0')
            {
                if (ch == quote) { if (next == quote) i++; else quote = '\0'; }
                continue;
            }
            if (ch == '-' && next == '-') { line = true; i++; }
            else if (ch == '/' && next == '*') { block = true; i++; }
            else if (ch is '\'' or '"') quote = ch;
            else if (ch == ';')
            {
                string statement = script[start..(i + 1)];
                if (HasSql(statement)) statements.Add(statement);
                start = i + 1;
            }
        }
        if (includeTail && HasSql(script[start..])) statements.Add(script[start..]);
        return statements;
    }
    public static bool HasSql(string text)
    {
        bool line = false, block = false;
        for (int i = 0; i < text.Length; i++)
        {
            char ch = text[i], next = i + 1 < text.Length ? text[i + 1] : '\0';
            if (line) { if (ch == '\n') line = false; continue; }
            if (block) { if (ch == '*' && next == '/') { block = false; i++; } continue; }
            if (ch == '-' && next == '-') { line = true; i++; }
            else if (ch == '/' && next == '*') { block = true; i++; }
            else if (!char.IsWhiteSpace(ch) && ch != ';') return true;
        }
        return false;
    }
    public static bool IsComplete(string script)
    {
        // Compare complete statements with a full split to detect an unfinished tail.
        var complete = Split(script, false);
        return complete.Count > 0 && complete.Count == Split(script).Count;
    }
}
