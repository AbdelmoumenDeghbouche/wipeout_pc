# Compilar y depurar en Windows desde WSL2

El código se edita en WSL2. Una copia dedicada en Windows conserva el build incremental, y el juego usa directamente el driver Vulkan de Windows. No requiere Docker ni Vulkan acelerado dentro de WSL2.

## Preparación

Instala MSYS2 y las dependencias de [compilación Windows](../packaging/windows/README.md), además de `mingw-w64-clang-x86_64-lldb`. Instala Python 3.12 de Windows y ejecuta con ese Python:

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

`build` usa `GOW3_BUILD_TYPE=Debug` y desactiva LTO. Compila el juego y ejecuta las pruebas Windows de metadatos, montajes, JSON y servicios. El build normal conserva `RelWithDebInfo` como valor predeterminado. La biblioteca gráfica mantiene sus definiciones explícitas existentes, incluido `NDEBUG`.

Los ejecutables con símbolos están en la copia Windows, bajo `out/`. No uses el ejecutable de `dist/` para depurar: el empaquetado elimina sus símbolos.

## Ejecutar y depurar

En una terminal WSL2, abre LLDB y espera al proceso del juego:

```bash
python3 scripts/windows_debug.py debug
```

En otra terminal WSL2:

```bash
python3 scripts/windows_debug.py run --game /ruta/al/juego-extraido
```

La carpeta del juego debe contener `eboot.bin`; se admite una ruta WSL o una ruta bajo `/mnt/c`. Las rutas WSL se convierten a UNC para Windows y pueden tener menor rendimiento que una carpeta nativa Windows. No se copia automáticamente el juego.

Se usa `user-debug` dentro de la copia Windows para separar estos guardados de los habituales. LLDB se adjunta a `gow3-probe.exe`; tras comprobar los breakpoints, continúa desde su consola. El runtime maneja algunas excepciones de memoria e instrucciones como parte de su funcionamiento: una parada del depurador no demuestra por sí sola un fallo fatal.

Los logs de comprobación, compilación y ejecución quedan en `out/windows-debug/` del checkout WSL. Se reemplazan en cada ejecución del mismo comando; conserva los que necesites antes de repetirlo. La consola interactiva de LLDB no se captura en esos archivos.

Los símbolos señalan los archivos de la copia Windows. Su ruta relativa (`src/archivo.c:línea`) corresponde al checkout WSL que se sincronizó. Recompila después de editar; no sincronices por separado mientras depuras, para conservar la correspondencia entre ejecutable y código.

## Comprobar el script sin Windows

```bash
python3 -B -m unittest discover -s tests -p test_windows_debug.py
```
