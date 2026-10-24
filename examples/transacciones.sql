-- Ejemplo autocontenido; ejecutar en una base vacía.
CREATE TABLE cuentas (
    id INTEGER PRIMARY KEY,
    titular TEXT NOT NULL,
    saldo INTEGER NOT NULL DEFAULT 0
);
INSERT INTO cuentas VALUES (1,'Ana',10000),(2,'Luis',5000);

-- Guardamos céntimos enteros para evitar redondeo binario de importes.
BEGIN TRANSACTION;
UPDATE cuentas SET saldo=saldo-2500 WHERE id=1;
UPDATE cuentas SET saldo=saldo+2500 WHERE id=2;
COMMIT;

SELECT titular,saldo FROM cuentas ORDER BY id;
SELECT SUM(saldo) AS saldo_total FROM cuentas;

-- Una simulación se puede descartar entera.
BEGIN;
UPDATE cuentas SET saldo=0;
SELECT titular,saldo FROM cuentas ORDER BY id;
ROLLBACK;
SELECT titular,saldo FROM cuentas ORDER BY id;
