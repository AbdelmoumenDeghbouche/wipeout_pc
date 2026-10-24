# Compilar y depurar en Windows desde WSL2

El código se edita en WSL2. Una copia dedicada en Windows conserva el build incremental, y el juego usa directamente el driver Vulkan de Windows. No requiere Docker ni Vulkan acelerado dentro de WSL2.

## Preparación

Instala MSYS2 y las dependencias de [compilación Windows](../packaging/windows/README.md), además de `mingw-w64-clang-x86_64-gdb`. Instala Python 3.12 de Windows y ejecuta con ese Python:

```powershell
python -m pip install pillow iced-x86==1.21.0 cryptography
```

El script busca MSYS2 en `%LOCALAPPDATA%\Programs\msys64` y Python en `%LOCALAPPDATA%\Programs\Python\Python312\python.exe`. Si están en otro lugar, usa `--msys2-root` y `--windows-python` con sus rutas Windows.

Desde el repositorio en WSL2:

```bash
git submodule update --init --recursive
python3 scripts/windows_debug.py check
python3 scripts/windows_debug.py build
```

El agente necesita ejecutar estos comandos con permisos ampliados para acceder a la interoperabilidad Windows y escribir en la carpeta Windows. No hace falta desactivar globalmente el sandbox ni ampliar la herencia de variables de entorno.

## Qué se copia y qué se compila

El destino predeterminado es `%LOCALAPPDATA%\wipeout-debug`. Se puede elegir otra carpeta dedicada con `--windows-dir 'C:\dev\wipeout-debug'`. Se rechazan carpetas no vacías que no pertenezcan al script.

Solo se copian archivos versionados, incluidos los cambios sin commit y los submódulos inicializados. Los archivos nuevos deben añadirse con `git add` antes de compilar. Los PKG, credenciales locales, guardados y salidas no versionados no se sincronizan. Los archivos fuente eliminados se retiran de la copia; se conservan las salidas de compilación y `user-debug`.

`build` usa `GOW3_BUILD_TYPE=Debug` y desactiva LTO. Compila el juego y ejecuta las pruebas Windows de metadatos, montajes, JSON y servicios, además de comprobar que GDB deja vivo un proceso de prueba al desconectarse. El build normal conserva `RelWithDebInfo` como valor predeterminado. La biblioteca gráfica mantiene sus definiciones explícitas existentes, incluido `NDEBUG`.

Los ejecutables con símbolos están en la copia Windows, bajo `out/`. No uses el ejecutable de `dist/` para depurar: el empaquetado elimina sus símbolos.

## Ejecutar y depurar

En una terminal WSL2, inicia el juego:

```bash
python3 scripts/windows_debug.py run --game /ruta/al/juego-extraido
```

Cuando esté abierto, adjunta GDB desde otra terminal WSL2:

```bash
python3 scripts/windows_debug.py debug
```

La carpeta del juego debe contener `eboot.bin`; se admite una ruta WSL o una ruta bajo `/mnt/c`. Las rutas WSL se convierten a UNC para Windows y pueden tener menor rendimiento que una carpeta nativa Windows. No se copia automáticamente el juego.

Se usa `user-debug` dentro de la copia Windows para separar estos guardados de los habituales. GDB se adjunta únicamente al proceso cuyo ejecutable corresponde a esa copia Windows; rechaza una selección ausente o ambigua. Usa `bt` para la pila actual, `thread apply all bt` para todos los hilos y `continue` para continuar. Para salir dejando el juego abierto, ejecuta `detach` y después `quit`. El runtime maneja algunas excepciones de memoria e instrucciones como parte de su funcionamiento: una parada del depurador no demuestra por sí sola un fallo fatal.

Se usa GDB porque LLDB 23.1.3 de este entorno termina incluso un proceso mínimo de prueba al ejecutar `process detach`, con estado `0xC0000354`. Su mensaje de desconexión correcta no garantiza que el proceso siga vivo. No vuelvas a usar esa versión para adjuntarte al juego sin verificar primero la supervivencia del proceso de prueba.

Los logs de comprobación, compilación y ejecución quedan en `out/windows-debug/` del checkout WSL. Se reemplazan en cada ejecución del mismo comando; conserva los que necesites antes de repetirlo. La consola interactiva de GDB no se captura en esos archivos. Al terminar el juego, `run.py` imprime su estado nativo de salida, en decimal y hexadecimal, antes de que las capas de Python/PowerShell/WSL puedan reducirlo a otro código.

Los símbolos señalan los archivos de la copia Windows. Su ruta relativa (`src/archivo.c:línea`) corresponde al checkout WSL que se sincronizó. Recompila después de editar; no sincronices por separado mientras depuras, para conservar la correspondencia entre ejecutable y código.

## Comprobar el script sin Windows

```bash
python3 -B -m unittest discover -s tests -p test_windows_debug.py
```
