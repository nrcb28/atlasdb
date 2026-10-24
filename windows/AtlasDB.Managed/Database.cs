using System.Text;
using System.Text.Json;

namespace AtlasDB.Managed;

public sealed class AtlasException : Exception
{
    public int Code { get; }
    public long Offset { get; }
    public AtlasException(string json) : this(JsonDocument.Parse(json)) { }
    private AtlasException(JsonDocument document) : base(document.RootElement.GetProperty("error").GetString())
    {
        using (document)
        {
            Code = document.RootElement.GetProperty("code").GetInt32();
            Offset = document.RootElement.GetProperty("offset").GetInt64();
        }
    }
}

/// <summary>A managed owner of an AtlasDB database. Dispose sessions when finished.</summary>
public sealed class Database : IDisposable
{
    private readonly object gate = new();
    private readonly DatabaseHandle handle;
    private bool disposed;
    public Database(string path = ":memory:")
    {
        ArgumentException.ThrowIfNullOrEmpty(path);
        RejectNull(path);
        IntPtr pointer = Native.atlas_open(path, out IntPtr error);
        if (pointer == IntPtr.Zero)
        {
            if (error == IntPtr.Zero) throw new OutOfMemoryException();
            throw new AtlasException(Native.TakeString(error));
        }
        handle = new DatabaseHandle(pointer);
    }
    public Session CreateSession()
    {
        lock (gate)
        {
            ObjectDisposedException.ThrowIf(disposed, this);
            IntPtr pointer = Native.atlas_session_create(handle, out IntPtr error);
            if (pointer == IntPtr.Zero)
            {
                if (error == IntPtr.Zero) throw new OutOfMemoryException();
                throw new AtlasException(Native.TakeString(error));
            }
            return new Session(new SessionHandle(pointer, handle));
        }
    }
    public void Dispose()
    {
        lock (gate) { if (disposed) return; disposed = true; handle.Dispose(); }
    }
    internal static void RejectNull(string text)
    {
        if (text.Contains('\0')) throw new ArgumentException("No se permiten caracteres NUL.");
    }
}

/// <summary>Calls on the same session are serialized. A transaction belongs to this session.</summary>
public sealed class Session : IDisposable
{
    public const int MaxSqlBytes = 1024 * 1024;
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);
    private readonly object gate = new();
    private readonly SessionHandle handle;
    private bool disposed;
    internal Session(SessionHandle handle) => this.handle = handle;
    /// <summary>Return a JSON object with ok, columns, rows, affected and message, or a SQL error.</summary>
    public string ExecuteJson(string sql)
    {
        ArgumentNullException.ThrowIfNull(sql);
        Database.RejectNull(sql);
        if (StrictUtf8.GetByteCount(sql) > MaxSqlBytes) throw new ArgumentException("SQL supera 1 MiB.");
        lock (gate)
        {
            ObjectDisposedException.ThrowIf(disposed, this);
            return Native.TakeString(Native.atlas_execute(handle, sql));
        }
    }
    /// <summary>Execute one statement; throw AtlasException for a SQL error.</summary>
    public JsonElement Execute(string sql)
    {
        string json = ExecuteJson(sql);
        using JsonDocument document = JsonDocument.Parse(json);
        if (!document.RootElement.GetProperty("ok").GetBoolean()) throw new AtlasException(json);
        return document.RootElement.Clone();
    }
    public void Dispose()
    {
        lock (gate) { if (disposed) return; disposed = true; handle.Dispose(); }
    }
}
