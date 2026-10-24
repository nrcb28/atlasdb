# Preparar la publicación en GitHub

Este paquete contiene código MIT, ejemplos, documentación y la configuración de CI original. La verificación local no implica que GitHub Actions se haya ejecutado.

## Publicación desde una carpeta descomprimida

Crear un repositorio vacío en GitHub y copiar su URL real. Desde la raíz del proyecto:

```sh
make check
make sanitize
git init -b main
git add .
git diff --cached --stat
git commit -m "Publicar AtlasDB con documentación y ejemplos"
# Sustituir la URL de ejemplo por la del repositorio propio:
git remote add origin https://github.com/TU_USUARIO/TU_REPOSITORIO.git
git push -u origin main
```

Antes del commit revisar autoría de LICENSE, ausencia de datos privados y archivos añadidos. Conservar la licencia existente y atribuciones. Los logs de verificación están en docs/evidencias; no incluyen bases de usuario. No incluir build/, directorios de datos ni secretos. Usar la pestaña Actions para confirmar el resultado remoto; nunca presentar las pruebas locales como ejecución remota.

## Presentación sugerida

Descripción: «Motor SQL educativo en C17: API embebida, CLI, TCP, índices AVL, transacciones y WAL con recuperación».

Temas: c, database, sql, avl-tree, transactions, write-ahead-log, educational, linux.

El nombre AtlasDB también lo usan otros proyectos: explicar claramente el alcance y evitar sugerir afiliaciones. Para etiquetar una versión, decidir primero la versión propia: este paquete parte de 1.0.0 y añade documentación y ejemplos; no constituye una release publicada.

## Comprobar antes de compartir

- El README enlaza la guía de usos, el informe de validación y los ejemplos.
- Los comandos funcionan desde la raíz en Linux con compilador C17, make y Python 3.
- El servidor escucha en loopback; los límites y las funciones no implementadas son visibles.
- El PDF se presenta como resumen técnico del proyecto, sin afirmaciones de escala o producción.
- Añadir la URL real del repositorio a la publicación de LinkedIn cuando exista.

Para generar el paquete original: make package. Para una release pública, adjuntar fuentes y SHA256, describir entorno y resultados, y revisar la CI antes de anunciarla.
