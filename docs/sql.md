# Referencia SQL

AtlasDB implementa un subconjunto de SQL. Las palabras clave y los nombres de tablas y columnas no distinguen mayúsculas. Las cadenas usan comillas simples y escapan una comilla duplicándola: `'O''Brien'`. Los identificadores pueden escribirse entre comillas dobles para incluir espacios.

Los comentarios son `-- hasta el final de línea` y `/* comentario */`. La API ejecuta exactamente una sentencia; la consola y los ficheros separan sentencias por puntos y coma fuera de comentarios y cadenas.

## Esquema

```sql
CREATE TABLE [IF NOT EXISTS] nombre (
    columna tipo [PRIMARY KEY] [UNIQUE] [NOT NULL] [DEFAULT literal],
    ...
);
DROP TABLE [IF EXISTS] nombre;
SHOW TABLES;
DESCRIBE nombre;

CREATE [UNIQUE] INDEX nombre_indice ON tabla(columna);
DROP INDEX nombre_indice;
```

Tipos: `INTEGER`/`INT`, `REAL`/`FLOAT`, `TEXT`/`VARCHAR`, `BOOLEAN`/`BOOL`. `VARCHAR(n)` acepta la sintaxis, pero el almacenamiento sigue siendo texto con el límite general del motor; no representa una longitud de columna exigida. `DEFAULT` admite un literal, incluido `NULL`, un número con signo o un booleano.

`PRIMARY KEY` combina unicidad y ausencia de nulos. `UNIQUE` permite varias filas con `NULL`. Las restricciones se comprueban sobre la sentencia completa; un error descarta todos sus cambios. Los índices son de una sola columna. Los nombres de índice son identificadores de esquema, por lo que deben evitarse colisiones.

## Escribir filas

```sql
INSERT INTO tabla [(columna,...)] VALUES (expresion,...),(...);
UPDATE tabla SET columna=expresion [,columna=expresion ...] [WHERE expresion];
DELETE FROM tabla [WHERE expresion];
```

Las columnas omitidas en un `INSERT` reciben su valor por defecto o `NULL`. Un `UPDATE` evalúa las asignaciones a partir de la fila anterior. No se convierten cadenas arbitrarias a números; un valor incompatible genera un error de tipo.

Se admite convertir `INTEGER` a `REAL`, y `REAL` a `INTEGER` cuando es exacto y cabe en 64 bits. Los enteros 0 y 1 pueden asignarse a `BOOLEAN`. Las operaciones entre enteros conservan el tipo entero: `5/2` produce 2; `5.0/2` produce 2.5. El desbordamiento numérico y la división por cero producen errores.

## Consultar

```sql
SELECT * | expresion [AS alias] [,expresion [AS alias] ...]
FROM tabla
[WHERE expresion]
[ORDER BY columna [ASC | DESC]]
[LIMIT entero [OFFSET entero]];

EXPLAIN SELECT ...;
```

`ORDER BY` acepta una columna de origen o un alias y un sentido. Los nulos aparecen al principio con `ASC` y al final con `DESC`. `LIMIT 0` conserva la descripción de columnas y devuelve cero filas. `EXPLAIN` muestra el plan escogido, incluido el uso de un índice cuando corresponde. La optimización básica reconoce igualdades directas entre columna y literal, también dentro de condiciones `AND`.

Operadores, por precedencia general:

1. Paréntesis y signo unario.
2. Multiplicación, división y resto: `*`, `/`, `%`.
3. Suma y resta: `+`, `-`.
4. Comparaciones: `=`, `!=`, `<>`, `<`, `<=`, `>`, `>=`, `LIKE`, `NOT LIKE`, `IS NULL`, `IS NOT NULL`.
5. `NOT`, `AND`, `OR`.

Las comparaciones ordinarias con `NULL` producen un valor desconocido; para buscar nulos utiliza `IS NULL`. `WHERE` conserva solo las filas cuya condición es verdadera. `LIKE` distingue mayúsculas, reconoce `%` como secuencia de bytes y `_` como un byte. No implementa escapes de patrones.

Las funciones escalares son `ABS(numero)`, `LENGTH(texto)`, `LOWER(texto)` y `UPPER(texto)`. El SQL y los valores de texto deben contener UTF-8 válido. `LENGTH` cuenta bytes; las conversiones de mayúsculas y minúsculas trabajan por bytes, con comportamiento ASCII en el locale `C`. No hay normalización Unicode ni reglas de ordenación lingüística.

Los nombres de columnas se validan antes de recorrer filas. Los errores de cálculo y de tipos de las expresiones se detectan al evaluarlas. Un índice, un conjunto vacío o un cortocircuito booleano pueden evitar evaluar una expresión en filas descartadas; no debe dependerse del orden de aparición de esos errores.

Los agregados globales son:

| Expresión | Resultado |
|---|---|
| `COUNT(*)` | Número de filas seleccionadas |
| `COUNT(expresion)` | Número de valores no nulos |
| `SUM(expresion)` | Suma de los valores no nulos |
| `AVG(expresion)` | Media de los valores no nulos |
| `MIN(expresion)` | Menor valor no nulo |
| `MAX(expresion)` | Mayor valor no nulo |

Sobre un conjunto vacío, `COUNT` devuelve cero y los otros agregados devuelven `NULL`. Una consulta agregada no puede mezclar columnas de fila sueltas con agregados. No hay agrupación por columnas.

## Transacciones

```sql
BEGIN;                 -- también BEGIN TRANSACTION
UPDATE cuentas SET saldo=saldo-10 WHERE id=1;
UPDATE cuentas SET saldo=saldo+10 WHERE id=2;
COMMIT;                -- o ROLLBACK
CHECKPOINT;
```

Fuera de una transacción explícita, cada escritura se confirma por separado. `BEGIN` crea una instantánea de la base. Las lecturas posteriores de esa sesión ven la instantánea y sus propios cambios.

Si otra sesión confirma una escritura mientras la transacción estaba abierta, una transacción con cambios recibe `ADB_ERR_CONFLICT` al intentar confirmar. La transacción sigue abierta para inspeccionarla o deshacerla. Ejecuta `ROLLBACK` y vuelve a iniciar la operación con datos actuales. Esta comprobación es global: incluso dos escritores que modifican tablas distintas pueden entrar en conflicto. Una transacción de solo lectura puede confirmar tras cambios concurrentes.

Un error de sentencia conserva los cambios anteriores de la transacción. Cerrar una sesión descarta sus cambios pendientes. No hay transacciones anidadas ni puntos de recuperación.

## Límites

Los límites de compilación incluyen 1 MiB por sentencia SQL, 1 MiB por valor de texto, 255 bytes por identificador SQL, 128 columnas por tabla, 1.024 tablas y un millón de filas por tabla. Los resultados tienen un presupuesto de 64 MiB para celdas y textos propios; su serialización JSON requiere memoria adicional. El formato persistente también impone un máximo de 256 MiB por imagen serializada. Estos límites son barreras de recursos; el uso práctico depende de la memoria disponible y del coste de copiar el estado completo.

No se implementan `ALTER TABLE`, `JOIN`, `GROUP BY`, `HAVING`, `DISTINCT`, `UNION`, subconsultas, parámetros preparados ni claves foráneas.
