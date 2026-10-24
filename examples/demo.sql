-- Ejecutar: ./build/atlasdb --file examples/demo.sql
CREATE TABLE productos (
    id INTEGER PRIMARY KEY,
    nombre TEXT NOT NULL,
    categoria TEXT,
    precio REAL NOT NULL,
    stock INTEGER DEFAULT 0,
    activo BOOLEAN DEFAULT TRUE
);

INSERT INTO productos (id,nombre,categoria,precio,stock) VALUES
    (1,'Teclado mecánico','Periféricos',89.90,12),
    (2,'Ratón ergonómico','Periféricos',39.50,24),
    (3,'Monitor 27 pulgadas','Pantallas',249.00,8),
    (4,'Adaptador USB-C',NULL,19.95,50),
    (5,'Soporte de portátil','Accesorios',34.00,0);

CREATE INDEX productos_categoria ON productos(categoria);
SHOW TABLES;
DESCRIBE productos;

SELECT nombre,precio,stock,precio*stock AS valor_inventario
FROM productos WHERE activo=TRUE ORDER BY nombre;

EXPLAIN SELECT nombre FROM productos WHERE categoria='Periféricos';
SELECT nombre FROM productos WHERE categoria='Periféricos' ORDER BY nombre;
SELECT nombre FROM productos WHERE categoria IS NULL;

BEGIN;
UPDATE productos SET stock=stock-2 WHERE id=1;
UPDATE productos SET precio=precio*0.90 WHERE categoria='Periféricos';
COMMIT;

SELECT COUNT(*) AS productos,SUM(precio*stock) AS valor_total,AVG(precio) AS precio_medio
FROM productos;

BEGIN;
DELETE FROM productos WHERE stock=0;
SELECT COUNT(*) AS dentro_de_transaccion FROM productos;
ROLLBACK;

SELECT nombre,stock FROM productos ORDER BY stock DESC LIMIT 3;
CHECKPOINT;
