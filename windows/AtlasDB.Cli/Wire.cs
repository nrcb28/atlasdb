using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using AtlasDB.Managed;

namespace AtlasDB.Cli;

internal static class Wire
{
    internal static readonly UTF8Encoding Utf8 = new(false, true);
    internal static string Failure(int code, string message) => JsonSerializer.Serialize(new { ok = false, code, offset = 0, error = message });
    internal static async Task<string?> ReadLine(Stream stream, int limit, CancellationToken token)
    {
        // A buffered stream wraps the connection to avoid one socket read per byte.
        using MemoryStream buffer = new();
        byte[] one = new byte[1];
        while (true)
        {
            int count = await stream.ReadAsync(one, token);
            if (count == 0)
            {
                if (buffer.Length == 0) return null;
                throw new IOException("La conexion termino antes del salto de linea.");
            }
            if (one[0] == (byte)'\n') return Utf8.GetString(buffer.GetBuffer(), 0, (int)buffer.Length).TrimEnd('\r');
            if (buffer.Length >= limit) throw new InvalidDataException("La linea supera el limite permitido.");
            buffer.WriteByte(one[0]);
        }
    }
    internal static async Task WriteLine(Stream stream, string text, CancellationToken token)
    {
        await stream.WriteAsync(Utf8.GetBytes(text + "\n"), token);
        await stream.FlushAsync(token);
    }
    internal static async Task<int> Client(string address, string sql, CancellationToken token)
    {
        Uri uri = new("tcp://" + address);
        if (uri.Port is < 1 or > 65535) throw new ArgumentException("Indica host:puerto.");
        if (sql.Contains('\n') || sql.Contains('\r')) throw new ArgumentException("TCP requiere una sentencia por linea.");
        if (Utf8.GetByteCount(sql) > Session.MaxSqlBytes) throw new ArgumentException("SQL supera 1 MiB.");
        using TcpClient client = new();
        await client.ConnectAsync(uri.Host, uri.Port, token);
        using BufferedStream stream = new(client.GetStream());
        await WriteLine(stream, sql, token);
        // JSON can be larger than the 64 MiB native result budget due to escaping.
        string json = await ReadLine(stream, Session.MaxJsonBytes, token) ?? throw new IOException("Respuesta vacia.");
        Console.WriteLine(json);
        using JsonDocument document = JsonDocument.Parse(json);
        return document.RootElement.GetProperty("ok").GetBoolean() ? 0 : 1;
    }
    internal static async Task Serve(Database database, IPAddress host, int port, int maxClients,
        bool insecureRemote, CancellationToken token)
    {
        if (!insecureRemote && !IPAddress.IsLoopback(host))
            throw new ArgumentException("Por seguridad, el servidor solo puede escuchar en loopback. Usa --insecure-remote únicamente detrás de una red protegida.");
        TcpListener listener = new(host, port);
        HashSet<Task> active = [];
        listener.Start(maxClients);
        Console.Error.WriteLine($"AtlasDB C# escuchando en {listener.LocalEndpoint}; maximo {maxClients} clientes");
        try
        {
            while (!token.IsCancellationRequested)
            {
                TcpClient client;
                try { client = await listener.AcceptTcpClientAsync(token); }
                catch (OperationCanceledException) { break; }
                active.RemoveWhere(task => task.IsCompleted);
                if (active.Count >= maxClients)
                {
                    // Close promptly: a saturated service cannot reserve another worker.
                    client.Dispose();
                    continue;
                }
                active.Add(Handle(client, database, token));
            }
        }
        finally
        {
            listener.Stop();
            await Task.WhenAll(active);
        }
    }
    private static async Task Handle(TcpClient client, Database database, CancellationToken shutdown)
    {
        using (client)
        using (Session session = database.CreateSession())
        using (BufferedStream stream = new(client.GetStream()))
        {
            try
            {
                while (!shutdown.IsCancellationRequested)
                {
                    using CancellationTokenSource timeout = CancellationTokenSource.CreateLinkedTokenSource(shutdown);
                    timeout.CancelAfter(TimeSpan.FromSeconds(30));
                    string? sql = await ReadLine(stream, Session.MaxSqlBytes, timeout.Token);
                    if (sql is null) return;
                    string result;
                    try { result = session.ExecuteJson(sql); }
                    catch (ArgumentException error) { result = Failure(1, error.Message); }
                    await WriteLine(stream, result, timeout.Token);
                }
            }
            catch (InvalidDataException error)
            {
                try
                {
                    using CancellationTokenSource timeout = new(TimeSpan.FromSeconds(2));
                    await WriteLine(stream, Failure(9, error.Message), timeout.Token);
                }
                catch (Exception failure) when (failure is IOException or OperationCanceledException) { }
            }
            catch (DecoderFallbackException)
            {
                // Invalid UTF-8 terminates the connection without running any SQL.
            }
            catch (Exception error) when (error is IOException or OperationCanceledException or SocketException) { }
        }
    }
}
