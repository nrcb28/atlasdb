using System.Text.Json;
using AtlasDB.Managed;

int checks = 0;
void Check(bool value, string message)
{
    checks++;
    if (!value) throw new Exception($"Fallo {checks}: {message}");
}
void Error(Action action, int expected)
{
    try { action(); throw new Exception("Se esperaba error SQL"); }
    catch (AtlasException failure) { Check(failure.Code == expected, $"Codigo {failure.Code}, esperado {expected}"); }
}
long Scalar(Session session, string sql) => session.Execute(sql).GetProperty("rows")[0][0].GetInt64();
using (Database database = new())
using (Session a = database.CreateSession())
using (Session b = database.CreateSession())
{
    a.Execute("CREATE TABLE t (id INTEGER PRIMARY KEY, nombre TEXT NOT NULL, saldo INTEGER DEFAULT 0)");
    Check(a.Execute("INSERT INTO t VALUES (1,'O''Brien; ñ',100),(2,'日本語',50)").GetProperty("affected").GetInt64() == 2, "INSERT");
    Check(a.Execute("SELECT nombre FROM t WHERE id=2").GetProperty("rows")[0][0].GetString() == "日本語", "UTF-8");
    a.Execute("CREATE INDEX por_nombre ON t(nombre)");
    Check(a.Execute("EXPLAIN SELECT * FROM t WHERE nombre='日本語'").GetProperty("rows")[0][0].GetString() == "INDEX LOOKUP", "AVL");
    Error(() => a.Execute("INSERT INTO t VALUES (3,'temporal',5),(1,'duplicado',0)"), 4);
    Check(Scalar(a, "SELECT COUNT(*) FROM t") == 2, "INSERT atomico");
    a.Execute("BEGIN");
    a.Execute("UPDATE t SET saldo=saldo-10 WHERE id=1");
    Check(Scalar(a, "SELECT saldo FROM t WHERE id=1") == 90, "Escritura propia");
    Check(Scalar(b, "SELECT saldo FROM t WHERE id=1") == 100, "Aislamiento");
    a.Execute("ROLLBACK");
    Check(Scalar(a, "SELECT saldo FROM t WHERE id=1") == 100, "Rollback");
    a.Execute("BEGIN"); b.Execute("BEGIN");
    a.Execute("UPDATE t SET saldo=90 WHERE id=1");
    b.Execute("UPDATE t SET saldo=60 WHERE id=2");
    a.Execute("COMMIT"); Error(() => b.Execute("COMMIT"), 8); b.Execute("ROLLBACK");
    Check(Scalar(a, "SELECT SUM(saldo) FROM t") == 140, "Conflicto global");
    Error(() => a.Execute("SELECT desconocido FROM t"), 2);
    Check(!JsonDocument.Parse(a.ExecuteJson("SELECT desconocido FROM t")).RootElement.GetProperty("ok").GetBoolean(), "JSON de error");
    a.Execute("DELETE FROM t WHERE id=2");
    Check(Scalar(a, "SELECT COUNT(*) FROM t") == 1, "DELETE");
    a.Execute("BEGIN"); a.Execute("INSERT INTO t VALUES (2,'rollback al cerrar',1)");
}
// SafeHandle keeps the native database alive until its final session is closed.
Database owner = new();
using (Session survivor = owner.CreateSession())
{
    owner.Dispose();
    survivor.Execute("CREATE TABLE vivo (id INTEGER)");
    Check(Scalar(survivor, "SELECT COUNT(*) FROM vivo") == 0, "Vida de la base retenida por sesion");
    try { owner.CreateSession(); throw new Exception("Base cerrada acepto sesion nueva"); }
    catch (ObjectDisposedException) { checks++; }
}
using (Database concurrent = new())
{
    using (Session init = concurrent.CreateSession()) init.Execute("CREATE TABLE paralelo (id INTEGER PRIMARY KEY)");
    Parallel.For(0, 8, i =>
    {
        using Session session = concurrent.CreateSession();
        for (int j = 0; j < 20; j++) session.Execute($"INSERT INTO paralelo VALUES ({i * 20 + j})");
    });
    using Session read = concurrent.CreateSession();
    Check(Scalar(read, "SELECT COUNT(*) FROM paralelo") == 160, "Sesiones concurrentes");
}
string path = Path.Combine(Path.GetTempPath(), "atlasdb-ñ-数据库-" + Guid.NewGuid().ToString("N"));
try
{
    using (Database database = new(path))
    using (Session session = database.CreateSession())
    {
        session.Execute("CREATE TABLE datos (id INTEGER PRIMARY KEY, texto TEXT)");
        session.Execute("INSERT INTO datos VALUES (1,'persistencia ñ 日本語')");
        try { using Database locked = new(path); throw new Exception("Fallo de bloqueo exclusivo"); }
        catch (AtlasException error) { Check(error.Code == 5, "Bloqueo entre aperturas"); }
        session.Execute("CHECKPOINT");
        session.Execute("INSERT INTO datos VALUES (2,'WAL despues de checkpoint')");
        session.Execute("CHECKPOINT"); // Windows must replace an existing snapshot.
        session.Execute("INSERT INTO datos VALUES (3,'recuperacion WAL')");
    }
    using (Database reopened = new(path))
    using (Session session = reopened.CreateSession())
    {
        Check(Scalar(session, "SELECT COUNT(*) FROM datos") == 3, "Recuperacion snapshot mas WAL");
        Check(session.Execute("SELECT texto FROM datos WHERE id=1").GetProperty("rows")[0][0].GetString() == "persistencia ñ 日本語", "Ruta y datos Unicode");
        session.Execute("BEGIN"); session.Execute("DELETE FROM datos");
    }
    using (Database reopened = new(path))
    using (Session session = reopened.CreateSession()) Check(Scalar(session, "SELECT COUNT(*) FROM datos") == 3, "Rollback al cerrar");
    // Simulate an incomplete final WAL record; opening must truncate this tail.
    using (FileStream tail = new(Path.Combine(path, "atlas.wal"), FileMode.Append)) tail.Write([1, 2, 3]);
    using (Database recovered = new(path))
    using (Session session = recovered.CreateSession()) Check(Scalar(session, "SELECT COUNT(*) FROM datos") == 3, "Cola incompleta");
}
finally { if (Directory.Exists(path)) Directory.Delete(path, true); }
var pieces = SqlScript.Split("-- ;\nCREATE TABLE s (v TEXT); INSERT INTO s VALUES ('a;''b'); /* ; */ SELECT * FROM s;");
Check(pieces.Count == 3, "Separacion de scripts");
Check(SqlScript.IsComplete("INSERT INTO s VALUES ('a;b'); -- final"), "Final con comentario");
Check(!SqlScript.IsComplete("SELECT * FROM s; INSERT INTO s VALUES ('sin terminar"), "Sentencia pendiente");
Check(SqlScript.Split("/* solo comentario ; */").Count == 0, "Comentarios vacios");
using (Database database = new())
using (Session session = database.CreateSession())
{
    foreach (string sql in pieces) session.Execute(sql);
    Check(session.Execute("SELECT * FROM s").GetProperty("rows")[0][0].GetString() == "a;'b", "Script y comillas");
    try { session.Execute("SELECT\0"); throw new Exception("NUL aceptado"); }
    catch (ArgumentException) { checks++; }
    try { session.Execute(new string('x', Session.MaxSqlBytes + 1)); throw new Exception("SQL enorme aceptado"); }
    catch (ArgumentException) { checks++; }
    session.Dispose();
    try { session.Execute("SHOW TABLES"); throw new Exception("Sesion cerrada aceptada"); }
    catch (ObjectDisposedException) { checks++; }
}
Console.WriteLine($"AtlasDB Managed: {checks} comprobaciones correctas en {System.Runtime.InteropServices.RuntimeInformation.OSDescription}");
