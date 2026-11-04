using System.Runtime.InteropServices;
using System.Runtime.CompilerServices;
using Microsoft.Win32.SafeHandles;

namespace AtlasDB.Managed;

internal static partial class Native
{
    private const string Library = "atlasdb_native";
    [LibraryImport(Library, EntryPoint = "atlas_open", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial IntPtr atlas_open(string path, out IntPtr error);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void atlas_close(IntPtr database);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr atlas_session_create(DatabaseHandle database, out IntPtr error);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void atlas_session_destroy(IntPtr session);
    [LibraryImport(Library, EntryPoint = "atlas_execute", StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial IntPtr atlas_execute(SessionHandle session, string sql);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void atlas_free(IntPtr pointer);

    internal static string TakeString(IntPtr value)
    {
        if (value == IntPtr.Zero) throw new InvalidOperationException("El motor no pudo crear el resultado JSON.");
        try { return Marshal.PtrToStringUTF8(value) ?? throw new InvalidDataException("Respuesta UTF-8 nula."); }
        finally { atlas_free(value); }
    }
}

internal sealed class DatabaseHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    internal DatabaseHandle(IntPtr value) : base(true) => SetHandle(value);
    protected override bool ReleaseHandle() { Native.atlas_close(handle); return true; }
}

internal sealed class SessionHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    private readonly DatabaseHandle database;
    internal SessionHandle(IntPtr value, DatabaseHandle database) : base(true)
    {
        this.database = database;
        bool added = false;
        try { database.DangerousAddRef(ref added); SetHandle(value); }
        catch
        {
            Native.atlas_session_destroy(value);
            if (added) database.DangerousRelease();
            throw;
        }
    }
    protected override bool ReleaseHandle()
    {
        Native.atlas_session_destroy(handle);
        database.DangerousRelease();
        return true;
    }
}
