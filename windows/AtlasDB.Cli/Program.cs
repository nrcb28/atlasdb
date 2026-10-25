using System.Globalization;
using System.Net;
using System.Text;
using System.Text.Json;
using AtlasDB.Managed;
using AtlasDB.Cli;

CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;
CultureInfo.CurrentUICulture = CultureInfo.InvariantCulture;
Console.InputEncoding = new UTF8Encoding(false);
Console.OutputEncoding = new UTF8Encoding(false);
return await Run(args);

static async Task<int> Run(string[] args)
{
    try
    {
        var options = Options.Parse(args);
        if (options.Help) { Help(); return 0; }
        using CancellationTokenSource shutdown = new();
        Console.CancelKeyPress += (_, e) => { e.Cancel = true; shutdown.Cancel(); };
        if (options.Client is not null)
        {
            shutdown.CancelAfter(TimeSpan.FromSeconds(30));
            return await Wire.Client(options.Client, options.Execute!, shutdown.Token);
        }
        using Database db = new(options.Path);
        if (options.Port is int port)
        {
            await Wire.Serve(db, IPAddress.Parse(options.Host), port, options.MaxClients,
                options.InsecureRemote, shutdown.Token);
            return 0;
        }
        using Session session = db.CreateSession();
        if (options.Execute is string sql) return Script(session, sql, options.Json);
        if (options.File is string file) return Script(session, File.ReadAllText(file, Wire.Utf8), options.Json);
        StringBuilder pending = new();
        bool interactive = !Console.IsInputRedirected;
        if (interactive) Console.WriteLine("AtlasDB para Windows / C# · .help para ayuda");
        int exitCode = 0;
        while (!shutdown.IsCancellationRequested)
        {
            if (interactive) Console.Write(pending.Length == 0 ? "atlas> " : "   ... ");
            string? line = await Console.In.ReadLineAsync(shutdown.Token);
            if (line is null) break;
            if (pending.Length == 0 && line.StartsWith('.'))
            {
                if (line.Trim() is ".quit" or ".exit") break;
                string? command = line.Trim() switch
                {
                    ".help" => null,
                    ".tables" => "SHOW TABLES;",
                    ".checkpoint" => "CHECKPOINT;",
                    _ when line.StartsWith(".schema ") => "DESCRIBE \"" + line[8..].Trim().Replace("\"", "\"\"") + "\";",
                    _ => ""
                };
                if (command is null) Help();
                else if (command.Length == 0) { Console.Error.WriteLine("Comando desconocido."); exitCode = 1; }
                else exitCode |= Script(session, command, options.Json);
                continue;
            }
            pending.AppendLine(line);
            if (Wire.Utf8.GetByteCount(pending.ToString()) > Session.MaxSqlBytes)
            {
                Console.Error.WriteLine("Entrada supera 1 MiB. Se descarta el bloque pendiente.");
                pending.Clear(); exitCode = 1; continue;
            }
            if (SqlScript.IsComplete(pending.ToString()))
            {
                exitCode |= Script(session, pending.ToString(), options.Json);
                pending.Clear();
            }
            else if (!SqlScript.HasSql(pending.ToString())) pending.Clear();
        }
        if (SqlScript.HasSql(pending.ToString())) exitCode |= Script(session, pending.ToString(), options.Json);
        return exitCode;
    }
    catch (OperationCanceledException) { return 130; }
    catch (Exception error) when (error is AtlasException or ArgumentException or IOException or
        System.Net.Sockets.SocketException or FormatException or DllNotFoundException or BadImageFormatException or DecoderFallbackException)
    {
        Console.Error.WriteLine(error.Message); return 1;
    }
}
static int Script(Session session, string text, bool json)
{
    foreach (string sql in SqlScript.Split(text))
    {
        string result = session.ExecuteJson(sql);
        using JsonDocument document = JsonDocument.Parse(result);
        var root = document.RootElement;
        if (json) Console.WriteLine(result);
        else if (!root.GetProperty("ok").GetBoolean()) Console.Error.WriteLine($"Error {root.GetProperty("code")}: {root.GetProperty("error")}");
        else
        {
            var columns = root.GetProperty("columns");
            if (columns.GetArrayLength() > 0)
            {
                Console.WriteLine(string.Join(" | ", columns.EnumerateArray().Select(x => x.GetString())));
                foreach (var row in root.GetProperty("rows").EnumerateArray()) Console.WriteLine(row.GetRawText());
            }
            else Console.WriteLine(root.GetProperty("message").GetString());
        }
        if (!root.GetProperty("ok").GetBoolean()) return 1; // Stop scripts on first failure.
    }
    return 0;
}
static void Help() => Console.WriteLine("""
AtlasDB C# / Windows x64
  AtlasDB.exe --db .\datos
  AtlasDB.exe --file .\examples\inventario.sql --json
  AtlasDB.exe --db .\datos --execute 'SHOW TABLES;'
  AtlasDB.exe --db .\datos --server 7373 [--host 127.0.0.1] [--max-clients 32]
               [--insecure-remote]
  AtlasDB.exe --client 127.0.0.1:7373 --execute 'SHOW TABLES;'
  .help  .tables  .schema tabla  .checkpoint  .quit
Una sentencia por llamada a la API o por linea TCP. El modo --file acepta scripts.
""");

sealed class Options
{
    internal string Path = ":memory:", Host = "127.0.0.1";
    internal string? Execute, File, Client;
    internal int? Port;
    internal int MaxClients = 32;
    internal bool Json, Help, InsecureRemote;
    internal static Options Parse(string[] args)
    {
        Options value = new();
        HashSet<string> seen = [];
        for (int i = 0; i < args.Length; i++)
        {
            string flag = args[i];
            if (!seen.Add(flag)) throw new ArgumentException($"Opcion repetida: {flag}");
            string Next() => ++i < args.Length ? args[i] : throw new ArgumentException($"Falta valor: {flag}");
            switch (flag)
            {
                case "--db": value.Path = Next(); break;
                case "--execute": value.Execute = Next(); break;
                case "--file": value.File = Next(); break;
                case "--client": value.Client = Next(); break;
                case "--server": value.Port = int.Parse(Next(), CultureInfo.InvariantCulture); break;
                case "--host": value.Host = Next(); break;
                case "--max-clients": value.MaxClients = int.Parse(Next(), CultureInfo.InvariantCulture); break;
                case "--insecure-remote": value.InsecureRemote = true; break;
                case "--json": value.Json = true; break;
                case "--help": case "-h": value.Help = true; break;
                default: throw new ArgumentException($"Opcion desconocida: {flag}");
            }
        }
        if (value.Port is < 1 or > 65535 || value.MaxClients is < 1 or > 128) throw new ArgumentException("Puerto 1..65535; max-clients 1..128.");
        if ((value.Execute is not null ? 1 : 0) + (value.File is not null ? 1 : 0) + (value.Port is not null ? 1 : 0) > 1)
            throw new ArgumentException("Selecciona --execute, --file o --server.");
        if (value.Client is not null && (value.Execute is null || value.File is not null || value.Port is not null))
            throw new ArgumentException("--client requiere --execute y no admite --file ni --server.");
        if ((seen.Contains("--host") || seen.Contains("--max-clients")) && value.Port is null)
            throw new ArgumentException("--host y --max-clients requieren --server.");
        if (value.InsecureRemote && value.Port is null)
            throw new ArgumentException("--insecure-remote requiere --server.");
        return value;
    }
}
