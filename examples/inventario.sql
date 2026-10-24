-- Demostración reproducible en una base vacía; importes en céntimos.
CREATE TABLE productos (id INTEGER PRIMARY KEY, nombre TEXT NOT NULL, precio_centimos INTEGER NOT NULL, stock INTEGER DEFAULT 0);
INSERT INTO productos VALUES (1,'Teclado',7990,12),(2,'Raton',3550,24);
CREATE INDEX productos_nombre ON productos(nombre);
BEGIN;
UPDATE productos SET stock=stock-1 WHERE id=1 AND stock>0;
COMMIT;
SELECT nombre,precio_centimos*stock AS valor_centimos FROM productos ORDER BY nombre;
SELECT COUNT(*) AS referencias,SUM(precio_centimos*stock) AS total_centimos FROM productos;
EXPLAIN SELECT * FROM productos WHERE nombre='Teclado';
