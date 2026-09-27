# Planificador

Simulador de actividades, modeladas como un DAG.
Ejecuta cada actividad en su propio proceso, respetando dependencias y un
límite de concurrencia `K`.

## Compilación

```
make
```

Compila con `g++ -Wall -Wextra -std=c++17 -lpthread` (ver `Makefile`).

## Uso

```
./planificador plan.txt K
```

- `plan.txt`: archivo con las actividades, formato `ID : Nombre : tiempo_ms : dep1, dep2, ...`
  (tiempo_ms puede omitirse; en ese caso se asigna aleatoriamente entre 100 y 5000 ms).
- `K`: número máximo de procesos corriendo simultáneamente.

Ejemplo:

```
make
./planificador plan.txt 2
```

Para interrumpir la ejecución (simula la inspección de la Seremi) usar `Ctrl+C`:
aborta todas las actividades en curso.

## Funciones implementadas

En `planificador.cpp` estan las las funciones en el orden en que
aparecen:

- **`struct Actividad`**: representa un nodo del DAG (id, nombre, tiempo_ms,
  su lista de dependencias, su lista de dependientes, grado de entrada, y su
  estado: completada/abortada/mensaje de resultado).

- **`trim(string)`**: quita espacios y saltos de línea sobrantes al leer
  cada campo del archivo.

- **`parsear_plan(ruta_archivo)`**: lee `plan.txt` y arma el grafo completo.
  Primera pasada: crea cada `Actividad` con sus datos y su
  `grado_entrada` (cantidad de dependencias). Segunda pasada: recorre todo
  de nuevo para llenar la lista `dependientes`, el archivo solo
  dice "yo dependo de X", pero el planificador necesita la relación
  contraria ("cuando X termine, avisar a quién"). Esto es el modelado del
  DAG en memoria.

- **`manejador_sigint(int)`**: manejador de la señal `SIGINT` (Ctrl+C).
  Solo levanta una bandera `volatile sig_atomic_t`, es lo único
  garantizado seguro de tocar dentro de un signal handler; toda la
  limpieza real se hace después, en `ejecutar_plan`.

- **`ejecutar_actividad(actividad, mensajes_entrada, fd_escritura)`**:
  código que corre el proceso hijo tras el `fork()`. Simula el trabajo con
  `usleep(tiempo_ms)`, tiene una probabilidad de fallar internamente del 5%
  configurable y escribe su mensaje de resultado al pipe antes de
  terminar con `_exit()`.

- **`abortar_rama(id, actividades, restantes)`**: cuando una actividad
  falla, marca como abortados (recorriendo con una cola, de forma
  iterativa para no arriesgar overflow de pila con cargas grandes) a todos
  sus descendientes, sin tocar el resto del DAG.

- **`ejecutar_plan(actividades, K)`**: el núcleo del programa. Instala el
  manejador de `SIGINT`, mantiene una cola de actividades listas (grado de
  entrada 0) y lanza procesos con `fork()` mientras haya cupo
  (`running < K`). Usa `wait()` (bloqueante, sin busy-waiting) para
  esperar a que cualquier hijo termine, lee su mensaje desde el pipe, y
  según el código de salida libera a sus dependientes o llama a
  `abortar_rama`.

- **`main(argc, argv)`**: valida los argumentos de línea de comandos
  (`./planificador plan.txt K`) y llama a `parsear_plan` y `ejecutar_plan`.

## Decisiones de diseño

**Paso de mensajes vía pipes.** Cada actividad crea un pipe antes de
forkearse. El hijo escribe su mensaje de resultado al pipe y termina; el
padre lo lee justo después de reapearlo con `wait()` y lo guarda en memoria
(`Actividad::mensaje`). Cuando el padre lanza a un dependiente, este ya
"recibe" los mensajes de sus dependencias porque `fork()` copia toda la
memoria del padre en ese momento, no hace falta un pipe explícito por cada
arista del grafo. Se eligió este diseño (un pipe por nodo, con el padre como
intermediario) en vez de un pipe por arista porque una actividad puede tener
varios dependientes, y esto evita crear un número de pipes proporcional a
las aristas del DAG, manteniendo el código más simple sin perder aislamiento
entre procesos.

**Aislamiento de errores sin hilos.** Cada actividad corre en un proceso
`fork()`, con memoria completamente separada del padre y de sus
hermanos. Si una actividad falla (código de salida != 0), el padre lo
detecta vía `WEXITSTATUS` y aborta recursivamente (de forma iterativa, para
soportar cargas grandes sin riesgo de overflow de pila) solo la rama que
dependía de ella, sin afectar al resto del plan.

**Sin busy-waiting ni race conditions.** El control de concurrencia usa
`wait()`, que bloquea al proceso padre hasta que cualquier hijo termina, en
vez de hacer polling. No hay condiciones de carrera porque no hay memoria
compartida entre procesos: cada uno tiene su copia privada tras el `fork()`,
y el único proceso que toca la estructura del grafo (`std::map<int,
Actividad>`) es el padre.

**Manejo de SIGINT.** El manejador de señal solo modifica una variable
`volatile sig_atomic_t` (lo único garantizado seguro dentro de un signal
handler). Toda la lógica real de limpieza es mandar `SIGTERM` a los procesos
activos y esperarlos con `waitpid`, se hace en el loop principal, no dentro
del handler.

**Buffering de `std::cout` y `fork()`.** `std::cout` se bufferea por
completo cuando la salida no es una terminal, por ejemplo si el proceso padre tiene
contenido pendiente sin escribir al momento de un `fork()`, el hijo hereda
una copia de ese buffer y puede terminar flusheándolo por su cuenta,
duplicando líneas cuando el padre lo flushea también más tarde. Se usa
`std::cout.setf(std::ios_base::unitbuf)` para forzar un flush automático
después de cada inserción, evitando que quede contenido pendiente en el
buffer al momento de cualquier `fork()`.

## Pruebas

- `plan.txt` de ejemplo del enunciado (6 actividades) con distintos valores
  de `K`.
- Carga de 10000 actividades sin duplicar ni
  perder actividades (verificado: completadas + fallidas + abortadas =
  total), y sin dejar procesos zombis al finalizar.
- `Ctrl+C` durante una ejecución en curso, para validar el aislamiento vía
  `SIGINT`.
